# Weak-memory-model checks

Three tiny programs that pin down the reclaim-side memory-ordering bug reported
by Thomas Rodgers (confirmed with Maged Michael),
[libstdc++/2026-July/067282](https://gcc.gnu.org/pipermail/libstdc++/2026-July/067282.html).

```bash
opam install herdtools7      # not packaged in nixpkgs
bash tools/litmus/run.sh
```

| File | Reclaim side (P1) | Expected |
|---|---|---|
| `hazptr-acquire-scan.litmus` | release removal store, **acquire** scan — as written | `Sometimes` |
| `hazptr-seqcst-loads.litmus` | release removal store, **seq_cst** scan, no fence | `Sometimes` |
| `hazptr-fence.litmus` | release removal store, **seq_cst fence**, acquire scan | `Never` |

All three are the same four accesses with the same `exists` clause; only P1's
ordering changes.

## What herd7 does

`herd7` takes one of these files and works out *every* execution the C11 memory
model permits, then answers one question: is the `exists` clause reachable?
`Sometimes` means yes, `Never` means no. It is arithmetic over a formal model —
not a test run, so there is no flakiness and no "ran it a billion times and saw
nothing".

The `exists` clause here spells out the use-after-free:

```
exists (0:rsrc=1 /\ 1:rhaz=0)
```

- `0:rsrc=1` — the reader re-read `src`, still saw `O`, so `try_protect`
  returned true and it keeps dereferencing `O`.
- `1:rhaz=0` — the reclaimer read an empty hazard slot, decided `O` is
  unprotected, and freed it.

## What these files do *not* prove

**They are hand-written abstractions of `hazard_ptr.hpp`, not the code itself.**
herd7's answer is about the eight lines in the file. If the abstraction is
unfaithful, the answer is about the wrong program. Two consequences worth being
explicit about:

**1. The load-bearing assumption is the missing locks.** `synchronize()` really
holds `retire_lists_mutex_` and each `list_mutex` during its collect step; P1
here takes no locks at all. That omission is justified by Rodgers' argument that
the lock chain orders the *writer's* retirement against the collect, while an
independent reader on a third thread never touches those mutexes and so gains no
ordering from them. **That argument is human reasoning. herd7 does not check
it.** If it is wrong, all three verdicts describe the wrong program.

**2. `Never` is weaker than it looks.** Dropping accesses makes a program *more*
permissive, so:

- `Sometimes` on the broken shapes is not, by itself, strong evidence the real
  code is broken — an abstraction with fewer ordering edges can allow outcomes
  the real code forbids. What makes it convincing is external: Rodgers observed
  the reordering on POWER9/POWER10 hardware, up to 1.4M of 959M runs.
- `Never` on the fixed shape does **not** transfer to the real code. It says the
  fence fixes *this* shape. Every place the implementation frees memory needs the
  same argument made separately — `synchronize()`, the orphan-list drain, and the
  thread-exit path. herd7 cannot tell you that one was missed.

So treat this directory as a lock on the *reasoning*, not as verification of the
implementation. Its strongest claim is the **difference** between the files:
`Sometimes → Never` when nothing changes but the fence isolates the fence as the
cause, and that holds up even if the abstraction is imperfect in other ways.

## Hardware runs

`litmus7` compiles these same files into real binaries and samples executions on
the host. Note the output directory must exist first, and C tests need
`-c11 true`:

```bash
mkdir -p /tmp/lit
nix develop --command bash -c '
  litmus7 -c11 true -o /tmp/lit tools/litmus/*.litmus
  make -C /tmp/lit -j"$(nproc)"
  cd /tmp/lit && sh run.sh'
```

Measured on an x86_64 dev machine (10⁶ runs each):

| File | Positive outcomes |
|---|---|
| `hazptr-acquire-scan` | 1 |
| `hazptr-seqcst-loads` | 127 |
| `hazptr-fence` | **0** |

**x86 does not hide this shape**, contrary to what one might assume from
"x86 is TSO". TSO permits precisely the StoreLoad reordering this needs, and
P1's release store plus acquire load compile to plain `mov`s with nothing
between them. When Rodgers says x86 hides it, that is about the *real* code,
where the reclaim path's mutex locked-RMWs sit between the removal store and the
scan — a different program from this abstraction, and a reminder that the
abstraction is not the implementation in *both* directions.

A sampled run can only ever supply weak positive evidence; zero positives never
clears the fix, which is why the hardware job is non-gating and `run.sh`
(herd7) is the gate.

## Keeping it honest

- Every access in every file carries a comment naming the `hazard_ptr.hpp` line
  it models. Keep those current when `synchronize()` or `try_protect()` moves —
  a stale mapping is how a suite like this quietly stops meaning anything.
- Rodgers offered to post his own litmus tests. Take him up on it: an
  independent transcription of the same code is the best available check on the
  abstraction, since two people rarely make the same modeling mistake.
- The dynamic complement is the concurrent stress test with poisoning (review
  finding R4). It runs the real header, so it can catch a translation mistake —
  it can never prove the bug is absent.

## Line references

Against `hazard_ptr.hpp` at commit `f36ed61`:

| Access | Line | Code |
|---|---|---|
| reader's hazard store | 407 | `slot_->store(ptr, seq_cst)` in `reset_protection(const T*)` |
| reader's re-validate | 385 | `ptr = src.load(seq_cst)` in `try_protect` |
| reclaimer's scan load | 564 | `sp->load(acquire)` in `synchronize()` step 2 |
| fence insertion point | 562 | between the collect step and the scan loop |

The removal store on `src` is **user code**, not part of the header. P2530R3 does
not require it to be `seq_cst`, so `release` is the weakest a conforming user may
choose — which is what the files model.
