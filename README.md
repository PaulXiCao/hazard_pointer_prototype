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

Requires CMake 3.21+, Ninja, C++26 compiler. First configure downloads CPM and fetches GTest -- requires internet.

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

## Files

```
hazard_ptr.hpp              # single-header prototype
PORTING_NOTES.md            # libstdc++ porting checklist
CMakePresets.json           # dev / release / tsan / asan / ci presets
tests/
  # One file per API function
  test_hazard_pointer_ctor.cpp   # default ctor, move ctor, destructor
  test_empty.cpp                 # hazard_pointer::empty()
  test_make_hazard_pointer.cpp   # make_hazard_pointer(), slot pool grow/reuse
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

- **`hazard_pointer_obj_base<T, D>`** -- CRTP base. `retire()` enqueues `this` for deferred deletion.
- **`hazard_pointer`** -- RAII slot handle. `protect(src)` / `try_protect(ptr, src)` / `reset_protection()`.
- **`make_hazard_pointer()`** -- acquires one slot from the default domain.

### Internals (`proto::detail`)

- **`HazardDomain`** -- singleton owning a growable hazard slot pool (starts at `kInitialSlots=8`, grows on demand) and the per-thread retire list registry.
- **`RetireListNode`** -- per-thread node registered lazily into a global linked list. On thread exit `~RetireListNode()` drains the list, offloads still-protected survivors to `HazardDomain::orphan_list_`, then unregisters.
- **`ThreadState`** -- bundles `retire_list` and `node` into one `thread_local` so that C++ member-destruction order guarantees the vector outlives the node.
- **`tl_state_`** -- one `thread_local ThreadState` per thread across all TUs.

### Lock ordering

`HazardDomain` has four mutexes. Only one nesting case exists: in `synchronize()` step 1, `retire_lists_mutex_` (outer) is held while each per-thread `list_mutex` (inner) is briefly acquired to swap that thread's retire list. All other mutexes are acquired in isolation.

| Mutex | Guards |
|---|---|
| `retire_lists_mutex_` | `retire_lists_head_`, node `next` pointers, `retire_list_node_count_` |
| `list_mutex` (per-thread, in `RetireListNode`) | the contents of `*RetireListNode::list` |
| `slot_free_mutex_` | `slot_free_[]`, `slots_[]` structure |
| `orphan_mutex_` | `orphan_list_` |

## Algorithm reference

### `try_protect()` -- single attempt

```
store ptr -> slot (seq_cst)
reload src (seq_cst)
if changed: clear slot, update ptr, return false
else: return true
```

The `seq_cst` store drains the store buffer before the reload. This ensures either the synchronize snapshot sees the hazard, or the reload sees the new pointer -- preventing use-after-free on weak-memory architectures and x86 (where a `release` store can sit in the store buffer while the snapshot's `acquire`-load reads null).

### `protect()` -- retry loop

```
loop: load src (relaxed) -> try_protect -> until success
```

### `synchronize()` -- scan + reclaim

```
1. collect: foreach thread -- swap retire list out under list_mutex
           + drain orphan_list_ (records from exited threads)
2. snapshot all active slots -> protected set (acquire loads)
3. reclaim: delete records not in protected set (no lock held -- deleters may call retire())
4. survivors -> calling thread's own list
```

Collect-then-snapshot, not snapshot-then-collect: every writer's retirement push happens under `list_mutex`, so the collect step's mutex acquires HB-after each writer's seq_cst exchange. When the snapshot's acquire-loads then run, the calling thread's vector clock already carries every writer's exchange clock. Reversing the order leaves the snapshot ahead of those mutex syncs, and TSan reports false-positive races in multi-writer/multi-reader workloads: the cross-atomic SC argument (seq_cst slot store <-> seq_cst src reload <-> seq_cst exchange) is beyond what TSan's vector-clock model tracks.

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
| `slot_->store(p)` in `reset_protection(T*)` | `seq_cst` | P2530R3; drains store buffer so snapshot cannot miss the hazard |
| `src.load()` re-check in `try_protect()` | `seq_cst` | P2530R3; SC pair with hazard store -- either snapshot sees hazard OR re-check sees new pointer |
| `slot_->store(nullptr)` in `reset_protection()` | `release` | clearing hazard; no reclamation depends on observing the clear promptly |
| scan `s.load()` in `synchronize()` | `acquire` | pairs with seq_cst store in `reset_protection(T*)` |
| `active_count_` read in `retire_impl()` | `relaxed` | heuristic threshold; stale value acceptable |

## Reference

- Maged Michael, "Hazard Pointers: Safe Memory Reclamation for Lock-Free Objects", IEEE TPDS 2004
- P2530R3 -- C++26 standard paper
- Folly `Hazptr.h` (`folly/synchronization/Hazptr.h`) -- production reference (different API, same algorithm)