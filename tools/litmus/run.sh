#!/usr/bin/env bash
# Check the reclaim-side memory-ordering argument against the C11 abstract
# machine.  Deterministic: herd7 enumerates every execution the model allows,
# so a verdict is a proof about the model, not a sampled test result.
#
# Exits nonzero if any verdict differs from the expected one.  The important
# case is hazptr-fence.litmus flipping away from Never -- that means the fence
# in synchronize() was removed or weakened.
#
# Usage:  bash tools/litmus/run.sh
# Needs:  herd7 on PATH   (opam install herdtools7)

set -uo pipefail

cd "$(dirname "$0")"

MODEL=${MODEL:-rc11.cat}

# file                       expected verdict
TESTS=(
  "hazptr-acquire-scan.litmus  Sometimes"
  "hazptr-seqcst-loads.litmus  Sometimes"
  "hazptr-fence.litmus         Never"
)

if ! command -v herd7 >/dev/null; then
  echo "error: herd7 not found on PATH.  Install with: opam install herdtools7" >&2
  exit 127
fi

echo "herd7: $(herd7 -version 2>&1 | head -1)"
echo "model: $MODEL"
echo

rc=0
for t in "${TESTS[@]}"; do
  read -r file want <<<"$t"

  out=$(herd7 -model "$MODEL" "$file" 2>&1)
  if [ $? -ne 0 ]; then
    printf '%-30s ERROR (herd7 failed)\n' "$file"
    echo "$out" | sed 's/^/    /'
    rc=1
    continue
  fi

  # "Observation <name> Sometimes 1 3" / "Observation <name> Never 0 4"
  got=$(awk '/^Observation/ {print $3; exit}' <<<"$out")

  if [ "$got" = "$want" ]; then
    printf '%-30s %-9s as expected\n' "$file" "$got"
  else
    printf '%-30s %-9s MISMATCH -- expected %s\n' "$file" "${got:-<none>}" "$want"
    echo "$out" | sed 's/^/    /'
    rc=1
  fi
done

echo
if [ $rc -eq 0 ]; then
  echo "all verdicts as expected"
else
  echo "FAILED: a verdict changed -- see above" >&2
  echo "If hazptr-fence.litmus is no longer Never, the seq_cst fence in" >&2
  echo "synchronize() is missing or weakened." >&2
fi
exit $rc
