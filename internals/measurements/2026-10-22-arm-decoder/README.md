# Compact ARM decoder experiment — 2026-10-22

This experiment targets first-entry ARM translation latency, identified by the
cache-population benchmark added in `e77b4b1`. It does not establish application
throughput leadership, engine memory bounds or release readiness.

## Mechanism and correctness

Upstream constructs a 4,096-bucket ARM table on first use. It scans the sorted
rule list for each bucket and copies matchers (including `std::function`
handlers) into bucket vectors. The present instruction definitions produce
4,919 bucket memberships from 261 rules. A CPU sample of the original
single-entry workload showed substantial translation/allocation work; its
compile-stage counters reported about 32 ms translating and 0.08 ms constructing
the generated Wasm module. Sampling and symbol-retaining links are diagnostic,
not production benchmark results.

The final adapter keeps one static function-pointer matcher per rule and uses a compile-time index
with 4,097 16-bit offsets and 4,919 16-bit rule IDs (18,032 bytes). Construction
sorts rule IDs stably by mask specificity and enumerates only matching buckets.
Compile-time bounds reject an upstream rule expansion exceeding the index
width. The 12-bit projection, bucket ordering and argument extraction retain upstream
semantics. Dispatch wrappers specialize the upstream `GetArgInfo` and
`VisitorCaller` helpers on constant member pointers; no matcher has a dynamic
constructor, destructor, `std::function` capture or allocation. Only the build-tree copy of
`translate_arm.cpp` switches to the adapter; the submodule is unchanged.

`arm_decoder_test` independently builds the original upstream table and checks
all ordered buckets, masks and expected bits. It also compares both lookups on
every rule's expected value, each condition code, single-bit mutations and one
million seeded random words. The all-bucket check covers the entire encoding
space because both decoders use the same projection and ordered predicates;
the random checks exercise the lookup implementation as well. Another 65,536
heterogeneous argument cases compare static dispatch against upstream and
explicit bit-field expectations. Zero-argument and const handlers are covered.

The native Release and ASan/UBSan builds pass all four CTest targets. The
profile build passes all twelve cache-population cases, including exact cache
peaks and zero warm compilation. Chrome 133 and Firefox Worker smoke pass.
The final static-dispatch build also passes the complete strict regression,
including the differential fuzz matrix (`strict-static.log.gz`). An earlier
run was interrupted by editing its shell script during execution; that failed
run (`strict.log.gz`) is retained and is not counted as a pass.
The emitted A32 page-table memory-kernel modules were disassembled: differences
were embedded memory/link-slot addresses, with unchanged instruction structure.

The final A32 benchmark main Wasm artifact shrinks from 1,780,304 to 1,734,872
bytes (-45,432 bytes, -2.55%, for `jit_cachepopbench`). Generated guest-module
bytes remain unchanged. The intermediate design kept one `std::function`
matcher per rule and grew the main module to 1,826,143 bytes; that design is
not the delivered implementation.

The intermediate design's warm failures motivated a second profile: V8 was
optimizing a 58,938-byte `TranslateArm` body containing the matcher initializer.
Moving construction into a `noinline` factory did not solve this: Binaryen
inlined it again (58,933-byte body), despite LLVM respecting the attribute.
The final constant dispatch table removes that runtime construction altogether;
its `TranslateArm` Wasm body is 1,444 bytes and the retained trace shows only
Liftoff compilation for that function in the 64-entry diagnostic.
This is why failed cohorts and the intermediate trace logs are retained.

## Collection and provenance

Machine: i7-12700, Node v24.19.0 / the V8 version recorded in each report,
Emscripten 6.0.13, Release, profiling counters OFF. Normal comparisons use CPU
list `0,2,4,6` (four distinct P cores). No CPU sampling, symbol flags or module
dumping is enabled in those comparisons. Builds and other test processes are
stopped for measurement. These cores are not exclusively reserved; OS and
frequency noise remain possible.

Baseline executables were copied from the clean `e77b4b1` build before editing;
candidate executables were rebuilt with each adapter revision. The saved reports retain
actual executable/workload SHA256 values. As documented by the benchmark
protocol, git fields describe the source checkout observed during collection,
not a build-time source manifest. Repeated baseline reports therefore observe
a dirty source checkout while still using the same retained baseline binaries.
Artifact hashes establish that those baseline files did not change. Candidate
reports were collected before its implementation commit and are marked dirty.
The source hashes in `manifest.json` identify the candidate adapter and patch.

The intermediate `std::function` design's initial five-process cohorts showed clear cold improvements but failed
several warm gates. A predeclared 15-process repeat passed every fixed-suite
gate; its population comparison still failed the 64-entry thunk warm gate
(1.195 times baseline, against a 1.15 limit). Both sides have high dispersion
in that case. These failures are retained, not silently discarded. A further
25-round population experiment alternates baseline/candidate order per case
and rotates case order each round. It still failed four warm metrics, including
all three 64-entry modes. The regression therefore was not dismissed as noise.
The final static-dispatch design is measured in a separate 25-round paired
population cohort and a new 15-process fixed-suite comparison. Every cohort
keeps all observations and uses the same existing budgets.

Reports use ordinary schemas and validators. `.json.gz` files are losslessly
compressed raw reports, including failed comparisons. Decompress before
running the validators/comparators; the experiment manifest records hashes of
both raw and compressed bytes.

## Final comparison results

Both final gates pass: `fixed-comparison-static.json.gz` (15 fresh processes
per case) and `population-comparison-static.json.gz` (25 paired rounds). No
budgets were changed. Every generated-module byte count is unchanged. Fixed
A32 memory compile-phase medians fall to 0.520–0.556 of baseline, while warm
throughput ratios are 0.996–1.023. The call/return compile median is 0.333 of
baseline. A64/Thumb results are controls, not claimed optimizations.

The final population ratios below are candidate/baseline latency; lower is
better. The raw artifacts contain absolute samples and dispersion.

| Configuration | Entries | Cold pass | First cached revisit |
| --- | ---: | ---: | ---: |
| Thunk | 1 | 0.356 | 0.898 |
| Thunk | 64 | 0.450 | 1.112 |
| Thunk | 512 | 0.726 | 0.995 |
| Thunk | 4096 | 0.916 | 1.140 |
| Page table | 1 | 0.363 | 0.980 |
| Page table | 64 | 0.482 | 1.086 |
| Page table | 512 | 0.753 | 0.916 |
| Page table | 4096 | 0.888 | 0.922 |
| Flat window | 1 | 0.407 | 1.049 |
| Flat window | 64 | 0.485 | 0.998 |
| Flat window | 512 | 0.745 | 1.055 |
| Flat window | 4096 | 0.901 | 1.084 |

For example, the 25-round thunk single-entry cold median falls from 51.696 to
18.412 ms. The 4,096-entry thunk cached revisit rises from 7.931 to 9.038 ms
(+14.0%, within the existing +15% review budget). Passing gates does **not**
mean every metric improved; the latter is a remaining sensitivity to inspect
when profiling Run/publication overhead and engine tiering. This experiment
supports a first-entry translation optimization, not an unqualified throughput
claim. Runtime memory ceilings and application workloads remain open.

## Reproduction

To check the retained final population gate from the repository root:

```sh
gzip -dc internals/measurements/2026-10-22-arm-decoder/static-population-before-paired.json.gz > /tmp/population-before.json
gzip -dc internals/measurements/2026-10-22-arm-decoder/static-population-after-paired.json.gz > /tmp/population-after.json
node tests/bench/cache_population_compare.mjs /tmp/population-before.json /tmp/population-after.json
```

Build the baseline revision and candidate in separate source/build trees using
the README instructions. For each, collect both suites on the same host:

```sh
taskset -c 0,2,4,6 node tests/bench/run.mjs <build>/tests --runs 15 --output fixed.json
taskset -c 0,2,4,6 node tests/bench/cache_population_run.mjs <build>/tests --runs 15 --output population.json
node tests/bench/compare.mjs before-fixed.json after-fixed.json
node tests/bench/cache_population_compare.mjs before-population.json after-population.json
```

The retained follow-up script accepts two validated report templates from the
exact builds and rechecks artifact, workload and host identities before and
after sampling. It is Linux-specific because it verifies `/proc` affinity:

```sh
taskset -c 0,2,4,6 node internals/measurements/2026-10-22-arm-decoder/paired-population.mjs \
  <baseline-build>/tests <candidate-build>/tests \
  before-population.json after-population.json /tmp/arm-decoder-paired 25
node tests/bench/cache_population_compare.mjs \
  /tmp/arm-decoder-paired/population-before-paired.json \
  /tmp/arm-decoder-paired/population-after-paired.json
```

For CPU sampling, follow [Performance.md](../../../docs/Performance.md)'s separate
`--profiling-funcs` build recipe. This session equivalently relinked the exact
benchmark object/static archives from each build's `link.txt`, adding only
`--profiling-funcs` and a distinct output filename. Baseline and candidate
`.cpuprofile` files are retained. The candidate sampling run overlapped other
correctness/build work, so its wall time is not comparative performance
evidence. The trace logs demonstrate that V8 compilation events can be
observed; they do not certify steady state for the production reports.
