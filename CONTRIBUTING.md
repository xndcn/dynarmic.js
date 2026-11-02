# Contributing

Thanks for helping improve dynarmic.js. Start with the
[build guide](docs/GettingStarted.md), [architecture](docs/Architecture.md) and
[embedding contract](docs/Embedding.md). [Support](SUPPORT.md) explains where to
ask questions and report bugs; report vulnerabilities through the process in
[SECURITY.md](SECURITY.md).

## Report a problem

Include the commit, Emscripten and Node/browser versions, guest architecture,
memory mode, build options and exact command. For an execution mismatch, attach
a minimized guest or saved fuzz case, initial CPU state, expected result and
actual result. Remove private binaries and sensitive data before sharing logs.
For performance reports, retain the raw samples and machine configuration as
described in [Performance.md](docs/Performance.md).

## Development workflow

1. Make a focused change on a branch. Keep the Dynarmic submodule unmodified;
   apply upstream changes through `patches/` against build-tree source copies.
2. Add an independent reference test or minimized Unicorn reproducer for a
   semantic change. Cover the affected architecture and memory modes.
3. Run focused tests, then the relevant native/runtime gates below. Update user
   documentation when behavior, requirements or callback contracts change.
4. Open a pull request describing the problem, resulting behavior, test commands
   and limitations. Preserve license notices and dependency attribution.

Follow the surrounding C++20, JavaScript and Python style. Avoid unrelated
formatting changes. A compiler optimization also needs compatible before/after
benchmark artifacts: disclose failed gates and measurement noise, keep slow
samples, and do not widen budgets to make a candidate pass. Keep production and
instrumented measurements separate.

## Fast checks

The benchmark protocol checks need an initialized Dynarmic submodule for source
provenance; they do not need Emscripten. The standalone C++ check needs neither:

```sh
node --test tests/bench/*.test.mjs
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc tests/unit/region_plan_test.cpp src/dynarmic/backend/wasm/region_plan.cpp \
  -o /tmp/region_plan_test
/tmp/region_plan_test
```

After a native build:

```sh
ctest --test-dir build-native --output-on-failure
```

See [tests/README.md](tests/README.md) for the suite layout and individual test
entry points. Experiment-specific checks and development archives belong under
[internals/](internals/README.md); general regression coverage belongs in
`tests/` so contributors can run it without a private development environment.

For JavaScript API or packaging changes, build and install the real npm tarball
using [tests/npm/README.md](tests/npm/README.md). Verify Node, TypeScript, browser
Workers and Vite consumers, plus the source rebuild and integrity gates. A source
import alone does not verify that the published files are complete.

For Pages tooling changes, use Node 22.12+ (CI uses Node 24 LTS), run `npm ci`
and `npm test` at the repository root. After configuring the browser build,
`npm run build` builds and packages its local source snapshot, then verifies the
artifact. `npm run pages:preview` serves it at
<http://127.0.0.1:4173/dynarmic.js/> with custom 404 routing. Run the real browser
checks in [the browser guide](examples/browser/README.md) for page changes; the
static checker alone does not verify UI or guest execution. The
[tooling research](docs/research-pages-tooling.md) explains the build boundary.

## Sanitizer build

The native sanitizer gate covers the backend and emitted-IR tests:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DDYNARMIC_JS_TESTS=ON
cmake --build build-sanitize -j2
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitize --output-on-failure
```

This checks native memory safety and undefined behavior. Runtime changes also
need the Emscripten regression and browser Worker tests.

## Runtime and release gates

```sh
tests/regress.sh build-em build-native
# Requires all fuzzing, guest and browser inputs:
tests/regress.sh --strict build-em build-native
```

The regular gate permits dependency skips; those skips do not count as release
validation. Strict mode fails when required inputs are unavailable and collects
five benchmark processes. Never rebuild an artifact while a benchmark or
regression is using it. Keep exact commands and raw results with the change.
Set `DYNARMIC_JS_TEST_ARTIFACTS` to a new directory to retain the complete run,
including successful commands and separate logs/reproducers for every fuzz
campaign. Existing paths are refused to preserve earlier evidence.

Replay the checked-in semantic corpus with:

```sh
python3 tests/fuzz/check_corpus.py \
  --a32-host build-em/tests/a32_fuzz.js \
  --a64-host build-em/tests/a64_fuzz.js
```

For new fuzz campaigns, saved cases and exact-state replay, see
[tests/fuzz/README.md](tests/fuzz/README.md). Release maintainers should also
follow [the release guide](docs/Releasing.md).

## Consumer and packaging checks

The install smoke test builds a separate consumer using only the exported
`dynarmic::dynarmic` target:

```sh
tests/install/smoke.sh build-native /tmp/dynarmic-js-install
```

When testing a sanitizer-instrumented static library, link the consumer with the
same sanitizer runtime:

```sh
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
LDFLAGS='-fsanitize=address,undefined' \
  tests/install/smoke.sh build-sanitize /tmp/dynarmic-js-sanitize-install
```

The source-embedding smoke project checks an Emscripten host from a parent CMake
project; see [tests/embedding/CMakeLists.txt](tests/embedding/CMakeLists.txt).

## Working with upstream

To update Dynarmic, fetch and check out the chosen revision in
`externals/dynarmic`, rebuild both configurations, run the full gates and commit
the new submodule pointer. Check [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
when revisions or bundled dependencies change.

Upstream `.cpp` fixes belong in `patches/NNNN-*.patch`, with paths relative to
the Dynarmic root (`a/src/...` and `b/src/...`). Configure fails with the patch
output if a patch stops applying. The current patch mechanism does not replace
upstream headers; implement header adaptations in this repository's backend or
frontend adapters.
