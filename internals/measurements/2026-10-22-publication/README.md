# Statistics publication experiment — 2026-10-22

Baseline: `4fe4b0e`. This change targets statistics publication at Run/Step
boundaries. It does not change ARM decoding, guest lowering or dispatch policy.

## Evidence and resulting implementation

A diagnostic derived from `jit_cachepopbench` compiles one entry, then executes
262,144 cached Runs with reset and result checking. Its source is retained as
`cpu_workload.cpp`; `CPU_WORKLOAD` output is intentionally not a benchmark
protocol report. The same diagnostic object was linked against baseline and
candidate archives, adding `--profiling-funcs` to the normal host link command.
Node CPU sampling used `--cpu-prof --cpu-prof-interval=100`.

The baseline publication import (`h` in the minified diagnostic JS) accounted
for about 42 ms of sampled self time in production and 305 ms with profiling.
It created typed-array views, compile objects, registry records and lookup
closures on every call; profile mode also built a name array, assigned each
field dynamically and boxed each u64 as a BigInt. Garbage collection was
visible in both profiles. Diagnostic warm wall time changed from 68 to 23 ms
in production and 377 to 57 ms with profiling. These instrumented timings
locate the bottleneck; they are not production throughput claims.

The final publisher shares one implementation between both configurations:

- It reads current Emscripten heap views and checks all eight compile totals.
  A same-instance production call with unchanged totals returns without
  constructing new JS snapshots or lookup closures.
- Instance switches still restore all legacy mirrors, even for equal totals.
  Changed compile data creates a new leaf, preserving previously retained
  values. Profile leaves are likewise newly published each time.
- Profile objects have a fixed shape. Each u64 is converted from two unsigned
  32-bit words. The high half times 2^32 is exactly representable as a double;
  the final addition rounds like the old `Number(BigInt)` conversion. The
  source counters remain thread-confined and are read synchronously at the
  Run/Step boundary. The Number representation and its safe-integer limit
  remain unchanged.
- Compile-only publication clears a stale active profile. Heap growth does
  not leave the publisher reading detached typed-array views.

`stats_test` uses member-name oracles independently of serialization offsets.
It checks all fields, source-memory preservation, equal-total owner switches,
every compile-field update, profile-only changes, frozen historical leaves,
missing-ID lookup, null profile input and actual `emscripten_resize_heap`.
Counter cases cover values around 2^32, 2^53, 2^63 and the u64 maximum.

## Controlled comparisons

Normal measurements use Release executables without CPU sampling or symbol
flags, Node v24.19.0, Emscripten 6.0.13 and an i7-12700 with affinity `0,2,4,6`.
No build or other test collection runs alongside the performance cohorts.
Reports retain complete runtime/compiler/host metadata and artifact hashes.
Baseline binaries were copied before editing. Git fields describe the observed
source checkout, not a build-time source attestation; later baseline collections
still use the retained baseline executables, verified by hash.

| Cohort | Processes per case | Result |
| --- | ---: | --- |
| Production fixed, sequential | 15 | Failed A32 page-table warm throughput (0.943 ratio; minimum 0.95) |
| Production fixed, alternating builds | 25 | Passed all original budgets; that case measured 1.015 |
| Production population, sequential | 15 | Passed all original budgets |
| Profile population, sequential | 5 | Failed single-entry thunk cold/compile latency (1.254/1.261 ratios) |
| Profile population, alternating builds | 25 | Passed all original budgets; those metrics measured 1.049/1.004 |

All failed cohorts are retained. No budgets or sample exclusions were changed.
`paired_run.mjs` alternates build order for each case and rotates cases each
round, retaining every sample in fresh processes. It refuses mismatched
identities and existing output files. Synthetic CLI tests check both protocols,
exact launch order, sample retention, changed artifacts, upstream mismatch,
regression exit status and failure after a printed PASS.
Failed children retain stdout/stderr and all previously completed samples in
a separate diagnostic; failed attempts are protected from overwrite too.

The profile cohort's cached-revisit latency ratios (candidate/baseline) are:

| Entries | Thunk | Page table | Flat window |
| ---: | ---: | ---: | ---: |
| 1 | 0.674 | 0.616 | 0.572 |
| 64 | 0.573 | 0.589 | 0.593 |
| 512 | 0.546 | 0.550 | 0.529 |
| 4096 | 0.566 | 0.752 | 0.622 |

Production cached revisits in the 15-process population cohort also improve
in most cases; the 512-entry flat-window case is 1.015 times baseline. Cold
latency, emitted bytes and cache peaks remain within the existing budgets.
The unrelated fixed kernels are regression controls, not claims that guest
instruction throughput was optimized. Application workloads, sustained engine
steady state and cross-engine performance comparisons remain open.

## Verification and reproduction

Native CTest passes 4/4; the protocol suite has 119 tests. Production and profile
strict regression gates pass, including the differential fuzz matrix. Snapshot,
instance and lifecycle tests pass in both builds. Chrome and Firefox Worker
smokes pass in both configurations. The browser CI configuration now tests both
profile modes and runs `stats_test`; hosted CI itself was not run in this
session. The Firefox driver also clears its timeout after success so an already
completed probe no longer waits for the two-minute deadline.

Raw reports, CPU samples and successful runtime logs are stored as `.gz` files.
`manifest.json` records hashes of the raw/compressed data and changed sources.
Decompress reports before using the ordinary validators/comparators:

```sh
gzip -dc internals/measurements/2026-10-22-publication/paired-fixed-before.json.gz > /tmp/publication-before.json
gzip -dc internals/measurements/2026-10-22-publication/paired-fixed-after.json.gz > /tmp/publication-after.json
node tests/bench/compare.mjs /tmp/publication-before.json /tmp/publication-after.json
```

To collect another cohort, first produce valid templates with `run.mjs` or
`cache_population_run.mjs` from the exact builds, using the same affinity. Then:

```sh
taskset -c 0,2,4,6 node tests/bench/paired_run.mjs fixed \
  <before-build>/tests <after-build>/tests before.json after.json /tmp/new-paired --runs 25
# Use 'population' and population templates for the corresponding suite.
```

Keep profiling ON/OFF separate in optimization comparisons. Existing CPU
sampling recipes are in [Performance.md](../../../docs/Performance.md). Diagnostic CPU
sampling overhead and reset/result checking are part of the retained diagnostic
wall time. Do not substitute those values for an uninstrumented performance gate.
