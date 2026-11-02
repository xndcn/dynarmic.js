# QuickJS ARM build investigation — 2026-10-31

Local GCC 10.3.1 / Emscripten 6.0.13 builds of pinned QuickJS 2025-09-13,
executed through the same Release dynarmic.js Wasm hosts on Node 24.19.0,
Linux x64, Intel Core i7-12700. Runtime source, FP semantics, workload inputs,
memory limits and the official V8 v7 harness remained fixed.

## Evidence layout

- `screen-analysis.*`: 12 ARM builds (O2/O3/Os × switch/threaded × LTO
  off/on), one preliminary round of four official cases plus full kernels.
  60/60 observations passed. The selected-metric summary is **not** the
  complete eight-suite score.
- `followup-analysis.*`: seven configurations, including four new builds,
  two screen rounds; 70/70 observations passed. This stage and confirmation
  restrict affinity to logical CPUs 8,10,12,14. External tests competed for
  CPU during part of round 0; all observations are retained and the stage is
  used only to nominate a candidate.
- `screen-raw.tar.gz`, `followup-raw.tar.gz`: immutable experiment metadata,
  build manifests, fixed artifact hashes, process observations, original
  reports and stdout/stderr logs. Each archive extracts beneath its phase
  name. Intermediate summary snapshots are omitted because they can be
  regenerated from the complete observations.
- `confirmation-analysis.*` and `confirmation-raw.tar.gz`: all ten official
  cases plus full kernels, three paired process rounds of ARM versus Thumb.
  66/66 observations passed, bringing the investigation to 196/196.
- `build-matrix.json`, `followup-build-matrix.json`: exact build commands,
  guest hashes and byte sizes. Each build preserved direct Wasm and fixed
  benchmark assets byte for byte.
- `static-review.json`, `followup-static-review.json`: ELF-to-binary identity,
  interpreter size and dispatch/ISA inspection. `multilib-review.json`
  verifies both ISAs select identical input C/library archives on this toolchain.
- `build-and-collector-sources.tar.gz`: exact historical builder/collector
  snapshots, original build logs and the command-equivalence audit. These are
  historical evidence, not alternative build entry points. Restore a snapshot
  to its original repository path to reproduce its recorded source hash.
- `checks/`: functional tooling, source packaging and CMake-generation checks.

`screen` used the initial collector. Before the following phases, the
collector was corrected to retain per-round pairing when computing multi-case
and multi-suite means, check executable script hashes around each observation,
and recover only identified dead-writer temporary results. The first stage
has one round, so its numeric aggregate is unaffected by that correction.
Its original analysis and collector remain archived.

The upstream harness still uses at least one second of warmup, one-second
measurement batches and at least 32 calls. Adaptive iterations and three
kernel samples are not independent process repeats. Compile counters cover
the whole call; they must not be added to timings that already include them.
Whole-call timings have variable work because adaptive iteration counts differ.

## Selection

The production default remains **O2, ARM, switch, without LTO or disabled
passes**. Thumb reduced the guest binary from 683,252 to 512,644 bytes
(24.97%), but its complete eight-suite speedup ratios were 0.9193, 0.9942 and
1.0130; median **0.9942×**, where values above 1 favor Thumb. Richards and
Crypto/Decrypt regressed in all three rounds. This does not establish a
stable throughput improvement or justify changing the performance default.
Thumb is available explicitly as a size tradeoff. The ten-case equally
weighted median was 0.9904×; full-kernel timings remain separate metrics.
These are Node observations on a non-exclusive workstation, not browser
speed claims. All observations, including noisy rounds, are retained. See
[the tuning notes](../../../docs/QuickJSArmTuning.md) for hypotheses and method.

## Final local validation

The final CMake build retains the original runtime bytes: all 11 frozen host,
QuickJS and workload assets match byte for byte. The updated manifests identify
the current builder and CMake source. The 60 Node protocol/UI tests, 17 source
packaging tests, 8 isolated CMake-generation checks and workflow/shell checks
passed. Chromium passed the official Richards case on all three engines with
the new manifest, plus the existing QuickJS correctness/export/cancel/retry
checks. Firefox passed the QuickJS comparison smoke.

The complete official suite was measured in Node; the final browser checks
are functional smoke checks, not a new browser performance comparison. The
full browser suites from the preceding change cover the identical runtime
bytes. `checks/final-official-smoke.mjs` was executed from the original
`build-quickjs-arm-tuning/` directory; its relative harness import is retained.
The pre-commit source-package check compared all 3,229 archived source files
with the working tree and verified all six explicit rebuild options; its log
and JSON result are retained under `checks/`. That package was a local dirty
snapshot, not a clean-release correspondence claim. These measurements and
checks were collected before committing this change. No deployment was performed.
