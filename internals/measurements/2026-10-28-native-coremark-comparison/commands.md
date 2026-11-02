# Reproduction and validation commands

All paths below are absolute because the preserved cohort used them. Output directories must not already exist; collectors and analyzers refuse to overwrite evidence.

## Inputs

- Repository: `/home/xiong/workspace/dynarmic.js`
- Clean CoreMark checkout: `/tmp/dynarmic-coremark-source`, revision `1f483d5b8316753a742cbf5590caf5bd0a4e4777`
- Existing pinned workload: `/tmp/dynarmic-coremark-long`
- A64 guest: `/tmp/dynarmic-coremark-long/coremark_a64.bin`, SHA-256 `8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1`
- Workload: CoreMark 2K performance seeds, one context, 200,000 iterations

The workload builder and source hashes are recorded in `evidence/base-workload-manifest.json`. The Linux binaries use the same algorithm sources, the same HOST monotonic-microsecond port, and the adapted `ee_printf.c` identified by that manifest.

## Build matched x86-64 and AArch64 Linux executables

```sh
python3 -I /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/build_linux_coremark.py \
  /tmp/dynarmic-coremark-source \
  /tmp/dynarmic-coremark-long \
  /tmp/dynarmic-native-coremark-linux-v3
```

Both are statically linked musl executables built by Zig's Clang 22.1.8 with `-O2`. The exact commands and artifact hashes are in `evidence/linux-build-manifest.json`.

The first build attempt requested static `*-linux-gnu` targets. Zig rejected them with `libc of the specified target requires dynamic linking`; no benchmark was run from those failed output directories. The successful build uses `x86_64-linux-musl` and `aarch64-linux-musl`.

## Build native x86-64 dynarmic runner

```sh
cmake \
  -S /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/native \
  -B /tmp/dynarmic-native-x64-coremark \
  -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/dynarmic-native-x64-coremark \
  --target a64_coremark_host -j 8
```

This standalone CMake project selects `ARCHITECTURE=x86_64`, only the A64 frontend, Release optimization, bundled externals, and dynarmic's native x64 backend. It is independent of the repository's Wasm backend build. The runner implements the existing `svc #0/#1/#4` guest ABI and loads the flat guest at `0x10000` in 16 MiB RAM. A post-link step writes `native-build-manifest.json`, binding compiler, optimization flags, source hashes, revisions and executable hash; the collector requires and freezes this manifest.

## Unit and protocol tests

```sh
node --experimental-test-coverage --test \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/compare.test.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.test.mjs \
  /home/xiong/workspace/dynarmic.js/tests/bench/coremark_protocol.test.mjs
```

The final combined suite passes 52/52. Comparison utilities have 100% line,
95.83% branch and 100% function coverage; the analyzer has 91.59% line, 91.67%
branch and 100% function coverage (its uncovered lines are the standalone CLI
entrypoint). Six analysis tests cover schema/provenance, overwrite refusal,
truncated stderr, missing or tampered dynarmic diagnostics, unexpected
non-dynarmic diagnostics and rejection of an unoptimized native host.

## Correctness and collector integration checks

Native dynarmic memory modes 0, 1 and 2 were each run once against the pinned guest. Every mode produced `crcfinal=0x4983` and `DYNARMIC_HALT_REASON=0x01000000`. Mode 2 is the sole mode used by the formal cohort.

A one-round collector integration run initially executed x86 and qemu-user successfully and executed the dynarmic guest, but the harness then failed because `String.prototype.matchAll` requires a global RegExp. The failed record remains at `/tmp/dynarmic-native-comparison-collector-smoke`; it is not a performance cohort. Diagnostic parsing was extracted into a tested pure function. A new one-round integration output directory then passed collection and analysis at `/tmp/dynarmic-native-comparison-collector-smoke-v2`.

## Formal five-round cohort

```sh
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/collect.mjs \
  /tmp/dynarmic-native-coremark-linux-v3/linux-manifest.json \
  /tmp/dynarmic-native-x64-coremark/a64_coremark_host \
  /tmp/dynarmic-coremark-long/coremark_a64.bin \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence \
  5
```

Every sample is a fresh process under host CPU affinity `0,2,4,6`. The implementation order rotates by round rather than running one implementation's entire cohort first. The per-process timeout is preregistered at 180 seconds. Any nonzero exit, signal, timeout, malformed report, wrong CRC, abnormal dynarmic halt or inconsistent timing stops the cohort after preserving that sample's raw streams and record.

The first complete cohort is preserved under `pre-review-cohort/` and excluded from final conclusions because it predates independent stderr validation and native-build provenance enforcement. The final `evidence/` cohort was collected after those fixes. Neither cohort was edited or rerun selectively to improve its result.

```sh
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence/analysis.json
```

The analyzer reparses every raw stdout and dynarmic stderr, requires all individual CRCs and `crcfinal=0x4983`, rechecks Run/halt/timing invariants and validates the frozen optimized native-host build manifest. It derives per-implementation medians, MAD, ranges and ratios of median guest-kernel times. Whole-process wall time and dynarmic Run time remain separate observations. This is a fixed-workload throughput comparison, not an official CoreMark score submission; any run below ten seconds is explicitly not duration-qualified.

## Final deterministic verification

A fresh analysis to a previously absent path was compared byte-for-byte with the frozen report:

```sh
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence \
  /tmp/dynarmic-native-coremark-final-analysis.json
cmp /tmp/dynarmic-native-coremark-final-analysis.json \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence/analysis.json
sha256sum /tmp/dynarmic-native-coremark-final-analysis.json
```

Both files have SHA-256 `16ffd6638b41d7639a00088dd5da7a0ab9d9ef08077fcf8d283020315990fd73`. Final `node --check` and Python isolated-mode compile checks also pass. Independent re-review approved the corrected protocol with no remaining CRITICAL, HIGH or result-invalidating MEDIUM findings.

## Four-way dynarmic.js Wasm extension

The original `evidence/` cohort and its v1 analysis remain immutable. Adding the optional Wasm build directory selects a separate four-way config/completion/analysis-v2 protocol; omitting it preserves the three-way collector interface. A formal four-way cohort must be collected into a new directory and only then copied to `evidence-four-way/`.

Build the ordinary production browser artifact from a clean harness commit. Do not substitute a profiling, region-diagnostics or focused-selection build:

```sh
source /home/xiong/sandbox/emsdk/emsdk_env.sh
env -u EMCC_CFLAGS emcmake cmake \
  -S /home/xiong/workspace/dynarmic.js \
  -B /tmp/dynarmic-js-four-way-production-build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDYNARMIC_JS_PROFILE=OFF \
  -DDYNARMIC_JS_REGION_DIAGNOSTICS=OFF \
  -DDYNARMIC_JS_A64_REGION_FOCUS= \
  -DDYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS=OFF \
  -DDYNARMIC_JS_EXAMPLES=ON \
  -DBoost_INCLUDE_DIR=/tmp/boost-headers
env -u EMCC_CFLAGS cmake --build /tmp/dynarmic-js-four-way-production-build --target arm_jit -j 8
```

Reconfigure and relink the native host at the same clean HEAD so its post-link manifest matches the repository revision:

```sh
cmake \
  -S /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/native \
  -B /tmp/dynarmic-native-x64-coremark-four-way \
  -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/dynarmic-native-x64-coremark-four-way \
  --target a64_coremark_host -j 8
```

Run the comparison, Wasm parser, collector and analyzer tests, plus the existing guest protocol tests:

```sh
node --experimental-test-coverage \
  --test-coverage-exclude='**/native-coremark-comparison/scripts/collect.mjs' \
  --test \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/compare.test.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/wasm.test.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/wasm_build.test.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/collect.test.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.test.mjs \
  /home/xiong/workspace/dynarmic.js/tests/bench/coremark_protocol.test.mjs \
  /home/xiong/workspace/dynarmic.js/tests/bench/environment.test.mjs
```

The extended combined suite passes 240/240 with 91.03% line, 92.36% branch and
96.51% function coverage across the reported files. The collector support and
Wasm parser modules each have 100% line and function coverage, with 98.08% and
100% branch coverage respectively; the analyzer has 95.71% line, 98.53% branch
and 100% function coverage. Its only uncovered lines are the standalone CLI
entrypoint. The process-oriented collector CLI is exercised separately by the
integration queue below. The analyzer suite includes real-v1-evidence byte
equality, raw/derived tampering, strict stderr and Wasm build/provenance
negative tests. Build-manifest tests cover post-link source/configuration/
artifact binding, dirty source, real submodule dirt hidden by Git configuration,
untracked files hidden by Git configuration and missing inputs.

A standalone correctness smoke uses one fresh Node/V8 process, one module/JIT and one `_arm_run`:

```sh
env -u NODE_OPTIONS -u DYN_DUMP_DIR \
  node /home/xiong/workspace/dynarmic.js/tests/bench/coremark/sample.mjs \
  /tmp/dynarmic-js-four-way-production-build/examples/browser/dist/arm_jit.js \
  /tmp/dynarmic-coremark-long/coremark_a64.bin \
  a64 2
```

Before the formal cohort, collect and analyze a one-round, four-process integration queue in a previously absent directory:

```sh
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/collect.mjs \
  /tmp/dynarmic-native-coremark-linux-v3/linux-manifest.json \
  /tmp/dynarmic-native-x64-coremark-four-way/a64_coremark_host \
  /tmp/dynarmic-coremark-long/coremark_a64.bin \
  /tmp/dynarmic-native-comparison-four-way-smoke \
  1 \
  /tmp/dynarmic-js-four-way-production-build
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.mjs \
  /tmp/dynarmic-native-comparison-four-way-smoke \
  /tmp/dynarmic-native-comparison-four-way-smoke/analysis.json
```

The formal queue has five rounds and 20 fresh processes. All implementations use affinity `0,2,4,6`; native dynarmic and Wasm execute the same A64 freestanding guest in memory mode 2. Every child receives only the recorded fixed `PATH`, `LC_ALL=C` and empty `NODE_OPTIONS`; no host credentials, dynamic-loader settings or `DYN_DUMP_DIR` are inherited. Non-dynarmic stderr must be empty, and native dynarmic stderr must contain exactly its four validated diagnostics. Wasm uses `process.execPath`; `runMs` surrounds only `_arm_run`, includes cold compilation/publication, and excludes module loading, RAM setup and JIT construction; it remains separate from guest-kernel and whole-process wall timing.

The `arm_jit` POST_BUILD step writes `wasm-build-manifest.json`, binding the actual linked JS/Wasm hashes to build-time source revisions, dirty state, compiler flags and production feature configuration. Source cleanliness forces submodule and untracked-file reporting rather than honoring Git configuration that could hide changes. The collector requires this manifest to match the current cache, source, build scripts and artifacts, then freezes its original text; a stale Debug/focused artifact cannot be made valid by reconfiguring its cache. Both collector and independent analyzer also reject direct definitions of the focus, profiling or region-diagnostics macros in the actual global/Release C++ flags, including quoted, separated `-D` and GNU/Clang `--define-macro` forms, and fail closed on indirect compiler input through `-Xclang`, `-Xpreprocessor`, `-Wp,`, response files, forced includes or compiler configuration files. The manifest separately records `EMCC_CFLAGS` at configure and build time; both must be empty, and the formal commands explicitly unset it. Node/V8 identity is a separate collection-time `wasmRuntime` observation, and all runner modules (including `scoped_cpu_profile.mjs`) are separate `wasmRunner` hashes. Every post-cohort verification runs inside the same failure-evidence boundary as the samples.

```sh
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/collect.mjs \
  /tmp/dynarmic-native-coremark-linux-v3/linux-manifest.json \
  /tmp/dynarmic-native-x64-coremark-four-way/a64_coremark_host \
  /tmp/dynarmic-coremark-long/coremark_a64.bin \
  /tmp/dynarmic-native-comparison-four-way-formal \
  5 \
  /tmp/dynarmic-js-four-way-production-build
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.mjs \
  /tmp/dynarmic-native-comparison-four-way-formal \
  /tmp/dynarmic-native-comparison-four-way-formal/analysis.json
```

After all samples and post-cohort identity checks pass, preserve the complete directory as `evidence-four-way/`. Reanalysis must produce a byte-identical `analysis.json`; the v1 analyzer regression must still reproduce the original hash above. No ratio may combine the old native medians with the new Wasm samples.

## Completed four-way validation and evidence

The standalone production Wasm smoke passed with normal halt, `crcfinal=0x4983` and 27.198062 guest-kernel seconds. The one-round four-process integration queue at `/tmp/dynarmic-native-comparison-four-way-smoke` also passed collection and independent analysis; it is an integration check, not a performance cohort.

The formal 20-process queue completed at clean harness revision `045107cd21d0e2bbd9fc8cf43baef74e5bea0a31`. All processes exited normally with the expected CRCs, and all post-cohort source, configuration, runner and artifact checks passed. Its complete raw evidence was copied without modification:

```sh
cp -a /tmp/dynarmic-native-comparison-four-way-formal \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence-four-way
node /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/scripts/analyze.mjs \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence-four-way \
  /tmp/dynarmic-native-coremark-four-way-final-analysis.json
cmp /tmp/dynarmic-native-coremark-four-way-final-analysis.json \
  /home/xiong/workspace/dynarmic.js/docs/measurements/2026-10-28-native-coremark-comparison/evidence-four-way/analysis.json
sha256sum /tmp/dynarmic-native-coremark-four-way-final-analysis.json
```

Independent reanalysis of both the temporary collection and the preserved copy was byte-identical. The frozen four-way analysis has SHA-256 `d3c7bed8ae3bb431b1111a33db765c783ed17d9f1caeb6371e0ad224b8a4c5fc`; results and ratios are tabulated in `README.md`. The separate v1 analysis remains byte-identical with SHA-256 `16ffd6638b41d7639a00088dd5da7a0ab9d9ef08077fcf8d283020315990fd73`.
