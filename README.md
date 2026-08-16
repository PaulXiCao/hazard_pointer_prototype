# hazptr_proto

Prototype implementation of C++26 `std::hazard_pointer` for learning and algorithm validation.
API mirrors `<hazard_pointer>` (`hazard_pointer_obj_base`, `hazard_pointer`, `make_hazard_pointer`)
to keep the prototype directly portable to libstdc++. `hazard_pointer_clean_up()` is not in the
C++26 standard; it lives in `proto::detail` as a test helper only.

## Purpose

1. Learn the algorithm without GCC build system friction
2. Validate correctness under TSan/ASan before touching libstdc++
3. Tests serve as a specification

## Build

Requires CMake 3.21+, Ninja, C++26 compiler. CPM is vendored in `cmake/CPM.cmake`; the first configure fetches GTest through it -- requires internet once, then cached under `.cache/CPM`.

**Linux / macOS (bash)**
```bash
# PRESET: dev | release | ci | tsan | asan  (tsan/asan: Linux/macOS only)
PRESET=dev && cmake --preset $PRESET && cmake --build --preset $PRESET && ctest --preset $PRESET
```

**Windows -- VS Developer PowerShell (PS 7+)**
```powershell
# preset: dev | release | ci  (tsan/asan not available on Windows)
$preset = "dev"; cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset
```

```bash
# Run only compile-fail tests (verbose -- shows compiler error output)
ctest --preset dev -R "^cf_" -V
```

**Nix / NixOS**

```bash
nix develop        # clang 21 (CI parity), gcc 14, cmake, ninja, herd7, setarch
```

`flake.nix` pins nixpkgs to the stable channel so the compiler does not move
under a prototype whose purpose is reproducing memory-ordering behaviour.
`nix develop` also provides `herd7`, so `tools/litmus/run.sh` works without an
opam switch.

**Sanitizers and ASLR.** On kernels that randomize more address bits than
TSan/ASan support, every sanitized binary dies at startup with
`FATAL: ThreadSanitizer: unexpected memory mapping` -- all tests "fail" while
reporting no races at all. Run them with randomization disabled:

```bash
cmake --preset tsan && cmake --build --preset tsan
setarch "$(uname -m)" -R ctest --preset tsan
```

`setarch` needs no root. The system-wide alternative is
`sysctl vm.mmap_rnd_bits=28` (on NixOS: `boot.kernel.sysctl."vm.mmap_rnd_bits" = 28`).

## Files

```
hazard_ptr.hpp              # single-header prototype
PORTING_NOTES.md            # libstdc++ porting checklist
CMakePresets.json           # dev / release / tsan / asan / ci presets
tests/
  # One file per API function
  test_hazard_pointer_ctor.cpp   # default ctor, move ctor, destructor
  test_empty.cpp                 # hazard_pointer::empty()
  test_make_hazard_pointer.cpp   # make_hazard_pointer(), record pool grow/reuse
  test_protect.cpp               # protect()
  test_try_protect.cpp           # try_protect()
  test_reset_protection.cpp      # reset_protection() (both overloads)
  test_swap.cpp                  # swap() member + free swap()
  test_move_assign.cpp           # operator=(&&)
  test_retire.cpp                # retire() + synchronize() reclamation
  test_custom_deleter.cpp        # retire() with custom deleter D
  # Compile-time property tests
  test_noexcept.cpp              # static_assert(noexcept(...)) for all noexcept APIs
  test_type_constraints.cpp      # HazardProtectable concept, retire() static_asserts
  # Contract precondition death tests
  test_contracts.cpp             # pre() violations; guarded by #ifdef __cpp_contracts
  # Concurrency
  test_concurrent.cpp            # readers/writers under TSan
  test_thread_exit.cpp           # thread-exit drains list; survivors -> orphan list
  compile_fail/                  # cf_*.cpp: expected-to-fail compilation tests
```

## Architecture

Public API in namespace `proto`; internals in `proto::detail`.

### Public API

- **`hazard_pointer_obj_base<T, D>`** -- CRTP base. `retire()` splices `this` onto the calling thread's retire list for deferred deletion. `retire()` is `noexcept` per [saferecl.hp.base] and, because the list is intrusive, genuinely cannot allocate.
- **`hazard_pointer`** -- RAII handle over one `HazptrRec`, one word wide. `protect(src)` / `try_protect(ptr, src)` / `reset_protection()`.
- **`make_hazard_pointer()`** -- acquires one record from the default domain. The only function here that allocates, which is what [saferecl.hp.make]/3 allows it to do.

### Internals (`proto::detail`)

- **`HazptrObj`** -- non-template private base of `hazard_pointer_obj_base<T,D>`. Holds the intrusive retire link, the type-erased reclaim function, and the space P2530R3 sec. 1.5 reserves for cohorts and integrated counting. Also serves as the tag `HazardProtectable` detects. `next == this` means "not retired", which backs the double-retire precondition at zero cost.
- **`RetireList`** -- intrusive singly-linked list of `HazptrObj` with head/tail/size, so concatenation in `synchronize()` is O(1) and allocation-free.
- **`HazptrRec`** -- one hazard pointer record, cache-line padded: the hazard value, an `active` flag claimed by CAS, the list link, and the domain pointer P2530R3 sec. 1.5 item 3 reserves. Records are created on demand and never unlinked, so a live handle's address stays valid and the scan needs no lock.
- **`HazardDomain`** -- singleton owning the append-only record list and the per-thread retire list registry.
- **`RetireListNode`** -- per-thread node holding that thread's `RetireList`, registered lazily into a global linked list. On thread exit `~RetireListNode()` drains the list, offloads still-protected survivors to `HazardDomain::orphan_list_`, then unregisters.
- **`tl_node_`** -- one `thread_local RetireListNode` per thread across all TUs.

### Lock ordering

`HazardDomain` has three mutexes. Only one nesting case exists: in `synchronize()` step 1, `retire_lists_mutex_` (outer) is held while each per-thread `list_mutex` (inner) is briefly acquired to detach that thread's retire list. All other mutexes are acquired in isolation. Neither claiming nor releasing a hazard pointer record takes a lock at all.

| Mutex | Guards |
|---|---|
| `retire_lists_mutex_` | `retire_lists_head_`, node `next` pointers, `retire_list_node_count_` |
| `list_mutex` (per-thread, in `RetireListNode`) | the contents of `RetireListNode::list` |
| `rec_alloc_mutex_` | appends to the record list only; claiming and releasing a record are lock-free |
| `orphan_mutex_` | `orphan_list_` |

## Algorithm reference

### `try_protect()` -- single attempt

```
store ptr -> rec->hazard (seq_cst)
reload src (seq_cst)
if changed: clear hazard, update ptr, return false
else: return true
```

The `seq_cst` store drains the store buffer before the reload, so either the synchronize snapshot sees the hazard or the reload sees the new pointer. A `release` store would not do: it can sit in the store buffer while the snapshot's load reads null.

This handles the **reader** side only. It is not on its own sufficient to prevent use-after-free -- the reclaim side additionally needs a `seq_cst` fence before its scan, because an acquire (or even seq_cst) scan load does not join the total order this pair relies on. See the fence row in [Memory Ordering](#memory-ordering) and `tools/litmus/`.

### `protect()` -- retry loop

```
loop: load src (relaxed) -> try_protect -> until success
```

### `synchronize()` -- scan + reclaim

```
1. collect: foreach thread -- detach retire list under list_mutex (O(1) splice)
           + drain orphan_list_ (objects from exited threads)
2. atomic_thread_fence(seq_cst)          <-- mandatory, see Memory Ordering
3. walk the record list -> protected set (acquire loads, no lock)
4. reclaim: reclaim objects not in protected set (no lock held -- deleters may call retire())
5. survivors -> calling thread's own list
```

Collect-then-snapshot, not snapshot-then-collect: every writer's retirement push happens under `list_mutex`, so the collect step's mutex acquires HB-after each writer's seq_cst exchange. When the snapshot's acquire-loads then run, the calling thread's vector clock already carries every writer's exchange clock. Reversing the order leaves the snapshot ahead of those mutex syncs, and TSan reports false-positive races in multi-writer/multi-reader workloads: the cross-atomic SC argument (seq_cst hazard store <-> seq_cst src reload <-> seq_cst exchange) is beyond what TSan's vector-clock model tracks.

Survivors go to the calling thread because other threads may have exited between steps 1 and 4.

### `retire_impl()` -- threshold + auto-sync

```
push to tl_retire_list_ (under list_mutex)
if size > 2 * active_count_: synchronize()
```

### Thread-local registry

Each thread lazily registers its `RetireListNode` into a global singly-linked list on first `retire_impl()` or `synchronize()` call. `RetireListNode` and its retire list live in a single `thread_local ThreadState` so that member-destruction order guarantees the vector outlives the node.

On thread exit, `~RetireListNode()`:
1. Calls `synchronize()` to reclaim as much as possible.
2. Splices the node out under `retire_lists_mutex_`. Doing this before step 3 is essential: `retire_lists_mutex_` is held by `synchronize()` for its entire collect loop, so this call blocks until any concurrent `synchronize()` that already has a pointer to this node has released both `retire_lists_mutex_` and `list_mutex`. After it returns, the node is invisible to all future `synchronize()` calls and the retire list is exclusively owned by the exiting thread.
3. Moves any still-protected survivors to `HazardDomain::orphan_list_` (collected by the next `synchronize()` from any thread). Safe to do without `list_mutex` because step 2 guarantees no concurrent access.

## Memory ordering rationale

| Operation | Ordering | Reason |
|---|---|---|
| `rec_->hazard.store(p)` in `reset_protection(T*)` | `seq_cst` | P2530R3; drains store buffer so snapshot cannot miss the hazard |
| `src.load()` re-check in `try_protect()` | `seq_cst` | P2530R3; SC pair with hazard store -- either snapshot sees hazard OR re-check sees new pointer |
| `rec_->hazard.store(nullptr)` in `reset_protection()` | `release` | clearing hazard; no reclamation depends on observing the clear promptly |
| `atomic_thread_fence` before the scan in `synchronize()` | `seq_cst` | **mandatory.** An acquire scan does not join the seq_cst total order, and the collect step's lock chain orders only a *writer's* retirement -- not an independent reader's hazard store. Without it: use-after-free on weak-memory targets. Upgrading the scan loads to seq_cst instead is **not** sufficient, because the removal store on `src` is user code and need not be seq_cst. See [tools/litmus](tools/litmus/) |
| scan `s.load()` in `synchronize()` | `acquire` | sufficient *given the fence above* |
| `active_count_` read in `retire_impl()` | `relaxed` | heuristic threshold; stale value acceptable |

The fence was added after review by Thomas Rodgers (confirmed with Maged
Michael), who observed the reordering on POWER9/POWER10 hardware. Same shape as
Folly's `do_reclamation()`. `tools/litmus/` locks the argument into CI:
`hazptr-acquire-scan` → `Sometimes`, `hazptr-seqcst-loads` → `Sometimes`,
`hazptr-fence` → `Never`.

## Reference

- Maged Michael, "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects", IEEE TPDS 2004
- P2530R3 -- C++26 standard paper
- Folly `Hazptr.h` (`folly/synchronization/Hazptr.h`) -- production reference (different API, same algorithm)