#!/bin/bash
# Build and collect a fixed workload. All artifacts survive failures.
# run.sh <clean CoreMark checkout> [EM_BUILD] [iterations] [collector options...]
# COREMARK_OUT selects a fresh workload directory; otherwise mktemp retains it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SOURCE="${1:?usage: run.sh <CoreMark checkout> [EM_BUILD] [iterations] [options]}"
EM="${2:-$ROOT/build-em}"
ITER="${3:-4000}"
if [ -n "${ARM_JIT:-}" ]; then
    echo 'Use EM_BUILD to select a configured build; ARM_JIT is no longer supported.' >&2
    exit 2
fi
OUT="${COREMARK_OUT:-$(mktemp -d)}"
echo "CoreMark artifacts: $OUT"
python3 "$HERE/build.py" "$SOURCE" "$OUT" --iterations "$ITER"
shift "$(( $# < 3 ? $# : 3 ))"
node "$HERE/run.mjs" "$EM/examples/browser" "$OUT" --output "$OUT/report.json" "$@"
