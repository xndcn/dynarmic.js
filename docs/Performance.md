# Performance and profiling protocol

## Scope and provenance

The fixed suite covers three A32 memory modes, one A32 call/return workload,
three AArch64 memory modes, paired AArch64 scalar/NEON arithmetic loops and
three Thumb memory modes. It still does not establish browser, large-cache or
application-level performance
leadership. The [roadmap](Roadmap.md) tracks that missing coverage. Historical
timings in the [backend development notes](../internals/WasmBackendHistory.md)
are not versioned performance baselines.

Build first, then stop other builds and test processes before measuring. Run
baseline and candidate on the same machine with the same power/CPU policy.
Keep raw JSON artifacts, executable hashes and exact source/build commands.
The runner rejects executable files that change during measurement and refuses
`DYN_DUMP_DIR` instrumentation. Child processes use inherited `NODE_OPTIONS`;
runner-only command-line Node flags are not forwarded. Its git
fields describe the build tree’s configured source checkout at measurement time; hashes describe the actual
executables. It does not prove that an old executable was built from the
current checkout. Use clean, explicitly identified builds for public claims.
Collectors also record diagnostic and test-exception switches when present;
comparisons reject differences or missing-versus-present configuration. Retain
the original reports and recollect older baselines with the current collector
when required provenance is absent. Fixed and cache collectors refuse existing
output paths and retain child output and partial samples in
`<output>.failure.json` if collection fails.

## Native Dynarmic comparison

`tests/bench/native` is a separate CMake project that links the unmodified,
pinned upstream CPU backend. It does not link dynarmic.js's native runtime stub.
Both backends compile the same five C++ hosts and execute the same 12 fixed
guest workloads with the same halt and result checks. No guest cross compiler
is needed for these embedded instruction streams. The native comparison
currently supports Linux x86-64 and AArch64 hosts; CI exercises x86-64.
`python3 tests/bench/check_instruction_counts.py` independently executes the
embedded streams in Unicorn and counts every guest instruction through SVC.
This checks the MIPS denominator as well as the result and runs in CI; the
JavaScript fast suite explicitly skips this check if Unicorn is unavailable.

```sh
cmake -S tests/bench/native -B build-dynarmic-native -DCMAKE_BUILD_TYPE=Release
cmake --build build-dynarmic-native -j2
ctest --test-dir build-dynarmic-native --output-on-failure
# build-em must be a production Release build, with default region settings.
cmake --build build-em -j2 --target \
  jit_bench jit_thumb_bench jit_a64_bench jit_a64_simd_bench jit_callbench
node tests/bench/native_run.mjs build-em/tests build-dynarmic-native \
  --runs 7 --output native-comparison.json
node tests/bench/native_validate.mjs native-comparison.json --require-comparison
```

The collector rotates cases and alternates native/Wasm order in each pair.
Each child is a fresh process with one fresh JIT. It retains the first Run and
all eight subsequent Runs of that JIT. At least five process pairs are required
for a comparison; shorter reports are explicitly `smoke-only`. The validator
reparses raw child output, checks result markers and instruction counts, and
recomputes every summary. Nonzero exits, signals, changed artifacts and workload
or upstream mismatches fail collection. Existing output files are never
overwritten; failures retain partial observations in `<output>.failure.json`.

| Report value | Interpretation |
| --- | --- |
| `native.coldMedianMs`, `wasm.coldMedianMs` | First Run, including translation and guest execution |
| `native.warmMedianMs`, `wasm.warmMedianMs` | Median of each process's eight-Run median |
| `warmProcessMadMs`, `warmIndexMedianMs` | Between-process dispersion and warmup curve |
| `nativeOverWasmThroughput.warmMedian` | Median paired Wasm time / native time; above 1 means native is faster |
| `nativeOverWasmThroughput.warmMad` | Median absolute deviation of paired ratios |

Run timing excludes process startup, host module loading, JIT construction,
reset and result validation on both sides. Native uses `steady_clock`; Wasm
uses Emscripten's performance clock. Cold Run time is not compilation time.
Native compilation phases and memory usage are unavailable and are never
represented as zeros. Wasm warm samples include engine tiering; the protocol
does not claim that eight Runs reach a universal steady state.

Callback and page-table cases use the same guest memory and callbacks. The
flat-window cases compare Wasm's bounded window with native fastmem for the
same in-range addresses; these implementations have different bounds and fault
handling costs. Native uses its normal CPU feature detection. Compiler versions,
flags, upstream options, CPU, affinity and Node options are retained so the
tradeoffs are visible. Do not mix profile/diagnostic builds with production
results or interpret a ratio as a comparison of compilers alone.

The Native comparison CI workflow checks the complete suite and uploads raw
observations. Hosted runners are noisy; there is no CI speed threshold against
native. For release measurements, use an idle controlled host, record its CPU
frequency/power policy, pin the collector to a CPU if appropriate (children
inherit affinity), and repeat the whole experiment when dispersion is high.
The short Thumb kernel is particularly sensitive to timer and scheduling noise.
Report per-case results and dispersion instead of selecting best runs or
collapsing all memory modes into one headline. These are microkernel results;
CoreMark and real browser measurements remain separate protocols.

A complete [recorded native comparison](../internals/measurements/2026-10-30-native-comparison/README.md)
includes all 1,080 samples, per-case results and an offline hash/schema verifier.
It illustrates the protocol on a shared workstation; its dispersion is
disclosed and it is not a controlled release baseline.

## Profile schema

Configure with `-DDYNARMIC_JS_PROFILE=ON`. Each JIT publishes its cumulative
counters at `Module.dynarmicWasmStats.profile` after Run/Step, using
`dynarmic.js/profile-v1`:

| Field | Meaning |
| --- | --- |
| `runCount`, `stepCount` | Calls entering Run and Step |
| `dispatchCount` | Dispatcher entries into compiled code; excludes chained calls |
| `dispatchCacheHits`, `dispatchCacheMisses` | GetOrEmit lookup outcomes, including link resolution |
| `compiledBlocks` | Emitted modules (a region counts as one module) |
| `compiledRegions`, `regionBlocks` | Multi-block modules and their contained blocks |
| `emittedBytes` | Sum of emitted module byte lengths |
| `linkPatches` | Lazy direct-link patches performed by the dispatcher |
| `fastDispatchMisses` | Direct-table misses filled by the dispatcher |
| `invalidationCount`, `invalidatedBlocks` | Full clears or range operations dropping modules; modules dropped |
| `cycleBudgetExits` | Dispatcher observations of an exhausted cycle budget |
| `regionCandidates` | Region successor locations popped from the selector work queue |
| `regionRejectedIncompatible` | Unique candidates rejected by frontend compatibility policy |
| `regionRejectedDistance` | Unique compatible candidates outside the guest-distance limit |
| `regionRejectedBlockBudget` | Unique compatible candidates skipped after the block budget was filled |
| `regionRejectedIrBudget` | Unique compatible, nearby candidates whose IR would exceed the instruction budget |
| `peakCachedBlocks` | Maximum number of live emitted modules observed by this JIT |
| `peakCachedBytes` | Maximum total Wasm module bytes live in the code cache |

Counters are monotonic for one JIT. The C++ `CompileStats` object is owned by
that JIT and receives translation, optimization, emission, engine compile,
instantiation and table-registration totals directly. Emscripten publishes
snapshots under `Module.dynarmicWasmStats.instances[instanceId]` and marks the
latest one with `activeInstance`; legacy top-level fields mirror that active
snapshot for existing hosts. They are not an aggregate. The fixed hosts use
one JIT per process, so before/after deltas are valid there. When multiple JITs
are live, select an instance by ID (or call
`Module.dynarmicWasmStats.getInstance(id)`) and never subtract snapshots from
different instances.

Phase accumulation uses C++ member updates selected at compile time. Engine
compile/instantiate/table timers still originate in JS. Generated-function
registration avoids the generic main-table scan while preserving the shared
Emscripten allocator and ordinary host function registration. For compile-only snapshots, C++ compares the active owner and all eight totals
before entering JS, so a cached Run without new statistics also avoids boxing
the u64 instance ID. Profiling still publishes every Run/Step; switching owners
or changing any phase invalidates the shortcut. These changes do not redefine
the eight compile totals or the profile-v1 counters.

Treat these records as read-only. Retained `compile` and `profile` leaf objects
keep their values; registry entries refer to the latest publication. The
publisher may reuse a compile object when all eight totals are unchanged.
Production calls for the same active instance then avoid rebuilding the
snapshot. Switching instances still updates the legacy mirrors, including
when their totals happen to match. Profile builds publish the latest counters
on every Run/Step. Views of linear memory are read afresh after memory growth.
Counters remain JavaScript Numbers: conversion preserves the previous u64
rounding behavior, while valid benchmark counters must be safe integers.
Take baseline copies and subtract values to measure deltas.

The five region-selection fields and the two cache high-water marks are
append-only extensions of
`dynarmic.js/profile-v1`; existing counter offsets and names retain their
meaning. Cache peaks are monotonic counters: invalidation lowers current live
occupancy but never lowers a previously observed peak. A candidate is counted when popped from the selector queue. Already
seen locations do not count as a policy rejection, and each remaining
candidate is assigned to the first applicable rejection reason in the order
compatibility, block budget, guest distance and IR budget. The entry block is
always retained and does not count as a candidate.

Production builds remove dispatch counter updates at compile time. Compile
phase timers still execute in production builds; there is no claim that all
instrumentation is free. Measure production throughput separately from the
profile build, and never compare the two as an optimization claim.

Use `node tests/bench/overhead.mjs production.json profile.json` for the
deliberate profile overhead experiment. It requires the same host, engine,
affinity, compiler/link flags, workload hashes, instruction counts and at
least five processes, with only the profile build option different. Its
`dynarmic.js/profile-overhead-v1` output reports warm wall-time and throughput
ratios, cold compile-phase ratio and emitted-byte ratio. Do not feed this
artifact to the performance regression comparator.

## Compile-boundary region diagnostics

Configure a separate Emscripten diagnostic build with
`-DDYNARMIC_JS_REGION_DIAGNOSTICS=ON`; native configuration rejects this switch,
which is independent of `DYNARMIC_JS_PROFILE`. After Run/Step, the owning
instance snapshot contains `regionDiagnostics`; the current schema is
`dynarmic.js/region-diagnostics-v2`, while `projectRegionDiagnostics` produces
the exact `dynarmic.js/region-diagnostics-v1` view for duplication analysis.
Production and ordinary profile builds do not contain this leaf.

Each append-only region record contains:

| Field | Meaning |
| --- | --- |
| `regionId`, `frontend` | Contiguous per-JIT history ID and A32/A64 identity |
| `entryDescriptor`, `entryPc`, `entryIndex` | Full entry identity, guest PC and selected-array index |
| `moduleBytes`, `entryBodyBytes` | Whole module and exact serialized exported entry-body sizes |
| `layout` | `single`, `structured` or `fallback` |
| `blocks` | Full descriptors plus emitted, original-entry-reachable and return-site masks |
| `liveRegionIdsBeforeCompile` | Sorted unique generation IDs owned by live cache entries before compilation |

The v1 projection exposes `selectedDescriptors` in place of `blocks`; all other
v1 fields are unchanged.

Descriptors and PCs are fixed-width 16-digit hexadecimal strings; do not coerce
them to JavaScript Numbers. Histories are compilation events, not live-cache
residency. Warm cache hits append nothing, invalidation does not erase old
records, and recompilation receives a new ID. Snapshots and nested arrays are
frozen; retaining an old snapshot does not turn it into a live view.

Use `analyzeRegionDiagnostics` from `tests/bench/region_diagnostics.mjs` to
derive `dynarmic.js/region-duplication-v1`. Duplicate selections attribute a
full descriptor to earlier/later region IDs. They are **static selection
counts**, not dynamic calls, RSB returns, other tail transfers, or proof of
byte-identical generated Wasm. Never divide a region's body bytes among its
selected blocks.

For a complete CoreMark compile-history report, build the browser host with
region diagnostics enabled and run:

```sh
node tests/bench/coremark/region_diagnostics.mjs \
  build-em-region-diagnostics/examples/browser \
  /path/to/coremark-workload a64 2 /tmp/a64-regions.json
```

The collector checks the native CRC oracle, normal guest halt and exact
compile/profile totals, and preserves raw records, derived duplication and
provenance under `dynarmic.js/coremark-region-diagnostics-v1`. Its Run time is a
diagnostic observation, **not a benchmark score**. The retained A64 flat-window
evidence and offline verifier are in
[the region-diagnostics record](../internals/measurements/2026-10-24-region-diagnostics/README.md).
The verifier also reruns the generated-Wasm comparison with wabt and requires
rejection of both an ordinary instruction mutation and an inconsistent link-slot
miss address. Browser CI runs this evidence gate and executes the diagnostic
and diagnostic-plus-profile hosts in Node and a real Firefox Worker.

### Bounded call/return static screen

The [PushRSB ablation record](../internals/measurements/2026-10-24-push-rsb-ablation/README.md)
tests one causal question without adding execution counters: what happens when
28 exact A64 return descriptors are suppressed **only** as `PushRSB` successors?
Ordinary branches and direct entries to those descriptors remain eligible. The
hardcoded set is experiment machinery, not production policy.

For the same complete A64 flat-window CoreMark history, selected blocks fall
4,177 → 3,208, duplicate selections 3,315 → 2,353, module bytes
708,917 → 690,520 and entry-body bytes 694,277 → 676,174. Both static byte gates
pass. The 28 targets fall from 451 selections to 25; five disappear and the
others remain reachable through ordinary control flow. Thirty-six entry
histories change, while six retained hot histories are exactly unchanged.

The entry sequence remains at 111 regions, so the probe does not eliminate
repeated entry compilation and is not accepted for production. Its Run time is
not a score, and no paired throughput/compile-latency comparison was started.
The next architectural screen is live multi-entry reuse. The completed
diagnostic-only v2 history classifies earlier selections by fallback/structured
layout, actual emission, original-entry reachability and return-site eligibility,
joined with exact live region generations at each later miss. The retained
[region-alias coverage record](../internals/measurements/2026-10-24-region-alias-coverage/README.md)
deep-equals the complete v1 projection and finds 16 covered entries among 82,
passing the pre-registered >=9 gate for bounded A64 fallback return-site
prototype TDD. This is not dynamic hotness, a performance result, or permission
to broaden the design to structured regions. Do not treat whole-region byte
attribution as reclaimable bytes or add dynamic counters unless a concrete
hot-edge decision remains unresolved.

## CPU sampling and generated-code review

Compile-phase totals locate a slow stage; use a separate diagnostic executable
to identify work inside that stage. For named C++ Wasm frames, configure a
Release build with Emscripten's `--profiling-funcs` link flag:

```sh
emcmake cmake -S . -B build-em-cpu -DCMAKE_BUILD_TYPE=Release \
  -DBoost_INCLUDE_DIR=/path/to/boost-headers -DDYNARMIC_JS_PROFILE=OFF \
  -DCMAKE_EXE_LINKER_FLAGS=--profiling-funcs
cmake --build build-em-cpu --target jit_cachepopbench -j2
mkdir -p /tmp/dynarmic-cpu
node --cpu-prof --cpu-prof-interval=100 --cpu-prof-dir=/tmp/dynarmic-cpu \
  --cpu-prof-name=cold.cpuprofile build-em-cpu/tests/jit_cachepopbench.js 0 1
```

Open the `.cpuprofile` in DevTools' JavaScript Profiler. Inspect the call tree
as well as self time: compiler inlining and function merging can obscure
individual C++ function attribution. Sampling itself changes timing, and the
symbol flag changes the main Wasm artifact. Use these files to form hypotheses;
verify improvements with ordinary Release builds and compatible benchmark
reports. Do not treat sample counts as exact compile-phase duration.

For a reproducible text/JSON view, use the CPU sample analyzer:

```sh
node tests/bench/cpu_profile.mjs /tmp/dynarmic-cpu/cold.cpuprofile > all-samples.json
node tests/bench/cpu_profile.mjs /tmp/dynarmic-cpu/cold.cpuprofile \
  --root 'AddressSpace::GetOrEmit' > compiler-samples.json
```

The `cpu-samples-v1` diagnostic preserves function name, script ID, URL and
source position instead of merging identically named functions from different
modules. `selfSamples` counts leaves; `inclusiveSamples` counts a frame at most
once per sample even when recursive. `--root` matches function names and selects
the union of their subtrees; callers above those roots and detached GC samples
are excluded. Shares use the selected sample count as denominator. Inclusive
shares overlap and must not be added. A missing or unsampled root, malformed
tree or inconsistent sample timing fails with exit 2. The CLI binds its summary
to the raw file's SHA256.

Counts are deliberately unweighted: a long scheduling gap must not be presented
as precise time spent in the next sampled function. The summary includes raw
delta median/p95/maximum, elapsed duration and trailing time for review; it
does not infer time for missing samples. Full-process CPU collection includes
startup, teardown and engine work outside the JIT's phase timers. Use multiple
fresh processes and retain both full and scoped profiles. In particular,
Binaryen may merge functions under one surviving name, so a destructor or
helper name alone does not identify the source of allocations. Verify the
callers and code before choosing a change.

For application execution, scope sampling to exactly one complete CoreMark Run
instead of including process startup and teardown. Use the named diagnostic
build and a separately built, CRC-validated workload:

```sh
node tests/bench/coremark/profile.mjs \
  build-em-cpu/examples/browser/dist/arm_jit.js \
  /tmp/coremark-long/coremark_a64.bin a64 2 \
  /tmp/dynarmic-cpu/a64-run.cpuprofile \
  > /tmp/dynarmic-cpu/a64-sample.txt
node tests/bench/cpu_profile.mjs /tmp/dynarmic-cpu/a64-run.cpuprofile \
  --root '^a64_' > /tmp/dynarmic-cpu/a64-guest-samples.json
```

The profiler enables the existing `_dynarmic_wasm_debug_names(1)` switch before
initialization and Run, starts immediately before `_arm_run(0x10000)`, and stops
immediately after it returns or throws. The normal `COREMARK_SAMPLE` record is
retained on success. A guest fault still retains the raw profile but emits no
successful sample. Output files are created exclusively and are never
overwritten. Captures are non-reentrant within a process; the reusable helper is
`captureCpuProfile(output, callback, {interval})` in
`tests/bench/scoped_cpu_profile.mjs`, with a default interval of 100 microseconds.
It supports synchronous and asynchronous callbacks and preserves callback and
cleanup errors.

Generated names identify frontend, guest entry PC and multi-block region status
(`a32_*` / `a64_*`, with `_region` where applicable). Select `^a32_`, `^a64_` or
`^a(32|64)_` as appropriate. The switch is off during ordinary execution, so
production generated bytes are unchanged; diagnostic modules contain additional
name metadata. Run-scoped sampling still changes execution and tiering timing,
and the instrumented `runMs` is not a benchmark result. Very short or failing
callbacks can produce too few samples for the analyzer even though the raw
profile was retained. Use long workloads and multiple fresh diagnostic processes
before selecting an optimization, then verify it with ordinary Release reports.

The first [named compiler cohort](../internals/measurements/2026-10-23-named-cpu/README.md)
contains five fresh processes each for A32/Thumb/A64 fixed kernels, the 4,096
entry population and A32/A64 CoreMark. It identifies A64 runtime decode-table
construction as the next frontend experiment; these instrumented samples are
not before/after speedups or publishable CoreMark scores.

The follow-up [Run-scoped application cohort](../internals/measurements/2026-10-24-runtime-cpu/README.md)
retains six complete long-workload profiles, qualified fixed/CoreMark baselines,
matching generated WAT and a V8 tiering trace. Its offline verifier recomputes
all sample summaries and validates the retained benchmark protocols.

On V8 versions that expose it, `node --trace-wasm-compilation-times <host.js>
<args>` records Liftoff/TurboFan events relative to host sample output. This
is engine-specific diagnostic output, not a portable tier-completion signal
or an uninstrumented latency measurement. Keep its engine version and raw log.

For emitted-code review, run a separate host with `DYN_DUMP_DIR=<existing-dir>`
and disassemble its generated modules with the SDK's `wasm-dis`. Compare code
structure and embedded addresses separately; allocator and static-data layout
changes can relocate memory and link slots without changing guest operations.
Dumping and sampling must be disabled during the performance comparison.

## Benchmark v2

```sh
node tests/bench/run.mjs build-em/tests --runs 5 --output before.json
node tests/bench/validate.mjs before.json
# Build the candidate, finish correctness tests, then measure it separately.
node tests/bench/run.mjs build-em/tests --runs 5 --output after.json
node tests/bench/compare.mjs before.json after.json > comparison.json
# Profile runs use a separately configured build tree.
node tests/bench/run.mjs build-em-profile/tests --runs 5 --output profile.json
node tests/bench/validate.mjs profile.json --require-profile
```

For a controlled follow-up to noisy comparisons, interleave unchanged builds
using report templates collected by the ordinary runners:

```sh
node tests/bench/paired_run.mjs fixed <before-build>/tests <after-build>/tests \
  before.json after.json /tmp/paired-fixed --runs 25
# The same collector supports cache-population-v1:
node tests/bench/paired_run.mjs population <before-build>/tests <after-build>/tests \
  before-population.json after-population.json /tmp/paired-population --runs 25
```

Apply the same CPU affinity and `NODE_OPTIONS` used for the templates. The
collector verifies runtime, host, upstream, workload and executable identities,
rotates cases and alternates build order each round. It retains every sample
and writes both reports plus their comparison. Existing cohort files are never
overwritten. Exit codes are 0 for a passing comparison, 1 for a compatible
regression (reports retained), and 2 for invalid input or collection failure.
Child-process failures retain stdout, stderr, status and completed samples in
a separate failure JSON; this partial diagnostic is not a benchmark report.
Git/build metadata is retained from the templates; this remains artifact
identity verification, not a build-time source attestation. Paired collection
does not change warm-up policy or certify completed engine tiering.

`benchmark-v2` intentionally supersedes v1: stricter validity and aggregation
rules make old artifacts unsuitable for automatic comparison. Regenerate old
baselines; do not relabel their schema.

The A32, AArch64 and Thumb memory kernels use separate source files and report
their architecture/memory mode explicitly. Their instruction counts are not
interchangeable; compare only the same named case. Each case runs in at least
five fresh processes for a performance comparison.

The browser host has a separate `dynarmic.js/browser-benchmark-v1` artifact:
`node examples/browser/bench.mjs <build>/examples/browser 3 --json browser.json`.
It records ARM32/AArch64 thunk and flat-window samples for the integer LCG,
guest-RAM sieve and FP64 pi kernels, plus artifact hashes and correctness.
Validate it with `node tests/bench/browser_validate.mjs browser.json`.
This collector runs the browser host's compiled module under Node and records
the Node/V8 version. It does not measure the browser page. Its reports remain
separate from the fixed host-JIT suite because workloads and lifecycle
boundaries differ.

The browser Worker compatibility gate runs the demo under Chromium and
Firefox. It consumes the same native reference output and checks both A32 and
AArch64 guests. These Worker checks establish compatibility, not qualified
browser throughput. Cross-browser timing comparisons still need equivalent
warm-up and tiering protocols.

Cache pressure uses a separate `dynarmic.js/cache-benchmark-v1` artifact:

```sh
node tests/bench/cache_run.mjs build-em/tests --runs 5 --output cache.json
node tests/bench/cache_validate.mjs cache.json
node tests/bench/cache_run.mjs build-em-profile/tests --runs 5 --output cache-profile.json
node tests/bench/cache_validate.mjs cache-profile.json --require-profile
node tests/bench/cache_compare.mjs cache-before.json cache-after.json > cache-comparison.json
```

Its three A32 cases patch one instruction, invalidate its closed range, and
rerun the guest for eight cycles. Every cycle must compile a module, while
cycles after the first must report exactly one invalidation in profile mode.
This intentionally measures code-cache destruction and reconstruction; it is
not mixed into benchmark-v2, whose warm phase requires zero compilation. The
report retains per-cycle wall time, compile bytes, invalidation counters and
the profile cache peak so repeated self-modifying-code costs can be reviewed
independently from steady-state throughput.
`cache_compare.mjs` requires at least five fresh processes and matching host,
engine, build, upstream and workload identity. It gates total invalidation-run
time and every cycle's median time at +15%, compiled bytes at +10%, and profile
cache peak bytes at +10%. It emits `dynarmic.js/cache-comparison-v1` and exits
0 for a pass, 1 for a compatible regression, or 2 for invalid/incompatible
reports.

Cache population has an independent `dynarmic.js/cache-population-v1` report:

```sh
node tests/bench/cache_population_run.mjs build-em/tests --runs 5 --output population.json
node tests/bench/cache_population_validate.mjs population.json
node tests/bench/cache_population_run.mjs build-em-profile/tests --runs 5 --output population-profile.json
node tests/bench/cache_population_validate.mjs population-profile.json --require-profile
node tests/bench/cache_population_compare.mjs before-population.json after-population.json
```

The 12 cases combine 1, 64, 512 and 4,096 independent A32 entries with three
memory configurations. Each entry sets a checked register result and halts at
SVC. An explicit back edge after SVC bounds speculative translation to that
entry. These tiny guests do not access guest data, so the memory configurations
exercise emitter configuration rather than memory throughput. A fresh process
constructs one JIT per case, fills its cache, then revisits every entry once.
The cold pass must compile exactly one module per entry; the second pass must
have zero compilation. Both passes check every result. Profile validation also
requires no invalidation and an exact peak of the population's module count and
emitted bytes. This catches cache-population and retention regressions that a
single-module invalidation loop cannot expose.

Pass timing includes reset, result checking and Run/profile publication, and
excludes JIT construction. The second pass is a cache-hit latency measurement;
it is not certified engine steady state. Reports retain both raw samples per
process, medians and median absolute deviations, compile phase totals, artifact
and workload hashes, host/engine/toolchain identities, flags and CPU affinity.
Cases rotate each round. The runner rejects failed processes and artifacts
changed during collection. The comparator requires five fresh processes and
matching experiment identities, with +15% cold/warm pass and compile latency
budgets and +10% emitted/peak byte budgets. Its exit codes are 0 for pass, 1
for a compatible regression and 2 for invalid/incompatible reports.

The peak is **emitted Wasm bytes**, not engine machine code, JS heap, RSS or a
configured cache memory ceiling. This synthetic A32 population does not cover
large application working sets, eviction, A64/Thumb populations or concurrent
instances. Those remain release work. These very short per-entry executions
also make host/profile overhead visible; inspect dispersion before interpreting
a timing gate failure.

For the fixed `benchmark-v2` suite described above:
Every process performs one cold Run and eight subsequent warm Runs of the
same cached JIT. All observations are retained. These warm samples include
engine tiering; they are not certified steady-state samples. The report records
`warmRunsPerProcess: 8`, labels each observation with `runIndex` (`0` for cold,
`1..8` in warm execution order), and publishes `summary.warmIndexMedianMs` so
that tiering-shaped curves can be inspected across fresh processes. It
explicitly marks `observableEngineEvent: false`:
Node/V8 has no portable completion event for optimization of the inner modules.
Case order rotates
between rounds. Reset and result checking are outside the Run timer; profile
publication, when enabled, is inside it.

Artifacts contain:

- CPU/OS, Node/V8, inherited `NODE_OPTIONS`, Linux CPU affinity when available, configured Clang/Emscripten versions, CMake build type and
  global/configuration compiler/linker flags, explicit profile mode;
- checkout/upstream revisions and dirty status, JS/Wasm SHA256, workload-source
  SHA256 (host source plus measurement helper), architecture and memory mode;
- per-process samples, exact instruction counts, wall time, throughput,
  compile counters and optional profile deltas;
- median cold wall time, median sum of compile phases, median emitted bytes,
  median of each process's warm median, minimum warm sample, median absolute
  deviation of the process warm medians and the median wall time at each warm
  run index. Profile reports additionally summarize cold cache high-water marks
  as `peakCachedBlocksMedian` and `peakCachedBytesMedian`; production reports
  set these two fields to `null`.

Compile phases are translation, IR optimization, byte emission, engine module
construction, instance construction and table registration. Their sum is not
cold Run wall time: guest execution and uninstrumented planning/runtime work
also contribute to the latter. Engine background optimizing compilation is
not fully captured by synchronous module construction time.

Validation rejects missing/duplicate cases or processes, invalid metadata,
inconsistent instruction counts/throughput, partial profiles, changed profile
mode, wrong phase order, nonzero warm compile activity and incorrect summaries.
A child process must exit successfully, produce all nine samples and report
PASS; timings alone cannot certify a correct run.

## Regression budgets

The comparator validates both artifacts, requires at least five processes per
case, and requires matching host/engine/build/methodology/workload identities.
Executable hashes and source revisions may change: they identify the compared
implementations, not the workload. Workload-source changes require a new
baseline or an explicitly reviewed migration.

A case fails if median warm throughput falls by more than 5%, median emitted
bytes grows by more than 10%, or the median compile-phase sum grows by more
than 15%. The CLI emits `dynarmic.js/comparison-v1` and exits with:

| Exit | Meaning |
| --- | --- |
| 0 | Compatible experiment, all budgets passed |
| 1 | Compatible experiment, at least one regression |
| 2 | Invalid/incompatible artifact or invocation |

These are review gates, not statistical significance or cross-machine speed
claims. Inspect raw samples and dispersion; repeat an unstable experiment
without discarding its original failure. No shared CI runner is a trusted
performance host. Dedicated hardware/browser baselines, sustained warm-up,
peak/live cache memory, latency percentiles and competitor comparisons remain
release work.

## CoreMark complete workload

The [CoreMark protocol](../tests/bench/coremark/README.md) adds A32/A64 × three
memory modes with fixed 2K performance seeds. Every sample starts a fresh Node
process/module/JIT and runs the complete guest once. Native output is an
independent CRC reference, never a speed comparator. Reports bind compiler,
source, workload and JIT artifact identities; retain console output, compile
phases and profile cache peaks; and report kernel/Run medians and MADs.

Short runs permit only the upstream minimum-duration error and still require
all CRC and process/guest-exit checks. `--require-duration` additionally requires
every guest sample to meet ten seconds. Duration qualification is not
certification. The kernel interval includes compilation misses and engine
tiering, so it must not be described as steady-state throughput. The comparator
requires five processes and compatible environments, and applies the existing
5% throughput / 15% latency / 10% byte-growth budgets. The default strict gate
explicitly reports CoreMark as uncovered; `--coremark WORKLOAD_DIR` makes its
correctness collection mandatory for that invocation. Release performance
claims additionally need the duration gate and controlled comparative runs.

The first [workload evidence](../internals/measurements/2026-10-22-coremark/README.md)
establishes correctness and duration behavior, not an optimization speedup.
