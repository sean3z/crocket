#!/usr/bin/env bash
# One bounded fuzz run for .github/workflows/fuzz.yml.
# Does not pass --save: new coverage stays in the process and is not written
# into tests/fuzz/corpus. A crash or hang is a file under build-fuzz/crashes/.
set -euo pipefail

seconds=${FUZZ_SECONDS:-45}
target=${FUZZ_TARGET:-all}
seed=${FUZZ_SEED:-1}

case $target in
  all|json|json_typed|proto|query|route|request|h2|jwt) ;;
  *) echo "crocket fuzz: target must be all, json, json_typed, proto, query, route, request, h2, or jwt" >&2
     exit 2 ;;
esac
if ! [[ $seconds =~ ^[0-9]+$ ]] || ((seconds < 1 || seconds > 90)); then
  echo "crocket fuzz: seconds must be a whole number from 1 to 90 (got '$seconds')" >&2
  exit 2
fi
if ! [[ $seed =~ ^[0-9]+$ ]] || ((seed < 1)); then
  echo "crocket fuzz: seed must be a positive whole number" >&2
  exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
crashes=$root/build-fuzz/crashes
mkdir -p "$crashes"
log=$crashes/fuzz.log
run=$crashes/run.txt
{
  echo "target $target"
  echo "seconds_per_target $seconds"
  echo "seed $seed"
  echo "save no"
} >"$run"

# abort_on_error so a sanitizer failure is a crash the harness records.
# The current input is a global, so it is not itself reported as a leak.
# A leak inside a target still fails the run.
export ASAN_OPTIONS=${ASAN_OPTIONS:-abort_on_error=1:halt_on_error=1}

set +e
set -o pipefail
./dev fuzz "$target" --seconds "$seconds" --seed "$seed" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
set +o pipefail
set -e
echo "exit $rc" >>"$run"
if ((rc != 0)); then
  echo "crocket fuzz: failed (exit $rc). Inputs, seed, and log are in build-fuzz/crashes/." >&2
  echo "Replay one file with: ./dev fuzz $target --replay build-fuzz/crashes/<file>" >&2
fi
exit "$rc"
