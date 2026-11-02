#!/bin/sh
set -eu

# Usage: tests/install/smoke.sh <configured-build> [prefix]
BUILD="$(cd "$1" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${2:-$(mktemp -d)}"
CONSUMER="$ROOT/tests/install/consumer"
OUT="$BUILD/install-smoke"

# CTest omits a prefix so every invocation starts from an empty installation;
# leftovers must not hide a removed export, header or license. Keep that fresh
# prefix on failure for investigation, and leave explicit user prefixes alone.
cleanup() {
    status=$?
    if [ "$status" -eq 0 ]; then
        rm -rf "$PREFIX"
    else
        echo "Install consumer failure prefix: $PREFIX" >&2
    fi
}
if [ "$#" -eq 1 ]; then
    trap cleanup EXIT
fi

rm -rf "$OUT"

cmake --install "$BUILD" --prefix "$PREFIX" >/dev/null
cmake -S "$CONSUMER" -B "$OUT" -DCMAKE_PREFIX_PATH="$PREFIX" >/dev/null
cmake --build "$OUT" --parallel 2 >/dev/null
"$OUT/dynarmic_js_consumer"
test -f "$PREFIX/lib/cmake/dynarmic/dynarmicConfig.cmake"
test -f "$PREFIX/share/licenses/dynarmic.js/LICENSE"
test -f "$PREFIX/share/licenses/dynarmic.js/THIRD_PARTY_NOTICES.md"
test -f "$PREFIX/share/licenses/dynarmic.js/third-party/dynarmic-LICENSE.txt"
test -f "$PREFIX/share/licenses/dynarmic.js/third-party/fmt-LICENSE.rst"
test -f "$PREFIX/share/licenses/dynarmic.js/third-party/mcl-LICENSE"
test -f "$PREFIX/share/licenses/dynarmic.js/third-party/robin-map-LICENSE"
echo "PASS install consumer smoke"
