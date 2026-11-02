# Focused region paired CoreMark performance protocol

This is a separate performance gate for the mechanism evaluated in
`../2026-10-27-focused-region-transfer`. Transfer counts, generated-module sizes
and compile times are not speedup evidence.

## Preregistered experiment

The following protocol is fixed before the real timing cohort is collected:

- Implementation source: `24b8ba0da580cb2212e2e5fab7c5f3c9ac4e9f1b` on branch
  `a64`, plus the harness-only commit containing this preregistered protocol.
  The exact clean collection revision is recorded in the raw report.
- Upstream dynarmic: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
- Workload: `/tmp/dynarmic-coremark-long`, identity
  `ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa`,
  200,000 iterations, A64 binary SHA-256
  `8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1`.
- Architecture/memory mode: A64, numeric mode `0` (callbacks/thunks).
- Baseline focus: explicitly empty.
- Candidate focus: exactly
  `0x000000000001202c,0x0000000000012370,0x000000000001237c,0x0000000000012418,0x0000000000012424,0x0000000000012428`.
- Exactly **10 pairs / 20 guest samples**. Even pairs run baseline then
  candidate; odd pairs run candidate then baseline. No sample replacement,
  outlier removal, cohort extension or selective pooling is permitted.
- Each sample is a fresh Node process with a new module and JIT and exactly one
  Run. Every guest sample must satisfy the upstream ten-second minimum and the
  native/guest CRC oracle. A collection/protocol failure produces no final
  performance verdict and retains the failed child output.
- Both builds are Release with examples ON, profiling OFF, region diagnostics
  OFF and focus test-only exceptions OFF. Only `a64RegionFocus` may differ.
- `NODE_OPTIONS` is empty, `DYN_DUMP_DIR` is disabled, and both sides use the
  same controlled CPU affinity. No concurrent build or test workload is run
  during collection.

### Timing and speedup definitions

Kernel time is the guest timer around upstream CoreMark `iterate`, expressed in
milliseconds as `ticks / 1000`. Complete Run time is the wall interval around
`_arm_run`, including cold JIT compilation and publication but excluding module
loading, RAM setup and JIT construction. Native timing is never compared with
JIT timing.

The preregistered headline estimator is the **ratio of medians**:

```text
kernelSpeedup = baseline kernel median / candidate kernel median
runSpeedup    = baseline Run median    / candidate Run median
```

A value above 1 is faster; below 1 is slower. Candidate/baseline latency ratios
are also retained. Per-pair ratios, median/MAD, min/max, relative MAD,
candidate-faster/slower/equal counts and baseline-first/candidate-first strata
are secondary diagnostics, not alternative headline estimators. MAD is
dispersion, not a confidence interval. Ten pairs do not establish steady-state
performance or statistical significance by themselves.

### Unchanged no-regression budgets

| Candidate/baseline median | Limit |
|---|---:|
| Kernel time | `1 / 0.95` |
| Complete Run time | `1.15` |
| Sum of compile phases | `1.15` |
| Emitted bytes | `1.10` |

The profile-only cache peak gate does not apply because profiling is OFF.
Compile and emitted-byte metrics are supporting/no-regression observations only.

## Build provenance

Both artifacts were built from the clean source above with the same Emscripten
configuration except focus. Initial artifact hashes:

```text
16747bdfd6585d6fb21ff5c9416a5702853dc3b2b2274ecf971070b60bfd0073  baseline arm_jit.js
ce0ad762b23e3fdeebd53711920609312d5ba47ca4cf28902044750659375b72  baseline arm_jit.wasm
16747bdfd6585d6fb21ff5c9416a5702853dc3b2b2274ecf971070b60bfd0073  candidate arm_jit.js
e2761a9bc365cdd3d3c9ed83988ac78ac2cc03aefd733228237ad73d9b5907e2  candidate arm_jit.wasm
```

Build directories:

```text
build-em-a64-focus-perf-baseline
build-em-a64-focus-perf-candidate
```

The collector rechecks source, build configuration, environment, workload and
artifact hashes before and after the cohort.

## Harness validation

The analyzer and collector use synthetic fixtures only for tests. They cover
known-value speedup/dispersion calculations, slowdowns, focus-only build
normalization, provenance failures, duration/CRC/halt failures, compile and size
budgets, exact alternating schedule, raw failed-child retention and
non-overwriting outputs.

```sh
node --test \
  internals/measurements/2026-10-27-focused-region-performance/analyze.test.mjs \
  internals/measurements/2026-10-27-focused-region-performance/duration-paired.test.mjs
```

## Collection command

The single real cohort is collected with:

```sh
env -u DYN_DUMP_DIR NODE_OPTIONS= taskset -c 0,2,4,6 \
  node internals/measurements/2026-10-27-focused-region-performance/duration-paired.mjs \
  build-em-a64-focus-perf-baseline/examples/browser \
  build-em-a64-focus-perf-candidate/examples/browser \
  /tmp/dynarmic-coremark-long \
  /tmp/dynarmic-focused-region-performance
```

A completed comparison may pass or fail the unchanged budgets; either outcome
will be retained. Results are appended only after the cohort completes.
