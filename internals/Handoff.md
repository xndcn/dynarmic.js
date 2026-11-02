# Engineering handoff

Updated: 2026-10-28. Active branch at initial audit: `a64`.
Starting revision: `c18fae5` (clean working tree).

## Resume protocol

Read this file, [Roadmap.md](Roadmap.md), [Architecture.md](../docs/Architecture.md),
and [Performance.md](../docs/Performance.md). Check `git status` and recent commits
before editing. Preserve unrelated changes. Update this file at every milestone
with commands, results, remaining risks and the next concrete task. Commit
verified progress. Do not mistake this handoff for an external scheduled job:
continuation requires an active development session.

## Environment

Existing local build trees: `build-native` (Release), `build-em` (Release,
profile OFF), `build-em-profile` (Release, profile ON). Emscripten toolchain is
under `/home/xiong/sandbox/emsdk`; Boost headers are `/tmp/boost-headers`.
These paths are local conveniences, not portable setup instructions. README
contains the clean-build procedure. Upstream is an unmodified submodule.

## Current work

Initial audit completed across architecture/performance docs, build files,
benchmark protocol/hosts, shared dispatcher/cache, frontend interfaces,
compile/profile publication and structured CFG emission. Existing advanced
optimizations are real; release readiness and comparative performance are not
established. See Roadmap audit findings rather than assuming older design
proposals are already implemented.

Verification:

- `node --test tests/bench/*.test.mjs`: 66 tests passed.
- Native, Emscripten and profile builds completed successfully.
- `ctest --test-dir build-native --output-on-failure`: 2/2 passed.
- ASan/UBSan planner executable: passed exhaustive 65,536 graph corpus.
- `tests/regress.sh build-em build-native`: `regression done fail=0`.
- Profile host (`build-em-profile/tests/jit_bench.js 2`): 9/9 samples carried
  valid profile-v1 deltas.
- Lifecycle boundary audit fixed zero-length and end-of-address-space
  invalidation wraparound in both frontend interfaces; `instance_test` now
  exercises the cases for A32 and A64. The post-fix host test passed.
- Follow-up corrected the A32 saturation expression for `size_t` maximum
  lengths and added explicit maximum-length cases; the rebuilt instance host
  still passes for both architectures.
- Added deterministic AArch64 and Thumb memory benchmarks with thunk,
  page-table and flat-window modes. The v2 runner now validates 10 cases;
  all three modes of both new hosts passed and a one-process expanded report
  validated successfully.
- Profile AArch64 benchmark host passed 9/9 samples with complete
  `profile-v1` deltas after the profile build was reconfigured for the new
  targets.
- Compile/profile ownership is now per JIT: `CompileStats` receives engine
  counters through the runtime boundary, and Emscripten keeps snapshots in
  `dynarmicWasmStats.instances[id]` with `activeInstance` plus legacy mirrors.
  The two-instance host asserts that both A32 and A64 snapshots are retained;
  production and profile builds pass.
- Full `tests/regress.sh build-em build-native` after the benchmark expansion
  completed with `regression done fail=0`, including all available fuzz loops.
- Full regression after the per-instance stats change also completed with
  `regression done fail=0`; no guest, IR, SMC, browser or differential-fuzz
  failures were observed.
- The new fail-closed command `tests/regress.sh --strict build-em build-native`
  also completed with `regression done fail=0`, including five-process
  benchmark collection and all installed fuzz/browser requirements.
- Added `lifecycle_test` for A32/A64 external halts, idle cache clears,
  single-step progression, callback-triggered cache clear plus user halt, and
  reset state. Production and profile hosts both pass.
- Lifecycle coverage now also asserts that `Step` with a pre-existing user
  halt executes nothing and returns the original halt bit without `Step`.
- Added `tests/fuzz/replay.py` and `--save-corpus` support to both differential
  fuzzers. A32/A64 exact-state replay smoke cases pass; the generated corpus
  remains outside the repository until a minimized semantic failure is found.
- Added `tests/bench/overhead.mjs` with schema
  `dynarmic.js/profile-overhead-v1`; profile ON/OFF instrumentation cost is
  now measured separately from implementation regression comparisons.
- Formalized the browser demo benchmark as
  `dynarmic.js/browser-benchmark-v1`; it records A32/A64 thunk and
  flat-window LCG/sieve/FP64 samples with artifact hashes and is validated by
  the regression gate.
- Browser report smoke run passed with four architecture/memory cases and the
  existing native comparison output; browser schema tests are included in the
  CTest benchmark protocol target.
- Full `tests/regress.sh build-em build-native` after browser report wiring
  completed with `regression done fail=0`.
- Added paired AArch64 scalar/NEON arithmetic benchmark cases to benchmark v2;
  a one-process 12-case report validates and both fixed hosts produce PASS.
- Full `tests/regress.sh build-em build-native` after the NEON workload change
  completed with `regression done fail=0`.
- Extracted `RegionSelector` from `AddressSpace`; it now owns only bounded
  successor exploration and ordering through explicit callbacks, leaving
  cache/link/invalidation ownership in `AddressSpace`. Native CTest and
  Emscripten memory smoke pass after the extraction.
- Added `region_selector_test` with synthetic IR blocks covering compatibility,
  guest-distance, max-block and zero-extension-budget behavior; native CTest is
  now 3/3.
- Full `tests/regress.sh build-em build-native` after selector tests and
  extraction completed with `regression done fail=0`.
- Added a dedicated browser Worker workflow using Emscripten, Chromium,
  `verify_chrome.js` and browser benchmark schema validation; it is scoped to
  browser/backend changes and can also be run manually.
- Added append-only region profitability counters to profile-v1. The selector
  reports queued candidates and compatibility, block-budget, guest-distance,
  and IR-budget rejections; AddressSpace aggregates them per JIT with the
  existing zero-cost profile macros when profiling is disabled. Synthetic
  selector coverage and the benchmark protocol now validate all 19 counters.
- Verification for the selector profile extension: native build and all 3 CTest
  targets passed; `node --test tests/bench/*.test.mjs` passed 66/66; Release,
  profile and Emscripten benchmark smoke builds passed; and
  `tests/regress.sh build-em build-native` completed with `regression done
  fail=0`. The profile smoke report exposed nonzero candidate/distance counts
  on the cold A32 flat-window run and zero deltas on warm cache hits.
- The strict release gate was rerun after this extension:
  `tests/regress.sh --strict build-em build-native` completed with
  `regression done fail=0`.
- Benchmark v2 samples now carry a validated `runIndex` and reports expose
  `warmIndexMedianMs`, making the eight-run warmup/tiering curve inspectable
  without claiming a portable V8 optimization completion event. All five host
  benchmark programs were updated together; protocol tests now pass 68/68 and
  both Release and profile Emscripten targets rebuild cleanly.
- Added append-only `profile-v1` cache high-water marks (`peakCachedBlocks`,
  `peakCachedBytes`). AddressSpace now tracks live emitted module bytes across
  range and full invalidation, while the peak values remain monotonic for
  benchmark deltas. Protocol fixtures and Performance.md document the fields;
  `node tests/bench/run.mjs build-em-profile/tests --runs 1 --output
  /tmp/profile-cache.json` followed by `validate.mjs --require-profile`
  produced nonzero cold peaks for all 12 cases and zero warm deltas.
- `tests/regress.sh --strict build-em build-native` completed with
  `regression done fail=0` after the cache profile extension. Native CTest
  remains 3/3 and the benchmark protocol remains 68/68.
- Benchmark-v2 summaries now carry profile cache peaks as
  `peakCachedBlocksMedian`/`peakCachedBytesMedian` (or `null` for production).
  Validation rejects a cold profile whose peak is below the modules/bytes it
  compiled and rejects nonzero warm peak deltas. Overhead reports preserve the
  same production/profile fields for cache-pressure review.
- The updated protocol suite passes 70/70; one-process production and profile
  reports both validate, and the profile summary reports nonzero cache peaks
  for all 12 fixed cases.
- Added `dynarmic.js/cache-benchmark-v1` with `jit_cachebench`: three A32
  memory modes run eight patch/invalidate/recompile cycles. Production and
  profile one-process reports validate; profile samples show one invalidation
  and one compiled module on each cycle after the cold cycle. The new cache
  protocol has four focused tests and is included in native CTest.
- Added a native ASan/UBSan workflow and reproducible full sanitized build
  recipe. The temporary Debug build completed with CTest 3/3; the CI scope is
  explicitly native planner/selector/IR/protocol coverage and does not claim
  Emscripten or browser sanitizer coverage.
- Added root MIT license text and `THIRD_PARTY_NOTICES.md`. The inventory
  records the current Dynarmic submodule revision, bundled dependency license
  evidence and which components the wasm configuration actually links. A final
  tagged-archive license review remains required; no third-party notice was
  changed.
- Added six checked-in `fuzz-case-v1` semantic cases covering A32 ARM/Thumb,
  A32 memory, A64 arithmetic/memory and A64 NEON. `check_corpus.py` replays
  every case through Unicorn and the Emscripten hosts; all six pass in modes
  0/1/2 as recorded. The strict gate and browser workflow now execute this
  corpus instead of treating saved JSON as passive documentation.
- After the corpus integration, `tests/regress.sh --strict build-em
  build-native` completed with `regression done fail=0`; the corpus replay
  stage ran after the full differential fuzz matrix.
- Install audit found and fixed an embedded-upstream CMake package bug:
  `BUILD_SHARED_LIBS` is now explicitly `OFF`, so generated
  `dynarmicConfig.cmake` emits dependency lookups. Added a downstream consumer
  smoke project and install script; it checks configure, compile, link, run,
  CMake config and installed license files.
- The consumer smoke initially caught a target-name mistake and now links the
  exported upstream target `dynarmic::dynarmic`. The corrected smoke passes
  after install, including dependency discovery, root license, and the four
  linked bundled-component license files.
- Added browser-neutral Worker verification via
  `examples/browser/verify_browser.mjs`: the page reports `DONE PASS/FAIL`
  through an HTTP callback, and CI now runs this smoke under Firefox ESR in
  addition to the existing Chromium DevTools check. This validates execution
  compatibility, not cross-engine performance equivalence.
- Added `cache_compare.mjs` and `cache-comparison-v1`. Cache-pressure reports
  now have a strict before/after gate requiring five fresh processes and
  checking total time, every invalidation cycle, compiled bytes and profile
  cache peaks. The protocol suite now passes 79/79, including malformed
  provenance rejection.
- Firefox smoke initially exposed two browser/runtime integration defects:
  Worker stats subtraction attempted to coerce registry/function properties,
  and the browser demo subtracted the previous architecture's top-level
  compile mirror from a fresh JIT. Numeric-only instance snapshots now report
  nonnegative per-architecture compile values; Chrome and Firefox smoke both
  pass after the fix.
- Benchmark v2 reports are in `/tmp/dynarmic-release-evidence` for this local
  session. The initial unpinned five-process comparison reported flat-window
  warm throughput at 94.3%, which failed the review gate. A controlled four-P
  core, 25-process sequence measured candidate/baseline flat-window ratios of
  97.9% and 103.2% in two candidate rounds; the two baseline rounds differed by
  9.7%. Emitted bytes and compile budgets remained stable. Treat the initial
  result as scheduler/frequency noise, not a code regression or a performance
  claim. Keep CPU affinity and dispersion in future paired artifacts.

Delivered commits include `e676525` (architecture/release plan), `6073a15`
(CFG planner and benchmark gates), `20166f3` (A64/Thumb benchmarks),
`23377a7`/`44fca66` (overflow-safe invalidation), `a5aa9ea` (per-instance
compile/profile stats), `bd041f6` (instance snapshot lookup helper), `d8b7552`
(Run/Step and cache lifecycle tests), `67e8f36`/`087376f` (exact fuzz corpus
replay), `26c5edb` (profile overhead measurement), `18eb18c` (browser
benchmark reports), `b23955a` (paired AArch64 NEON workload), `f1fd1bd`
(browser Worker CI), `1d68eb1` (Step halt semantics), `e7e5de7`/`e0ba986`
(region selection split and budget tests), `0c87e68` (strict release
regression gate), `cf6f034` (cache high-water profile counters), `82b577b`
(cache peak benchmark validation), `40579b1` (cache invalidation pressure
workload), `8e01bbb` (native sanitizer CI), and `5ad97fb` (license/dependency
  inventory), `beed8a8` (release audit record), `1ad2984` (checked-in semantic
  fuzz corpus and replay gate), `574d5ca` (cache-pressure before/after
  comparator), and `112c1d1` (cache report provenance validation).
The performance signal is under active paired-build investigation; do not
consider this handoff a release sign-off.

## Cache population milestone (2026-10-22)

Resumed from clean `319fb79`; the preceding work is verified progress (cache
report provenance validation was committed). The unchanged tree first passed
`tests/regress.sh --strict build-em build-native` again.

Added `jit_cachepopbench` and the `cache-population-v1` runner, validator and
comparison gate. Twelve cases cover 1/64/512/4,096 live A32 entry modules in
three memory configurations. Every entry is checked on a cold fill and a warm
revisit; the revisit must compile nothing. Profile reports assert exact module
and emitted-byte peaks and no invalidation. The strict regression now collects
five fresh processes for this suite too. See Performance.md for timing scope,
identity checks, budgets and limitations.

Verification and evidence:

- Both Release and profile Emscripten targets build and run. Native build and
  CTest pass 3/3; `node --test tests/bench/*.test.mjs` passes 114/114 (35 new
  population protocol/comparison tests).
- Five-process production and profile reports each pass all 12 cases. Local
  raw files are `/tmp/dynarmic-release-evidence/cache-population/production.json`
  and `profile.json`; these temporary files are not a durable release archive.
  Commands: `node tests/bench/cache_population_run.mjs <build>/tests --runs 5
  --output <file>`, then `cache_population_validate.mjs <file>` (add
  `--require-profile` for the profile build).
- At population 4,096, profile reports retain exactly 4,096 modules and
  2,401,792 emitted bytes; every warm pass has zero compilation. On this
  i7-12700 / Node v24.19.0, the five-process thunk cold/warm medians were
  354.21/5.09 ms in production and 380.35/10.78 ms with profiling. Affinity was
  unrestricted `0-19`; these are initial diagnostic observations, not an
  optimization or cross-machine performance claim.
- Comparator CLI exit codes 0/1/2 were checked against identical production
  reports, a synthetic doubled-latency report, and incompatible production vs
  profile reports. This verifies the gate, not an implementation improvement.
- The initial guest omitted a post-SVC back edge; speculative discovery then
  translated padding and neighbouring entries (510,731,793 emitted bytes at
  4,096 entries). The delivered guest has an explicit back edge, so each
  module owns only its own tiny region. Do not remove it when extending this
  workload. This was a benchmark construction correction, not a JIT speedup.
- The expanded `tests/regress.sh --strict build-em build-native` completed
  with `regression done fail=0`, including five-process population collection
  and the full differential fuzz matrix. The browser CI workflow now runs one
  process per population case and uploads the raw report; the workflow change
  is committed configuration, not a claim that hosted CI has run in this session.

Remaining scope: generated-byte peaks do not measure engine memory or enforce
a cache budget. Add selective invalidation within a populated cache, bounded
eviction, application working sets and A64/Thumb populations. The new timing
data also warrants CPU profiling of first-entry translation and repeated
Run/snapshot publication before choosing an optimization: one cold sample spent
31.13 ms in translation but only 0.08 ms constructing the generated module.
Separate frontend initialization from engine tiering before attributing cost.

## ARM decoder optimization milestone (2026-10-22)

Resumed from clean `e77b4b1`; the preceding cache-population delivery is verified
progress. First-entry profiling identified runtime ARM decode-table building
and matcher allocation. Added the frontend adapter
`src/dynarmic/frontend/wasm/arm_decoder.h` and a translation-unit-only patch.
It generates compact bucket offsets/IDs at compile time and uses constant
function-pointer handlers specialized through upstream argument decoding.
The upstream submodule stays unchanged. No runtime matcher construction,
allocation or `std::function` remains in the ARM adapter.

The initial indexed design still used runtime `std::function` matchers. Its
warm cache-population regressions persisted in a 25-round paired experiment.
V8 trace revealed a roughly 59 KB TranslateArm body containing construction
code; a noinline factory was re-inlined by Binaryen. The delivered constant
dispatch design reduces that body to 1,444 bytes. Do not restore lazy matcher
construction without rechecking this engine-compilation cost.

The durable [experiment record](measurements/2026-10-22-arm-decoder/README.md)
contains raw compressed reports, including failed intermediate comparisons,
CPU samples, V8 traces, source/artifact hashes and a paired collection script.
Local build/evidence scratch files remain under
`/tmp/dynarmic-release-evidence/decoder`; the important benchmark evidence is
also committed under `internals/measurements/2026-10-22-arm-decoder`.

Final static-dispatch verification:

- Native Release and full native ASan/UBSan builds pass CTest 4/4. New coverage
  compares all 4,096 ordered buckets against upstream (4,919 candidates,
  261 rules), every rule's condition/bit mutations, one million seeded random
  words, and 65,536 typed argument-dispatch cases plus zero-argument/const calls.
- Release and profile Emscripten builds succeed; all twelve profile population
  cases validate. Chrome 133 and Firefox Worker smoke pass for the final build.
- `tests/regress.sh --strict build-em build-native` passes on the final static
  dispatch build (`regression done fail=0`), including the full differential
  fuzz matrix. An earlier intermediate-design run was invalidated by editing
  the shell script while it was executing; its error log is retained. The
  script was then kept fixed for both complete reruns, which passed.
- The final 15-process fixed-suite and 25-round paired population comparisons
  both pass existing budgets. A32 memory cold compile-phase medians are
  0.520–0.556 of baseline; single-entry cold population wall time is
  0.356–0.407 of baseline. Generated module bytes are unchanged. The A32 host
  Wasm artifact shrinks by 45,432 bytes (2.55%).
- Not every warm metric improves: the 4,096-entry thunk revisit is +14.0%
  (7.931 to 9.038 ms), within the existing +15% budget. Keep this visible when
  investigating publication/engine-tiering overhead. No application or
  browser-engine leadership claim follows from these synthetic measurements.

Next use the retained profiles to investigate remaining Run/snapshot publication
cost and the other frontend decoders. Preserve source/build identity and failed
samples; do not widen budgets to compensate for engine-tiering effects.

## Statistics publication milestone (2026-10-22)

Resumed from clean `4fe4b0e`; the preceding decoder optimization is verified
progress. CPU sampling of 262,144 cached Runs identified the publication import
and GC as major costs, especially with profiling enabled. Replaced duplicated
publishers with one implementation: unchanged active compile totals reuse the
existing snapshot, instance switches restore all mirrors, and profile counters
are emitted with a fixed object shape from current heap views. u64-to-Number
conversion uses exact 32-bit halves with the same rounding as before.

Added `stats_test` for every compile/profile field, source-memory preservation,
historical leaf immutability, equal-total instance switches, phase-only and
profile-only changes, null counters, large u64 values and actual heap growth.
Member-name oracles are independent of serialized offsets. Both production
and profile builds pass this test plus the existing instance/lifecycle tests.

Added `tests/bench/paired_run.mjs` for fixed and population suites. It validates
template/build/runtime/workload identities, alternates builds, rotates cases,
retains all observations, rejects overwrites and saves failed-child logs plus
already completed samples. Synthetic CLI tests verify order, both schemas,
regression/error statuses, artifact changes, upstream mismatch and failure
after PASS. Native CTest passes 4/4; protocol tests pass 119/119.

Performance evidence:

- The initial 15-process production fixed comparison failed A32 page-table
  warm throughput at 0.943 of baseline. The 25-round paired comparison passes
  all budgets, with that case at 1.015. The failed report is retained.
- Production population comparison passes at 15 processes. The initial
  five-process profile population cohort failed one cold/compile metric; its
  25-round paired follow-up passes all budgets. Profile cached-revisit latency
  is 0.529–0.752 of baseline across the twelve cases. Generated bytes and cache
  peaks are unchanged. Do not attribute unrelated fixed-kernel fluctuations
  to this boundary optimization.
- Raw reports, CPU samples, source/artifact hashes and reproduction commands
  are committed in [the experiment record](measurements/2026-10-22-publication/README.md).
  Scratch builds and evidence are under `/tmp/dynarmic-release-evidence/publication`.

Both `tests/regress.sh --strict build-em build-native` and the corresponding
`build-em-profile` command complete with `regression done fail=0`. Chrome and
Firefox Worker checks pass in both configurations. CI is configured for both
profile modes and the new snapshot test; hosted CI was not run locally.
The Firefox smoke driver now clears its timeout after completion.

Treat published JS records as read-only; compile/profile leaves preserve prior
values, while registry entries point to the latest publication. No public
schema or counter semantics changed. Remaining work includes compile-stage
FFI overhead, application working sets, populated-cache invalidation, memory
budgets and cross-engine performance evidence. These results are not a release
sign-off or a fastest-engine claim.

## Next concrete tasks

1. Extend the controlled benchmark protocol to report engine tiering markers
   where an engine exposes them, and add larger application workloads; do not
   change JIT code for the unreproduced flat-window signal.
2. Add additional Chromium Worker coverage and a Firefox browser benchmark
   artifact with an explicit warm-up/tiering policy before making a versioned
   cross-engine performance claim.
3. Add selector shape/budget benchmark reports and cache invalidation pressure
   workloads, keeping cache ownership separate from selection policy. The
   single-module invalidation and 4,096-module population workloads are now
   delivered; combine population with selective invalidation and measure engine
   memory before designing cache limits. ARM first-entry table construction is
   now addressed, as is repeated snapshot allocation. Use profiles to examine
   remaining decoders and the JS string/map path in compile-phase accounting;
   prioritize application working sets and populated-cache invalidation too.
4. Extend sanitizer coverage to runtime hosts where the toolchain supports it.
   Recheck the license inventory against the exact tagged archive before
   packaging or publishing.

## CoreMark workload milestone (2026-10-22)

Resumed from `08fb914`. Prioritized the missing complete-workload baseline
before changing compile-stage JS crossings. No compiler/backend optimization
was made in this milestone. `RecordCompilePhase` still converts phase strings
and maps offsets through JS; typed phases/direct C++ updates are a future
experiment requiring measured before/after evidence.

The old CoreMark script returned success despite upstream's duration error and
`Errors detected`. Replaced it with a source/binary manifest builder, a fresh
process collector, strict output/CRC/duration validator and comparison gate.
Six cases cover A32/A64 × thunk/page-table/flat-window. Native execution is a
CRC oracle only. Sources remain external at pinned upstream revision
`1f483d5b8316753a742cbf5590caf5bd0a4e4777`; source files, compiler commands,
binaries and local port/builder files are hashed. Only the documented UART stub
is adapted in the build tree. No third-party source is committed.

The browser host also incorrectly returned normal exit for interpreter fallback
and guest exceptions. These now return `UserDefined2`; existing semihost exit
(including unknown SVC) remains `UserDefined1`. Worker, browser benchmark and
CoreMark child reject abnormal halts. `examples/browser/fault_test.mjs` exercises
normal/UDF-or-BRK/normal across both architectures and all three memory modes.
Production and profile both pass 18 runs.

The shared benchmark environment helper removes duplicated provenance logic;
existing fixed-report schema is unchanged. CTest and browser CI include the new
protocol/collector/environment tests. The regression option
`--strict --coremark /absolute/workload-dir` requires the external correctness
workload and retains its report outside the temporary regression directory.
Without it, regression explicitly reports that this application gate is not
covered. CI fetches the pinned source and collects short reports in both profile
modes, but hosted CI was not run during this local session.

The [durable evidence](measurements/2026-10-22-coremark/README.md) retains:

- Production/profile short reports, five fresh processes × six cases. Every
  CRC passes; all runs are below ten seconds. These are correctness/diagnostic
  samples, not publishable CoreMark scores.
- Production 200,000-iteration report, one fresh process × six cases. Kernel
  durations 22.361–67.770 seconds pass the duration gate. This checks the gate;
  one sample per case is not enough for a comparative performance claim.
- Expected rejection of the short report by `--require-duration`, and the
  exact browser host patch relative to the base revision.

Local artifacts: `/tmp/dynarmic-coremark-source`,
`/tmp/dynarmic-coremark-v1-final` (4,000 iterations),
`/tmp/dynarmic-coremark-long` (200,000 iterations), and
`/tmp/coremark-{production,profile,duration}.json`. The earlier
`/tmp/dynarmic-coremark-v1` was an intermediate manifest without the builder
self-hash; do not use it. All builders/collectors reject overwrites. The initial
UART-stub regex failed safely and was fixed before these artifacts were built.

Protocol verification: 159 tests pass, including subprocess failure after valid
output, retained short report after duration rejection, binary mutation,
CRC/error/summary tampering, profile counters and comparison budgets. Native
CTest passes 4/4 after reconfiguring to include the new test files.

Chrome 133 and Firefox Worker checks pass in both profile configurations. These
browser smoke checks overlapped differential fuzzing, so their timing output
is not performance evidence. The separately archived CoreMark collections ran
without competing builds/tests. The fresh wrapper build/collection passes, and
its overwrite rejection returns status 2. The production strict regression with
`--coremark /tmp/dynarmic-coremark-v1-final` passes (`regression done fail=0`);
its CoreMark report is retained at `/tmp/tmp.NnWMqnjlnY/report.json`.

Next concrete task: use the complete-workload baseline alongside fixed and
population suites to evaluate typed/direct C++ compile-phase accounting. Profile
before deciding whether its JS string/map crossings justify a change; require
controlled paired before/after cohorts. Other open work includes Thumb
application coverage, populated-cache selective invalidation, engine memory
budgets and browser performance/tiering artifacts. This milestone is not a
release sign-off or performance-leadership claim.

The profile strict regression also passes with the same explicit CoreMark
workload (`regression done fail=0`); its retained report is
`/tmp/tmp.Ubvzn0nupe/report.json`. Both complete regression logs are archived
with the milestone. No build artifacts or executing regression scripts were
modified during either run. This is a verified local milestone; all listed
remaining tasks stay active.

## Compilation boundary milestone (2026-10-23)

Resumed from clean `87732d2`. The implementation and evidence are complete;
this session stops after committing this milestone at the user's request.
The long-term release/performance objective remains unfinished and paused.
Do not automatically start the following tasks without a new user instruction.

Delivered changes:

- C++ translation/optimization/emission accounting now uses compile-time phase
  selection and direct updates to its own `CompileStats`. Native writes remain
  no-ops; the eight published fields and profile-v1 semantics are unchanged.
- Generated Wasm entries use Emscripten's shared slot allocator/mirror directly,
  avoiding the generic first-use deduplication scan of the main-module table.
  When a generic map already exists it stays synchronized; a later ordinary
  `addFunction` scan still includes generated entries. Released slots can be
  reused in both directions, and table/mirror/map references are cleared.
  Dependencies are explicit through `EM_JS_DEPS`; helpers were checked against
  Emscripten 6.0.13, not promised stable across arbitrary toolchain upgrades.
- A module-local C++ cache compares the last compile-only owner and all eight
  totals before entering JS. Unchanged production Runs avoid the call and u64
  instance-ID boxing. Owner/phase changes still publish; profiling publishes
  every time. The cache is invalidated before a potentially throwing call.
  Snapshot records remain read-only and historical leaves retain their values.

The [experiment record](measurements/2026-10-22-compile-accounting/README.md)
and its `manifest.json`/`evidence.tar.gz` retain all intermediate failures,
CPU/heap/GC diagnostics, exact observations, artifact/source hashes and commands.
Scratch builds and raw files remain under
`/tmp/dynarmic-release-evidence/compile-accounting`. The local `baseline*`,
`phase-only*` and `table-only*` directories contain immutable earlier artifacts.
The current `build-em` and `build-em-profile` hold the final boundary version.

The initial phase-only change failed paired profile warm latency at 64 modules
(~3× baseline); GC traces located a Scavenge inside the short revisit. Heap
sampling then exposed the avoidable full-table scan and function wrappers.
The table-only candidate passed profile/fixed/CoreMark gates but still failed
several production population warm budgets. Moving unchanged publication ahead
of the JS boundary resolved those gates. No budgets were relaxed and no samples
were excluded. The archive preserves the original stage names: `paired-*` is
phase-only, `final-paired-*`/`coremark-paired` is table-only, and `boundary-*`/
`coremark-boundary-paired` is the final implementation.

Final performance verification:

- Production fixed, production population and profile population: all original
  gates pass with 25 fresh processes per case, alternating builds/case order.
- Complete short CoreMark: all original gates and CRCs pass with ten fresh
  processes per case, alternating complete six-case cohorts. These 4,000-
  iteration runs are not duration-qualified or certified performance scores.
- Fixed cold compile medians are 0.873–0.993 of baseline; production cached
  population revisit medians are 0.390–0.993. Cold population ratios span
  0.796–1.088, so not every cold case improved. Profile population cold/warm
  ratios are 0.816–0.984 / 0.644–0.968. CPU frequency was not locked.
- Fixed/population emitted-byte totals and profile cache peaks are unchanged.
  A32 CoreMark compiled paths vary on both builds (112–115 modules); no code-
  size optimization is claimed from its small byte-median changes.

Correctness verification:

- Native CTest: 4/4, including the existing 159 Node protocol tests.
- `stats_test` passes both configurations, including field changes, repeated
  additions, null pointers, owner switches, heap growth, retained leaves and a
  property-access probe proving unchanged compile-only publication skips JS.
- `runtime_table_test` passes both configurations. Tagged independent Wasm
  entries detect live-slot clobbering; two 128-entry cycles check bounded table
  growth, shared slot reuse and ordinary host registration/deduplication.
- The tagged table test also passes with Emscripten `ASSERTIONS=2`; linked
  against the baseline library it fails the no-scan assertion as expected.
  A population host linked with `EXPORTED_RUNTIME_METHODS=[]` also passes,
  validating the explicit helper dependencies.
- Both `tests/regress.sh --strict --coremark /tmp/dynarmic-coremark-v1-final
  <build-em-or-build-em-profile> build-native` runs finish with
  `regression done fail=0`. Their additional CoreMark reports were retained at
  `/tmp/tmp.g20bF4JNt5/report.json` and `/tmp/tmp.tjF3m6FtZs/report.json` and are
  included in the archive. No executing regression script was modified.
- Chrome 133 and Firefox Worker checks pass in both configurations. Browser
  smoke output is correctness evidence, not controlled browser performance
  data. Hosted CI was configured for the new test but not run locally.

When work resumes: obtain named CPU profiles for the remaining frontend and
emission costs; combine populated-cache selective invalidation with retained
engine-memory measurements; extend Thumb application and browser/tiering
coverage; and collect complete-duration controlled application comparisons.
The heap samples here are allocation diagnostics, not RSS/engine memory bounds.
Shared-memory/pthread support, release packaging and performance leadership
remain unestablished. This milestone is not a release sign-off.

## Named compiler profiling milestone (2026-10-23)

Resumed under the user's active continuation instruction from clean `c1a33c9`.
That instruction supersedes the historical pause above. The preceding goal
work is verified progress: compilation boundary changes and the subsequent A64
benchmark instruction-count correction are committed. The long-term objective
is active and remains incomplete.

Added `tests/bench/cpu_profile.mjs` with full-frame identity, self/inclusive
sample counts, recursion deduplication, regex subtree selection, raw input hash
and sampling-gap diagnostics. It rejects malformed trees, missing samples and
unmatched/unsampled roots. These are unweighted diagnostic observations, not
phase timings or performance comparison reports. CTest includes its 19 tests;
the full Node protocol suite passes 178/178 and rebuilt native CTest passes 4/4.

Built a separate Release executable set at `/tmp/dynarmic-named-cpu` with
`--profiling-funcs`, profiling counters OFF. The stale `/tmp/boost-headers`
dependency was absent and was restored from system Boost headers before the
successful configure. No existing production/profile artifacts were modified.
Thirty sequential fresh-process profiles cover A32/Thumb/A64 fixed workloads,
A32 population 4,096 and A32/A64 complete short CoreMark. Fixed and population
outputs pass existing validators; all CoreMark CRCs match a fresh native run.
The raw profiles, commands, hashes, logs, summaries and replayable audit are
retained in [the experiment record](measurements/2026-10-23-named-cpu/README.md).
Scratch evidence is `/tmp/dynarmic-cpu-evidence-20261023`; the committed archive
is authoritative if temporary paths disappear.

The next optimization is now selected by evidence: A64 runtime decode-table
construction accounts for 54.9–59.5% of the compiler subtree samples in the
fixed workload and 41.3–43.0% in short CoreMark, across all five processes each.
Implement a compact constant A64 index with static handlers, preserving stable
specificity **and** upstream's three SIMD priority exceptions. Compare every
ordered bucket with the original table and validate typed handler arguments,
then run differential/sanitizer/browser gates and alternating ordinary Release
comparisons. Do not reuse fixed reports preceding `c1a33c9` without addressing
its changed instruction-count workload identity. No A64 optimization or speedup
is claimed in this profiling milestone.

The 4,096-entry A32 profiles also locate substantial engine Module/Instance and
emission costs. Investigate those after the A64 experiment; do not infer precise
source allocation costs from merged C++ names. Populated-cache selective
invalidation, engine-memory bounds, Thumb applications, browser tiering and
duration-qualified comparative workloads remain open. The strict full runtime
regression was not repeated for this tooling-only change.

## A64 constant decoder experiment — rejected; goal paused (2026-10-23)

The user explicitly requested **finish this round, then stop the goal**.
This round is closed with the candidate rejected by the unchanged performance
gates. After the evidence/documentation commit, set the goal to `paused` and
stop. Do not resume optimization or release work without a new user instruction.
The overall release/performance objective remains unfinished.

Starting from `bb7f1af`, the candidate introduced a shared constant index/static
matcher, an A64 adapter preserving upstream SIMD priority, ARM helper reuse,
and native A64 equivalence tests. Every ordered bucket, 643 rules, 28 priority
overlaps, one million random words and typed argument checks passed. Native
Release and ASan/UBSan CTest passed 5/5 (including 178 Node protocol tests).
Production/profile strict regressions with short CoreMark and Chrome 133 /
Firefox Worker checks passed. Final header-include rebuilds preserved all 74
runtime-tested JS/Wasm artifacts byte-for-byte; named diagnostic links matched
as well. These are candidate results, not tests added to the final source tree.

The 25-round fixed comparison passed, with A64 compile medians reduced 61–79%.
Initial production population and both separate short CoreMark cohorts failed
some gates. Population identical-artifact controls passed; combining all 35
short CoreMark rounds passed. Every original failure is retained. Ten candidate
CPU profiles validated without a runtime decode-table frame, and binary
inspection confirmed removal of its 67,446-byte initializer.

The final 120,000-iteration CoreMark collection completed all 60 executions
(five fresh processes per implementation and each of six cases). CRC and
minimum-duration checks passed, but the collector exited 1 for performance:

- A64 page-table kernel median ratio 1.0893, exceeding 1.05263.
- A64 flat-window kernel ratio 1.1768 and total Run ratio 1.1745, exceeding
  1.05263 and 1.15 respectively.
- A32 page-table compile ratio 1.1507, exceeding 1.15.

A64 long-workload compile medians fell 35–55%, but this does not override the
failed gates. Dispersion was substantial and clocks were not locked, so the
cause of the slower kernel intervals remains unresolved. No budget was widened
or sample discarded. No additional optimization or rerun was started.

The complete candidate code, tests, integration patch and CI change are
preserved as `source.patch` inside the 227-file archive in the
[experiment record](measurements/2026-10-23-a64-decoder/README.md). Production
source, integration, tests and general architecture documentation were restored
to the starting revision; this commit lands only evidence and documentation.
Archive audits passed before and after fresh extraction, including every
comparison, all 60 duration child outputs and ten CPU profiles. The archived
patch applies cleanly to the retained production source.

Scratch evidence is `/tmp/dynarmic-a64-static-evidence`; the committed archive
is authoritative if temporary files disappear. All benchmark processes have
terminated. `build-em`, `build-em-profile`, `build-native` and the sanitizer
scratch tree still contain candidate builds and need reconfiguration/rebuilding
before use against restored source. To inspect the candidate later, extract
its patch and apply it to an isolated checkout of `bb7f1af`, then build anew.

Only after an explicit resume: investigate the duration failure with controlled
host conditions, engine tiering and generated-code evidence before reconsidering
this candidate. Populated-cache invalidation/engine memory, Thumb applications,
browser performance and release packaging remain later open work.

## Active release/runtime-profiling continuation (2026-10-24)

The user's explicit release and sustained-performance objective resumes the
previously paused goal. `build-native` and `build-em` have now been rebuilt from
clean source at `f95d6c9`, replacing the stale candidate artifacts warned about
above. `tests/regress.sh --strict build-em build-native` completed with
`regression done fail=0`; external CoreMark was not requested in that run, so
this is not an application-gate result.

A fresh production fixed-suite baseline is at
`/tmp/dynarmic-head-f95d6c9-fixed-20261024.json` (five processes, Node v24.19.0,
affinity `0,2,4,6`). It passes the ordinary validator. Like all benchmark-v2
warm data, it includes engine tiering and is only a local before/after reference,
not a publishable performance score. A separate clean named host was configured
at `/tmp/dynarmic-named-runtime` with `--profiling-funcs`, profile counters OFF,
and no decoder candidate.

The Run-scoped profiler is implemented and verified. The ordinary sampler now
exports its collection path; the profiling CLI enables guest names before init,
captures exactly one Run and preserves guest/cleanup errors. Final protocol
verification passes 188/188 and native CTest passes 4/4. A real six-mode short
CoreMark smoke passed after the sampler refactor; full strict regression was
last run at clean `f95d6c9`, not after these tooling-only additions.

Complete 120,000-iteration CoreMark profiles cover A32/A64 in all three memory
modes. Thunk Runs spend substantial samples in memory callbacks; page-table and
flat Runs attribute about 99% to guest regions. Five hot A64 flat regions map to
`matrix_test`, `core_bench_state` and `iterate`. A synchronous compilation trace
matches every one to a TurboFan record, so persistent Liftoff is not the current
explanation. Generated WAT confirms per-access flat bounds checks, page-table
translation and thunk calls; do not remove required fallback semantics.

The `iterate` regions hit the 64-block cap and still make cross-module calls
with large state flushes. Profile counters report 328 block-budget and zero
IR-budget rejections. Raw profiles, WAT, tiering trace, fixed/full-duration
baselines and an offline verifier are committed in
[the runtime-profile record](measurements/2026-10-24-runtime-cpu/README.md).

The resulting isolated 128-block experiment was rejected during diagnostic
screening. Only `max_blocks` changed; 2,000 IR and 4,096 guest-distance limits
stayed fixed. Matched A64 flat emitted bytes rose from 711,948 to 875,375
(+22.95%), exceeding the +10% gate. Both `iterate` entries survived with equal
59,220-byte bodies, 24 static external-transfer sites and 131 `br_table` labels,
versus 21,973 bytes, 10 sites and 67 labels at baseline. These static counts do
not measure dynamic calls. Sequential sampled Runs passed native/guest CRC but
are not speedup evidence; an earlier candidate capture overlapped a native build
and is retained separately, excluded from timing observations. No full paired
performance cohort was started. Focused selector/planner CTest passes 2/2 and
static review found no new defect; full semantic/browser/sanitizer gates were
not run for the rejected candidate. The production default remains 64. See
[the region-size record](measurements/2026-10-24-region-size/README.md) and its
fresh-extraction verifier (`region-size evidence PASS profiles=3 files=45`).

Next concrete performance task: add compile-boundary per-region descriptor and
duplication attribution, plus diagnostic-only dynamic external direct-call /
RSB-return / other-tail-transfer counts if required. Do not scan larger caps or
put execution counters into production generated code. Reconsider selective
hot-call absorption only after those data identify a profitable, bounded edge;
otherwise select another application-profile target. Open release gates still
include callback/API semantics, bounded cache ownership, real engine memory,
equivalent browser performance collection, and an installable web distribution.

## Compile-boundary region diagnostics milestone (2026-10-24)

The compile-boundary infrastructure is implemented under the independent
`DYNARMIC_JS_REGION_DIAGNOSTICS` switch. Each Emscripten JIT owns an append-only
history of full `IR::LocationDescriptor` selections, entry identity and exact
module/entry-body bytes. Run/Step publishes frozen per-instance snapshots with
fixed-width hexadecimal 64-bit identities; unchanged histories are not
republished. Invalidation does not rewrite history. Production generated code
and ordinary profile snapshots are unchanged, and native configuration rejects
this Emscripten-only publication option instead of failing later in C++.

The pure-JavaScript duplication analyzer and complete-CoreMark collector check
record identity, totals, native CRC and normal guest halts. The durable
[region-diagnostics record](measurements/2026-10-24-region-diagnostics/README.md)
contains the raw report, all production/diagnostic modules, comparison report,
comparator, verifier and inventory hashes. Its A64 flat-window history has 111
regions, 4,177 selected block instances, 862 unique descriptors, 587 repeated
descriptors and 3,315 duplicate selections, totaling 708,917 module bytes and
694,277 exported entry-body bytes. These are static compilation observations,
not dynamic calls, live-cache residency or a performance score.

All 111 generated module pairs have equal sizes and identical normalized WAT
after masking only classified runtime addresses. There are zero raw-byte-
identical pairs, as expected from embedded addresses; raw equality is not
claimed. The 3,386 differences comprise 2,273 flat-window offsets, 278 link-slot
loads, 278 link-slot miss stores, 460 paired RSB entries and 97 fast-dispatch
bases. The comparator requires every other token to match, validates link-slot
load/miss equality within each module, and rejects both an ordinary `i64.const`
mutation and an isolated link-slot miss-address mutation. The latter regression
was confirmed to fail before the pairing fix and pass afterward.

Review also fixed link-slot RAII ownership, per-recorder publication ownership,
fail-closed diagnostic/profile configurations and native option handling. The
follow-up correctness review approved the fixes with no remaining CRITICAL,
HIGH, MEDIUM or LOW findings; the security review found no actionable CRITICAL,
HIGH or MEDIUM issue. CI now includes diagnostic and diagnostic-plus-profile
hosts, real diagnostic Firefox Workers, and a pinned-wabt
retained-evidence/mutation job. Hosted CI has not run in this local session; do
not describe workflow configuration as a hosted-CI result.

The apparent diagnostic Firefox timeout was an incomplete local artifact set,
not a JIT execution failure: only `arm_jit` had been built, so the page and demo
files returned 404. Building `browser_page` fixed the input. Production,
diagnostic, and diagnostic-plus-profile Firefox Worker smoke now pass, as do
the diagnostic Node normal/fault hosts (A32/A64, all three memory modes).
`verify_browser.mjs` now preflights all required artifacts and fails immediately
with a build-target hint instead of waiting for the 120-second timeout.

Validation completed for this milestone:

- `node --test tests/bench/*.test.mjs`: 210 tests passed.
- Native Release CTest: 5/5 passed; fresh native ASan/UBSan CTest: 5/5 passed.
  Sanitizer coverage is native only, not Emscripten or browser coverage.
- Production, profile, diagnostic and diagnostic-plus-profile Emscripten
  `region_diagnostics_test` hosts all passed.
- `tests/regress.sh --strict --coremark /tmp/dynarmic-coremark-long build-em
  build-native`: `regression done fail=0`; all six complete-CoreMark cases had
  five samples and `durationQualified=true`. The final report is
  `/tmp/tmp.JWC6LvMvWI/report.json`; all 30 ordinary production samples kept
  `profile=null` and had no `regionDiagnostics` property.
- `node internals/measurements/2026-10-24-region-diagnostics/verify.mjs --wabt-bin
  /home/xiong/.npm/_npx/cf2e988b5c429dbd/node_modules/wabt/bin`: all retained
  evidence, fresh comparisons and both mutation negative tests passed with
  wabt 1.0.39.

Next concrete performance task: investigate the observed call/return duplicate-
selection cascade. `RegionSelector::Successors` includes `PushRSB` return
locations, and several new entries are continuations already selected in earlier
regions. Formulate one bounded call/return-region policy and measure external
transfers, emitted bytes, compile latency and qualified complete-CoreMark
throughput. Static duplication alone does not establish dynamic hotness; add
diagnostic-only transfer counters only if a concrete edge decision remains
unresolved. Do not resume global block-cap sweeps. Callback/API semantics,
bounded cache ownership, real engine memory, browser performance collection and
installable web packaging remain independent release gates.

## PushRSB static ablation and multi-entry design screen (2026-10-24)

The main production source remains at clean `9731e9a`; the experiment was built
only in `/tmp/dynarmic-return-probe.x0YvH5/source`. The durable
[PushRSB ablation record](measurements/2026-10-24-push-rsb-ablation/README.md)
contains the complete baseline/candidate reports, exact comparison, frozen
selector/test patch, commands, comparator and tamper-testing verifier. The
hardcoded 28-descriptor list is diagnostic machinery and was not applied to the
main tree.

Control-flow review rejected the idea that all repeated continuations are dead.
A caller/callee/continuation can share one region, so removing a `PushRSB`
successor can add a real external return. Conversely, a continuation selected
only in the caller is not an alternate entry for an external callee: the module
exports only its original entry and table transfers carry no interior index.
Structured emission includes only original-entry reachability; fallback dispatch
serializes every selected block.

The candidate suppresses the 28 values only at `PushRSB` append, preserving
ordinary edges, condition-failed edges, direct entries and full 64-bit identity.
The regenerated baseline matched all earlier archived diagnostic objects. The
selector regression failed before implementation for the intended reason, then
passed; native CTest passed 5/5 and the Emscripten diagnostic host passed. One
complete 200,000-iteration A64 flat-window CoreMark history per implementation
passed normal-halt and native/guest CRC checks.

The independently reproduced static comparison is PASS:

- Regions/entries: 111 → 111; region IDs and entry sequence are identical.
- Selections: 4,177 → 3,208; duplicates: 3,315 → 2,353.
- Module bytes: 708,917 → 690,520 (-18,397, -2.60%).
- Entry-body bytes: 694,277 → 676,174 (-18,103, -2.61%).
- Target selections: 451 → 25; target duplicates: 423 → 2. Five targets vanish;
  the other 23 remain through ordinary control flow or direct entries.
- Thirty-six entry histories changed (30 smaller, two larger, four byte-neutral);
  the two increases are retained explicitly. All six selected hot histories are
  exactly unchanged.

This is not a performance result or production acceptance. No timing cohort,
dynamic generated-code counter, browser performance run or release claim was
made. Candidate full differential/sanitizer/browser qualification was not run
after this diagnostic-only screen. Production behavior is unchanged.

Verification for the evidence milestone: isolated benchmark protocol suite
210/210; comparator 31/31; archived verifier 16/16 plus a successful real audit
(47/47 across the two evidence suites). The comparator initially had three
evidence bugs (candidate target absence, region-ID-only history changes, and
identical-JS handling); all were corrected. Independent review then caught a
missing workload-iteration pin. A valid 120,000-iteration pair demonstrated the
bypass before the fix; both reports now require exactly 200,000 iterations, all
31 tests pass and the real comparison is byte-identical. Selector and final
comparator reviews have no remaining findings. Verifier review reproduced
coordinated target-list tampering, extra candidate-status entries and ignored
patch destination paths; exact pins and mutation tests now close all three, with
negative coverage for the core static invariants. A second review reproduced
non-target patch-body and same-count changed-history tampering, so the verifier
now also pins all seven evidence files' expected SHA-256 values independently of
the mutable inventory; the final verifier re-review has no remaining findings.
Independent security review found no actionable CRITICAL/HIGH/MEDIUM issue. The verifier refreshes hashes after deliberate
mutations to prove that inventory checks are not hiding stale comparisons or
deeper evidence tampering. The main tree's last complete
strict regression remains the one recorded at `9731e9a`; this milestone changes
evidence/docs/CI only.

Read-only multi-entry architecture review is complete. **Do not alias an
interior descriptor to the current `"f"` export.** The smallest potentially
sound follow-up is A64-only, fallback-dispatch-only and return-site-only, with
masked-`state.pc` initial selection and a fail-closed unknown-PC path. It keeps
the two-argument ABI and one table slot, but requires a single shared region
allocation, full-descriptor aliases, no overwrite of live owners, and all-alias
invalidation with exactly-once resource release. External-callee RSB return must
pop exactly once; entry into the continuation must not pop again. Structured
regions and single-step behavior stay unchanged.

Next concrete task: implement a diagnostic-only `region-diagnostics-v2` history
and offline coverage classifier before implementing that prototype. Existing
artifacts reconstruct exactly 82 later compilation records, but cannot establish
per-block emission, actual return-site membership or live region generations.
The v2 record will retain layout, emitted/original-entry-reachable/return-site
masks and sorted live region IDs captured before each miss; v1 archives remain
unchanged. Validate the v2-to-v1 projection, exact 111/82 cohort counts and all
emission/liveness invariants. The pre-registered stop rule is <=8 covered later
entries; >=9 of 82 with at least one statically eligible live fallback return-
site owner permits prototype TDD, not a performance claim. If overlap is not
negligible, plan/TDD the bounded experiment with direct-link, RSB, fast-dispatch,
unknown-PC, clear/destruction and range-invalidation tests before considering a
performance run. The 552,378-byte whole-region attribution is not a promised
saving. Do not broaden to structured multi-entry, add generated-code counters or
resume global cap sweeps. Independent callback/API, bounded-cache/engine-memory,
browser-performance and installable web packaging release gates remain open.

## Diagnostic-v2 live region-alias coverage milestone (2026-10-24)

The diagnostic recorder now publishes `dynarmic.js/region-diagnostics-v2`.
Each region records its actual `single`/`structured`/`fallback` layout and each
selected block records its full descriptor, actual emitter-callback emission,
original-entry CFG reachability and actual return-site membership. Immediately
before a confirmed miss compiles, the diagnostic build captures sorted unique
region-generation IDs from actual live `BlockEntry` owners. Successful module
instantiation associates the recorder's new generation ID with that cache entry.
The implementation adds no generated-code counters and production/profile
snapshots remain without the diagnostic leaf.

`projectRegionDiagnostics` reconstructs the complete v1 schema, including
selection order, and all prior duplication analysis operates on that projection.
The offline `classifyRegionAliasCoverage` requires A64 on both entry and owner,
fallback layout, an interior emitted return site, and a live owner generation.
Exactly eight covered entries STOP and nine GO in synthetic boundary tests; a
mixed-frontend coverage bug found in review was fixed with a regression test.

The durable
[region-alias coverage record](measurements/2026-10-24-region-alias-coverage/README.md)
contains the complete 200,000-iteration A64 flat-window CoreMark v2 report, all
82 classifier rows, both sets of generated modules, structural comparison,
commands, inventory and fail-closed verifiers. The exact v2-to-v1 projection
matches the retained v1 history; duplication remains 3,315 selections and
708,917 module bytes. Layouts are 79 structured, 11 single and 21 fallback. Of
82 later entries, exactly 16 have a live eligible owner, passing the registered
>=9 gate for bounded prototype planning/TDD only. Covered generations are 22,
23, 26, 27, 28, 29, 33, 38, 42, 43, 44, 52, 53, 72, 80 and 81.

Fresh production and diagnostic dump Runs both halted normally with native/guest
CRC `0x4983`, 111 regions and 708,917 bytes. WABT 1.0.39 was built from verified
official source because the official prebuilt binary required unavailable
GLIBC 2.38. All 111 module pairs have equal sizes and identical normalized WAT
after masking only the classified 3,386 embedded runtime-address differences:
2,273 flat offsets, 278 link-slot indices, 278 link-slot miss addresses, 460
paired RSB entries and 97 fast-dispatch bases. Zero pairs are raw-byte identical;
no raw equality or general semantic-equivalence claim is made. The retained
verifier binds module sizes/hashes to region history, rejects non-regular or
unexpected archive entries and independently pins the four frozen evidence
payloads in addition to the mutable inventory.

Validation for this milestone:

- JavaScript benchmark/evidence suite: 238/238 passed, including exact 8/9
  thresholds, mixed-frontend rejection, refreshed-inventory attacks, coordinated
  owner/analysis mutation, compressed-input preflight, and generated
  metadata/archive tampering.
- Native Release and fresh isolated ASan/UBSan CTest: 5/5 passed in each build.
- Emscripten diagnostic and diagnostic-plus-profile region hosts: both passed.
- Current region-alias, previous region-diagnostics and PushRSB evidence
  verifiers all passed; `git diff --check` passed.
- Fresh code review approved the milestone with no CRITICAL/HIGH/MEDIUM/LOW
  findings and independently reproduced the retained 111-module comparison.
- Security review found one LOW compressed-input ordering issue; its RED
  regression and frozen-hash preflight fix passed focused code/security
  re-reviews with no remaining actionable findings.

No throughput comparison or multi-entry implementation was started. The last
complete strict regression remains the earlier one recorded at `9731e9a`; this
milestone's diagnostic Run and dump Runs are not qualified performance evidence.

Next concrete task: plan and TDD the bounded A64 fallback return-site prototype,
without broadening its scope. Keep the current two-argument ABI and one table
slot, but add explicit masked-`state.pc` initial entry selection with fail-closed
unknown-PC behavior. Use one shared allocation with full-descriptor aliases, no
overwrite of a live owner, all-alias invalidation and exactly-once table/link
release. External-callee RSB return must pop exactly once; continuation entry
must not pop again. Structured regions and single-step behavior stay unchanged.
Direct-link, RSB, fast-dispatch, unknown-PC, clear/destruction and range-
invalidation tests must pass before any performance evaluation. The 16/82 result
does not establish dynamic hotness or promised byte savings. Do not add dynamic
counters, resume global cap sweeps, or apply the hardcoded PushRSB ablation to
production. Independent callback/API, bounded-cache/engine-memory,
browser-performance and installable web packaging release gates remain open.

## A64 fallback return-site alias prototype (2026-10-24)

The bounded prototype is complete. A64 enables the previously reviewed fallback
selector in production; the emitter option remains off by default for other
configurations. `AddressSpace::Emit` returns one shared allocation and its actual
fallback aliases, and `AddressSpace::Publish` registers the original descriptor
plus aliases with `emplace`. Existing owners are never overwritten. A conflicting
original destroys the redundant allocation; conflicting aliases stay with their
prior owners. Publication reserves rollback storage before insertion, rolls back
only newly inserted descriptors on failure, and updates allocation-based module
byte/cache counts and profile peaks only after successful publication.

The new `a64_region_alias_test` uses an actually emitted irreducible fallback
region with a distant external `bl`/`ret` callee and BRK observation points. It
checks owner/alias identity and one compilation, direct-link and lazy/warm RSB
behavior, exactly one external return pop, cold/warm fast dispatch, pre-existing
owner/alias conflicts, owner- and alias-range invalidation, guest mutation,
single-step descriptor separation, clear/destruction, live table entries and
free-list uniqueness. A strengthened warm-transfer test verifies patched RSB
zeroing, recompilation without recompiling the surviving callee, fast-dispatch
clearing and absence of stale-generation execution. The host is wired into
`tests/regress.sh` as a fixed semantic test.

Two test-fixture defects were diagnosed rather than hidden:

- The strengthened test initially failed only after an earlier harness was
  destroyed. Its `ExecutionContext` was not zero-initialized, so reused stack
  storage could contain an old RSB descriptor/index pair. A matching descriptor
  allowed an external `ret` to call the released numeric table slot. Initializing
  the fixture's state and execution context fixed the cold-call failure without
  changing terminal/RSB production logic. The cross-test ordering and assertions
  remain in place.
- Diagnostic builds initially reported seven selected blocks where the test
  expected nine. `SUBS` and its following conditional branch are one frontend IR
  basic block; the expectation incorrectly listed every guest instruction. The
  diagnostic assertion now uses the actual seven block starts while retaining
  all layout, emission, entry-reachability and return-site checks.

Validation completed for this milestone:

- Production Emscripten: alias, region-diagnostics, lifecycle, runtime-table,
  stats and instance hosts passed.
- Region-diagnostics and region-diagnostics-plus-profile Emscripten builds:
  alias, region-diagnostics and lifecycle hosts passed in both configurations.
- Native Release CTest: 6/6 passed. Fresh isolated ASan/UBSan CTest: 6/6 passed
  with leak detection and halt-on-error enabled.
- JavaScript protocol/evidence tests: 228/228 passed. Native smoke, A64 fallback,
  saturation (14,641 cases) and Wasm binary Node tests passed.
- A32 guests and reference-log matrix passed. A64 guests, the complete seeded
  A64 Unicorn differential matrix and the checked-in semantic corpus passed.
  The A32/Thumb Unicorn differential matrix, SMC test and demo host also passed.
  Browser host normal/fault halts passed all 18 runs and the Firefox Worker
  smoke passed; no browser benchmark was collected.
- Fresh Phase-3 code review approved the final source/test/CMake/regression diff
  with no CRITICAL/HIGH/MEDIUM/LOW findings. Allocation rollback was reviewed by
  inspection; no fault-injection test was added. `git diff --check` passed.

The complete strict regression was deliberately not run because it includes
performance workloads and this round authorizes correctness validation only.
No CoreMark run, throughput comparison, dynamic generated-code counter or new
optimization experiment was started. This milestone establishes prototype
semantics, not a performance improvement or release sign-off.

Next concrete task: stop here as requested. Do not begin multi-entry performance
collection without separate authorization. Later work must not broaden the
prototype to structured regions, resurrect global cap sweeps or treat the 16/82
static coverage result as a throughput/byte-saving promise. Independent release
gates listed above remain open.

## A64 alias static-gate failure and exact diagnosis (2026-10-24)

The later authorized performance qualification began with two independent
Release/profile-OFF/region-diagnostic-ON builds at baseline `f96fe7a` and
candidate `6a6c6d7`. Both ran the frozen 200,000-iteration A64 flat-window
CoreMark workload with normal halt and matching native/guest CRC `0x4983`.
The preregistered exact mechanism gate required 95 candidate regions (111 minus
16 covered later entries). The candidate compiled 99, so the gate **failed before
any short or complete-duration timing collection**. Generated module bytes fell
708,917 to 576,602 (-132,315, 18.66%), but this static observation does not
override the failed gate or establish a performance improvement.

The exact difference is not a four-entry no-overwrite conflict. Actual candidate
publication replay proves 15 predicted entries were absorbed by real aliases.
The remaining predicted entry `0x10380` survives because its only predicted
owner, baseline region 22 at `0x10370`, was itself alias-absorbed; no candidate
region exposes `0x10380` as an alias. Five additional baseline entries (regions
30, 34, 39, 40 and 41) disappeared as ordinary emitted interior blocks, while
eight new original entries compiled. The closed reconciliation is
`111 - 15 - 5 + 8 = 99`, and the difference from the registered expectation is
`1 - 5 + 8 = 4`. The existing coverage classifier remains correct as a Boolean
live-owner screen; the error was treating it as a candidate-history predictor.

The durable
[alias static-gate record](measurements/2026-10-24-region-alias-static-gate/README.md)
retains both compressed raw reports, the applicable candidate patch, commands,
fresh categorized analysis and fail-closed verifiers. `alias_replay.mjs`
replays actual no-overwrite publication and requires every earlier owner to be
live at every miss; invalidation-containing histories are outside this exact
experiment and rejected rather than reconstructed. It distinguishes 26 actual
published aliases from nine conflicting raw return-site exposures, including
`0x100c8` and `0x10af8`, and rejects any original entry compiled while its real
alias is live. The analyzer validates source/artifact/workload identity,
stdout metrics and pinned CRCs, duplication and compile totals. Independent
payload hashes prevent coordinated report/analysis/inventory rewrites from
turning the retained STOP into a pass.

Independent review found two evidence-tool gaps: stdout compiler flags were not
bound to the frozen workload, and replay omitted the candidate's single-step
alias exclusion. Both are fixed, with coordinated native/guest flag-tamper,
native/guest output-swap and synthetic single-step regressions. The frozen raw
evidence and all categorized results are unchanged. Focused validation passes
19/19 tests, also covering synthetic taxonomy, actual no-overwrite behavior,
owner-generation loss, real raw exposures, exact real descriptor sets, stale
totals, coordinated CRC changes, stale verdicts, frozen hashes, inventory and
patch-path tampering. The candidate patch also applies cleanly to the isolated
baseline clone. The three test files are registered in `benchmark_protocol`:
the complete JavaScript protocol suite passes 247/247, and native CTest passes
6/6 after reconfiguration. These are evidence/protocol checks only; no new JIT
semantic or performance campaign was run in this round.

Targeted independent re-review approves both fixes with no remaining correctness
findings. Next concrete task: separately register any renewed alias mechanism
model before collecting performance timing. Do not rewrite 95 to 99, discard the
eight-entry cohort, or start timing while the registered gate remains failed. A
future alias experiment needs a model that accounts for owner dependencies and
changed candidate entries. Independent callback/API, bounded-cache/engine-memory,
browser-performance and installable web packaging release gates remain open.

## Active A64 alias performance qualification (2026-10-27)

The sustained-performance goal resumes from clean `8ceeee5`. The previous
95-region gate remains failed and unchanged. A separate
[forward-publication mechanism and timing protocol](measurements/2026-10-27-region-alias-performance-plan.md)
is now registered before any alias timing collection. It replays actual candidate
misses with no-overwrite publication, rejects live-original conflicts, retains
owner dependencies/new entries as explicit outcomes, and preserves all ordinary
correctness, byte and duration budgets. The retained static evidence passes this
new mechanism gate; this does not establish dynamic hotness or a speedup.

The pinned short and 200,000-iteration CoreMark workloads were rebuilt after the
temporary directories were lost. Their workload identity matches the retained
experiment, and the A64/native executable hashes match exactly. Independent
baseline (`f96fe7a`) and candidate production builds are under
`/tmp/dynarmic-a64-alias-perf` and `build-em`. Native CTest passed 6/6 and the
JavaScript protocol suite passed 228/228 before timing.

The five-round short screen was retained as a non-duration-qualified failure:
A64 flat-window kernel ratio 1.05910 and A32 page-table compile ratio 1.24432.
The subsequent five-round complete-duration comparison collected all 60 guest
samples with matching native/guest CRCs and no exclusions. It also failed the
unchanged gates: A64 flat-window kernel median regressed from 22.918225 to
25.086437 seconds (ratio 1.09461, budget 1.0526316), while generated bytes fell
to ratio 0.81336. A64 thunk/page-table kernel ratios were 1.03320/1.03748 and
all Run/compile gates passed. A32 bytes were unchanged; its timing differences
are not attributed to this A64-only change. Full raw evidence, hashes and exact
analysis are in
[the alias performance report](measurements/2026-10-27-region-alias-performance/README.md).
Default A64 alias publication is therefore disabled; the opt-in emitter and
semantic tests remain for a future profiled redesign. Entry-selector overhead
is a hypothesis, not a proven cause. V8 13.6 Liftoff lowers `br_table` to a
binary compare tree, so `br_table` is not assumed to be a jump-table fix.

The first strict regression after disablement exposed the integrated test's
implicit default-on assumption. The fixture now opts in explicitly through the
internal address-space constructor, and a separate test constructs the
production default with every optional argument omitted and requires independent
continuation compilation. Native/Emscripten rebuilds passed, native CTest passed
6/6, the integrated default/opt-in host passed, and `tests/regress.sh --strict
build-em build-native` completed `regression done fail=0`. Final independent
review found no actionable issues.

## Structured register-cache pruning (2026-10-27)

The cache-domain experiment is implemented after a confirmed RED/GREEN cycle.
An entry-unreachable A64 return-site block carrying an `X30` cache load remains
in the irreducible fallback positive control, including with aliases explicitly
enabled. In the structured case, cache-heavy and inert versions of that omitted
block now produce byte-identical modules. The mask is installed only after
`PlanRegion` succeeds; fallback therefore retains every selected block and
diagnostic reachability remains unchanged.

Native and Emscripten rebuilds passed, Node smoke execution passed, native CTest
passed 6/6, the default/opt-in A64 alias host passed, and `tests/regress.sh
--strict build-em build-native` completed `regression done fail=0`. Independent
review found no actionable correctness, lifecycle or test-quality issues. No
runtime speedup is claimed yet. Long-term browser, bounded-memory, callback/API
and packaging release gates remain open. The independent
[cache-pruning qualification protocol](measurements/2026-10-27-cache-pruning-performance-plan.md)
was registered before timing.

The original A32 equal-history mechanism gate failed (117 baseline versus 118
candidate regions), so timing was not started. A separately registered repeat
experiment demonstrated within-implementation A32 variability (baseline 117/118,
candidate 118/116), identical cross-implementation descriptor unions, and only
fallback layout for varying entries. The strengthened four-pair A32 plus strict
A64 mechanism audit passed; 52 omitted-block structured A64 regions shrank, with
no common-region growth. Raw mechanism reports and the repeat/union audit are
archived. The original equal-history comparison output was not separately
retained; its failure remains explicit in the original plan and the audit's
`originalGate` field, and is not reconstructed or relabeled.

The subsequent complete-duration comparison collected all 60 valid samples with
matching CRCs and no exclusions, but **failed** the unchanged A32 thunk compile
budget: 84.700447 ms baseline versus 143.708962 ms candidate (ratio 1.696673,
limit 1.15). All kernel, Run and emitted-byte budgets passed; this is not an
accepted speedup. The other five cases passed all budgets. Raw multi-phase
outliers support a noise investigation but do not erase the failure. The
[complete evidence and exact commands](measurements/2026-10-27-cache-pruning-performance/README.md)
are retained, including artifact hashes and collector logs.

The separate [noise follow-up protocol](measurements/2026-10-27-cache-pruning-noise-followup-plan.md)
was registered before replacement timing. Its pooling tool was RED/GREEN tested,
validates identity and recomputes summaries without mutating raw reports.
Mechanism audit fixtures validate against region-diagnostics-v2 after independent
review. The complete JavaScript protocol suite passed 235/235 before follow-up
timing, and native CTest passed 6/6.

Exactly one registered follow-up cohort was collected. All 60 samples completed
with valid durations and CRCs. A32 thunk compile passed at ratio 1.022316, but
the independent cohort **failed** A64 flat-window compile at ratio 1.174318
(126.852117 versus 148.964732 ms, limit 1.15). The derived ten-sample pooled
comparison passed every budget (that A64 compile ratio was 1.130867), but the
preregistered rule required both independent and pooled comparisons to pass.
Final result is therefore **REJECT**: original FAIL, follow-up FAIL, pooled PASS.
No further cohort was collected and no budget was changed. The complete raw and
derived evidence is retained in the
[cache-pruning report](measurements/2026-10-27-cache-pruning-performance/README.md).
Independent final review also caught and corrected a mechanism-archive label:
the two retained comparison payloads are repeat/union audits, not the original
one-shot equal-history output, which was not separately retained.

The cache-pruning implementation is disabled by restoring the baseline backend
and semantic tests from `b0aa3d1`; no optimization code or mask remains in the
default emitter. Native and Emscripten rebuilds passed, native CTest passed 6/6,
the complete JavaScript protocol suite passed 235/235, focused A64 alias and
region-diagnostics hosts passed, and `tests/regress.sh --strict build-em
build-native` completed `regression done fail=0` (external CoreMark was not
requested). Evidence hashes, compressed payloads and new-archive byte identity
also passed. Independent review approved the exact restoration, retained
samples, verdicts and evidence scope with no findings. Byte reductions and
isolated kernel improvements are not accepted as a speedup. Next: collect fresh
Run-scoped profiles of this restored default before selecting a new runtime
optimization. Long-term browser, bounded-memory, callback/API and packaging
release gates remain open.

## Restored-default Run-scoped profiles (2026-10-27)

After committing the cache-pruning revert as `339939c`, a separate named Release
host collected three fresh Run-scoped profiles for each of the six CoreMark
memory/frontend modes (18 processes, 200,000 iterations). The checkout was clean
throughout collection. Every Run passed normal-halt and native/guest CRC checks;
raw hashes, all 36 recomputed summaries, aggregate reports and 97 archived
payloads pass the offline verifier, including from a fresh extraction. The
complete evidence is in
[the restored-runtime profile record](measurements/2026-10-27-restored-runtime-cpu/README.md).
Instrumented Runs and unweighted sample shares are diagnostics, not benchmark
scores or speedup evidence.

The new cohort reproduces the earlier hot regions. A64 `iterate` regions
`a64_10b00_region` and `a64_10b18_region` have median guest self-sample shares
14.11%/12.37% in flat-window and 16.33%/17.78% in page-table mode. The other
three dominant A64 regions remain `matrix_test` and two `core_bench_state`
entries. Thunk memory callbacks/helpers account for 49.36–49.61% (A32) and
56.52–56.68% (A64) of guest self samples. A32 page-table compilation histories
remain variable; all per-process byte counts are retained.

Function-level profiles cannot isolate external calls and state transfers from
arithmetic or memory work inside a hot region. Next: add diagnostic-only exact
external direct-call / RSB-return / other-tail-transfer counts for these regions
before choosing a runtime optimization. Do not infer hotness from static edge
counts, revive rejected cache pruning, or resume global block-cap sweeps. Any
candidate must retain the 64-block cap and pass full correctness, compile-time,
emitted-byte and complete-duration throughput gates. Browser, bounded-memory,
callback/API and packaging release gates remain open.

## Exact region-transfer diagnostics (2026-10-27)

The [diagnostic plan](measurements/2026-10-27-region-transfer-diagnostics-plan.md)
is implemented without changing production region selection, terminal lowering,
generated bytes or snapshot shape. Diagnostic builds now publish a separate
frozen `regionTransferDiagnostics` leaf after every A32/A64 Run or Step. Stable,
append-only per-JIT records hold exact wrapping shared-memory `i64` counters for
module entries and taken external direct links, RSB returns and fast-dispatch
tail transfers. Misses, halt-pending paths, dispatcher returns, internal edges,
single-step exits and helper calls are excluded. Values are fixed-width
hexadecimal strings; a never-taken exit has zero last-target metadata, and the
last target is not represented as a histogram.

Real generated-Wasm execution tests cover direct/RSB/fast-dispatch hits, misses
and halt paths, RSB-mismatch classification, internal exclusions, Step, aliases,
invalidation, per-instance ownership, high A64 PCs and immutable snapshots. The
Run-only conservation identity passed; it is deliberately not applied to Step.
Fixed IR fixtures require production repeatability and byte equivalence for a
null diagnostic configuration. Production snapshots reject the new leaf.

CoreMark reports retain raw transfer history and exact `BigInt` analysis, and
bind every transfer generation to its compile-generation entry and selected
exit blocks and to the workload architecture. Strict validation rejects
oversized dispatch counts rather than silently wrapping them. Counter
aggregation alone retains defined modulo-2^64 semantics.

Independent core review found no correctness, lifecycle or publication issues.
A separate test/report review found one HIGH evidence-binding gap and two MEDIUM
validation gaps. New regressions intentionally failed 4/55 before the fixes and
then passed 55/55; independent re-review approves all three fixes with no
remaining findings. Native CTest passed 6/6, including the complete registered
JavaScript protocol suite. Diagnostic, diagnostic-plus-profile and production
publication, execution, lifecycle and instance hosts all passed. The strict
production regression completed `regression done fail=0` (external CoreMark was
not requested). `git diff --check` passed and the dynarmic submodule remains
pristine.

These counters are mechanism evidence only; no diagnostic duration is a benchmark
score or speedup. Next: commit this infrastructure, then collect fresh A64
flat-window and page-table CoreMark reports from the clean committed source.
Use the exact hot-region counts to select or reject a boundary-optimization
hypothesis. Do not run another cache-pruning cohort or infer a speedup from
transfer counts alone. Browser, bounded-memory, callback/API and packaging
release gates remain open.

## A64 exact transfer evidence (2026-10-27)

Infrastructure commit `ec4701f` produced two clean-source 200,000-iteration
CoreMark reports, one A64 flat-window and one page-table. Both passed normal
halt, workload identity and native/guest CRC checks. The
[raw reports, exact analysis and verifier](measurements/2026-10-27-region-transfer-diagnostics/README.md)
are retained with pinned hashes and tamper tests. The focused evidence tests
passed 4/4, and native CTest passed 6/6 with the verifier registered in the
complete JavaScript protocol suite. Independent review approved the arithmetic,
evidence binding and interpretation; a fresh clone also reproduced the verifier
and all four tests. Both memory modes have the same 298,200,064 taken external
transfers: 145,800,018 direct chained edges,
152,400,030 RSB returns and only 16 other tail transfers.

The two profile-hot `iterate` entries have about 200,000 direct transfers each
and zero RSB/other transfers, so they do not justify an RSB-focused candidate.
The stronger eligible mechanism is direct edge `0x12370 -> 0x1237c` in region
`0x1202c`, taken 47,199,999 times in each mode. Its caller region has
88,899,996 direct transfers; target region `0x1237c` has 48,000,000 entries and
47,999,998 RSB returns. Last-target metadata is not a per-target histogram, and
direct chained edges are not all architectural calls.

## A64 hot-edge static eligibility (2026-10-27)

The design review rejected immediately merging the two full 64-block regions and
allowed only a bounded static phase. That phase is now PASS for a small opt-in
focused-reselection prototype. The
[static-gate record](measurements/2026-10-27-hot-edge-static-gate/README.md)
preserves the exact closure, accounting and emission sanity checks.

Decode proves `0x12370` is a direct-branch block, not `BL`; there is no call
continuation or `PushRSB`. Do not call this call/return inlining or attribute
the target region's RSB count to the hot edge. A required-first selection fits
17 blocks/127 IR, and deterministic fill produces 64 blocks/524 IR while keeping
all seven return sites. It adds `0x1237c` and `0x12418`, displacing `0x12350`
and `0x12398`. Both ordinary and candidate layouts remain fallback.

Two measured external edges totaling 49,599,998 transfers would become internal,
but three formerly internal edges would become external and their old dynamic
counts are unknown. Therefore this is not a net-transfer or speedup claim. The
retained selector probe reproduces the pinned analysis exactly, and the portable
emitter reproduces both archived Wasm hashes. Its 20 tests plus nine independent
evidence-verifier tests pass; three additional native emitter integration tests
cover stale/unwritable outputs and byte-identical reproduction. The verifier
rejects semantic tampering even when
an attacker regenerates the inventory. Native CTest passes 6/6, and the strict
backend regression finishes with `fail=0` (CoreMark not requested).

Next: capture a fixed pre-change default generated-Wasm corpus, then implement
only the default-off focused selector through TDD. The selected configuration is
a private CMake string of comma-separated full hexadecimal descriptors, parsed
strictly into immutable per-instance A64 state; there is no public JIT API,
runtime environment setting or mutable process-global focus. Stale or impossible
focused closures must fail explicitly, never silently revert to a partial or
ordinary region. Require pre/post
disabled-focus byte identity, semantic and generated-Wasm tests, exact whole-Run
mechanism comparison and independent review before paired complete-duration
qualification. Stop on displaced hot transfers or failed correctness/performance
gates. Do not repeat rejected cache pruning. Browser, bounded-memory,
callback/API and packaging release gates remain open.

## Default region/generated-Wasm corpus (2026-10-27)

Before changing the selector, `tests/tools/region_corpus.cpp` now records five
synthetic A32/A64 regions through the real `AddressSpace::GenerateRegion` and
`MakeEmitConfig`, then emits each twice with fixed link/fast-dispatch addresses.
It checks deterministic descriptors and bytes without publishing or executing
code. The frozen pre-change source is
`33f2ca51149ca12c48dd4c60b22fe900719f2a2c`; candidate recordings carry their
own current Git revision rather than being forced to impersonate that baseline.

The checked-in raw modules cover A32 conditional/backward flow, an A64 single
block, A64 backward conditional flow with sorted `entry_index=1`, A64
`bl`/`ret` with a `PushRSB` continuation, and a 65-block chain capped at 64.
Their sizes are 394, 113, 202, 644 and 523 bytes respectively. All five pass
`WebAssembly.validate`; fresh native recording reproduces every full descriptor,
entry index and raw byte exactly. Exact A32 instruction-word and mapped
descriptor assertions pin the PC+8 branch displacement and condition field.
SHA-256 values, in the same case order, are
`d667f22eea78b2151e4176463245da0207c876161080fd73d25987cf07e266f8`,
`22eb58fb01594371b9bc87051a819e30a41dcecb09c65635ca01e202b501422e`,
`662711f6bd723a15018d9f402d2a0a9165ea9cdc2db72234970164acd4955725`,
`1ccf80650fdb2aca1d84cc07f12cd46a91d1b0743c1ecded31a011b93a45121a`, and
`8051d9952ccb42e0e3bc59bc05c61e2723405285625e0affb3f8cd11c1f1366f`.

`region_corpus_compare.mjs` compares exact case/order/descriptors/entry indices
and uses `Buffer.equals`, not hashes or module sizes, for byte identity. Its 26
tests cover required mutations, malformed metadata and unsafe paths, invalid
headers, stale-output refusal, post-change provenance and fresh native output.
The deterministic `region_corpus` CTest rebuilds and records into the build tree;
the source-tree baseline is never overwritten. Full native CTest passes 7/7.
This is a compatibility/reproducibility gate only, not runtime or performance
evidence. Next: write focused selector/parser/address-space tests before
implementing the default-off A64 prototype. Disabled-focus output must continue
to pass this fixed corpus.

## Default-off focused selector (2026-10-27)

`RegionSelector::Select` now accepts an optional `RegionSelectionFocus`; its
absence dispatches to the operationally unchanged ordinary selector, and the
production `AddressSpace` call still supplies no focus. Focused discovery uses
full descriptors and production successor order, with a fail-closed 1,024
accepted-block safety bound independent of the unchanged 64-block/2,000-IR
selection budgets. Deterministic shortest paths construct the required closure;
a real direct terminal edge is mandatory. Required blocks are budgeted first,
then connected optional blocks fill in accepted-discovery FIFO order. Rejected
optional parents cannot admit descendants, every selected block is checked for
entry reachability, and stale or impossible focus throws rather than returning
an ordinary or partial region. Focused statistics describe optional fill only.

The focused suite passes 33/33, including exact budget and 1,024-block
boundaries, full-descriptor aliases, direct-edge classification, cycles,
shortest-path ties, required-first displacement, deterministic ordering and
fail-closed behavior. Full native CTest passes 8/8; the detailed frozen-corpus
suite passes 26/26 and fresh native output remains byte-identical in all five
cases. Independent review found no implementation blockers; its two test-
evidence findings were fixed and re-reviewed with no remaining findings.
This milestone implements only the selector mechanism, not A64 configuration
or address-space wiring, and is not runtime or performance evidence.

Next: TDD the strict CMake/C++ parser and immutable per-instance A64 hook.
Absent, explicitly empty and nonmatching focus must preserve the same frozen
raw Wasm corpus. Then add real generated-Wasm correctness tests and complete
whole-Run transfer comparisons before considering paired complete-duration
CoreMark qualification. Do not infer speedups from selection shapes, compile
costs, module sizes or transfer counts, and do not repeat rejected cache pruning.

The committed selector was also reproduced from a fresh clone: after building
all test executables, the complete native suite passed 8/8. An initial CTest
invocation with four unbuilt executables reported `Not Run`; this was a test
invocation error, not a product assertion failure.

## Default-off A64 focus configuration (2026-10-27)

`DYNARMIC_JS_A64_REGION_FOCUS` is now a private, default-empty CMake string.
Its grammar is exactly
`entry,exit_block,direct_target[,return_block...]`; every descriptor is `0x`
plus 16 lowercase hexadecimal digits, and return descriptors must be strictly
increasing. Different roles may intentionally alias to represent direct
self-loops and zero-length required paths; consecutive return blocks remain
strictly ordered. CMake rejects malformed nonempty values. The C++ parser
applies the same full-string grammar, and `A64AddressSpace` construction throws
if a nonempty compiled definition cannot be parsed, so invalid focus cannot
disable itself or fall back to ordinary selection.

Each A64 address space deeply owns an immutable optional focus. The base
address-space hook returns no focus, leaving A32 and default A64 selection
unchanged; A64 supplies focus only for an exact full-descriptor entry match.
Focused selection failures propagate without an ordinary-selection retry.
There is no public JIT API, runtime environment variable or mutable global.

Nine native parser/hook tests cover empty and boundary values, deep ownership,
intentional cross-role aliases, 10,000 ordered returns, malformed grammar,
exact/nonmatching/same-PC aliases, and stale-focus failure. CMake grammar and
four configure-integration tests pass. Full default native CTest passes 11/11,
and the enabled nonmatching hook test passes. Fresh recordings with the
variable absent, explicitly empty, and valid but nonmatching all pass the
frozen corpus comparator; direct `cmp` also confirms raw-byte identity for all
five modules in all three modes. Independent review exposed two test-only
fixture assumptions: ORing an already-set single-step bit, and treating
`exit_block` as necessarily different from `entry`. The alias now toggles the
bit, the nonentry case toggles an entry PC bit, and positive tests pin valid
cross-role repetition. Default, nonmatching and entry-equals-exit builds pass
9/9; the configured-bit-57 case also passes. Final independent re-review
withdrew the repeated-role concern and found no remaining findings. The
committed configuration was also reproduced from a fresh clone at
`/tmp/dynarmic-a64-focus-fresh-J8FulM`, where the complete native suite passed
11/11 after building all test executables.

This is configuration and compatibility evidence only. Matching-focus real
generated-Wasm execution, invalidation and multi-instance behavior remain
untested, and no candidate transfer or performance qualification has run.
Next: add those generated-Wasm correctness gates, strict ordinary regressions,
focus provenance and complete Run-scoped transfer conservation before any
paired complete-duration CoreMark measurement. Selection shapes, diagnostics,
compile timings, module sizes and transfer counts are not speedup evidence.

## Focused A64 generated-Wasm correctness (2026-10-27)

`tests/hosts/a64_region_focus_test.cpp` now exercises the real A64 selector,
structured emitter, Wasm runtime and public JIT with a synthetic guest whose
exact focus is
`0x0000000000001000,0x0000000000001800,0x0000000000001900,0x0000000000001904,0x000000000000190c,0x0000000000001a00`.
The focused checks pin all 64 selected full descriptors, emission and entry
reachability; execute both conditional directions cold and warm; cover internal
`bl`/`PushRSB`/`ret`, required callee and return blocks; and verify that the
direct target has no implicit standalone alias. They also cover same-PC
FPCR-distinct descriptors, ordinary single-step behavior, required-first
64-block displacement and newly external continuations, guest-code
invalidation, dependent direct-link/fast-dispatch state, and independent public
`A64::Jit` instances. A stale required path throws before publishing a cache
entry, compiled module, live table entry, diagnostic region or statistics
instance; there is no ordinary fallback. Empty and nonmatching configurations
retain ordinary generated-Wasm behavior.

All five configurations were rebuilt and passed after the final build change:
default without diagnostics, default with diagnostics, exact focus with
diagnostics, compiled nonmatching focus, and exception-enabled stale focus.
`DYNARMIC_JS_A64_REGION_FOCUS_TEST_EXCEPTIONS` is default-off and scopes
`-fexceptions` plus exception catching to the stale-test configuration only;
ordinary, diagnostic, exact-focused and nonmatching builds contain none of
those flags. This prevents the test host from instrumenting later benchmark or
production JIT builds. The neighboring alias, instance and region-diagnostics
hosts pass, as does the complete 11/11 native suite and `git diff --check`.
Independent review initially found the global exception-flag contamination;
after the scoped option and complete rebuild/rerun, re-review approved with no
remaining findings.

An additional strict invocation against the diagnostics-enabled, examples-off
nonmatching build was not accepted: `cache-population-thunk-4096` aborted with
`Aborted(OOM)`, and the browser artifact was absent. The raw log is retained at
`/tmp/dynarmic-a64-focus-strict-nonmatching.log` and ends with
`regression done fail=1`. That invocation is neither a passing regression nor
performance evidence. The ordinary configured gate uses a separate examples-on,
diagnostics-off nonmatching build; no memory or performance budget was widened.
After a full rebuild of every ordinary host, the default strict regression
completed with `regression done fail=0`; external CoreMark was not requested.
The separate examples-on, diagnostics-off nonmatching-focus strict regression
also completed with `regression done fail=0`, including browser normal/fault
halts and no requested CoreMark workload.

This is correctness and build-isolation evidence only. No candidate CoreMark
performance qualification, complete candidate transfer comparison or net
transfer/speedup claim has been made. Next: record exact focus provenance, run
focused diagnostics-plus-profile checks, and verify complete Run-scoped
transfer conservation and whole-history edge accounting before considering
paired complete-duration measurement. Do not repeat rejected cache pruning.

## Focus provenance and Run-only conservation (2026-10-27)

The evidence pipeline now records `build.a64RegionFocus` verbatim from the
configured CMake cache. New reads require the cache key: an explicitly empty
value is an unfocused baseline, while a missing key identifies a stale build
and fails closed. Shared report validation remains compatible with historical
reports that lack the field, but validates the exact lowercase full-descriptor
grammar whenever it is present. Return descriptors must strictly increase;
intentional cross-role repetition remains valid.

When a CoreMark diagnostic sample contains both transfer diagnostics and a
profile, report construction now requires exactly one Run, zero Steps and safe
nonnegative profile counters. It independently recomputes
`sum(module entries) = dispatchCount + sum(external transfers)` with the
existing BigInt helper, rejects inequality, and retains the exact result as
`transferConservation`. Reports without either input do not gain a fabricated
field. Production counters, raw diagnostic schemas and generated Wasm are
unchanged.

TDD captured six intended initial failures, plus an omitted-profile failure,
before implementation. The focused environment/collector/transfer/report suite
passes 65/65 independently, full native CTest passes 11/11, and independent
review approved with no findings. The ordinary strict regression completed
with `regression done fail=0`, including browser normal/fault halts; external
CoreMark was not requested. No CoreMark workload or performance qualification
has run for this milestone.

The retained historical transfer reports remain useful mechanism evidence but
cannot satisfy the new gate: they lack profile counters, measured dispatch
count, conservation and focus provenance. A fresh same-source default/candidate
pair is therefore required. Their archived filenames also reverse the actual
host memory-mode names; new evidence must use explicit numeric modes rather
than propagate those labels. Next: complete the ordinary strict regression,
then add a separate focused-evidence analyzer/verifier and collect complete
Run-scoped histories with exact empty/nonempty focus provenance. Diagnostic
counts and compile timings remain mechanism evidence, never speedup evidence.

## Focused whole-history analyzer and verifier (2026-10-27)

`internals/measurements/2026-10-27-focused-region-transfer/` now contains an offline
baseline/candidate analyzer, complete generation/edge comparison, non-overwriting
CLI and frozen-evidence verifier. It requires exact empty baseline focus and the
six-descriptor candidate focus, clean identical source/workload/build identity
apart from focus, A64 numeric memory mode `'0'` (callbacks/thunks), enabled
region/transfer diagnostics and profiling, one Run and zero Steps, normal halt,
matching reparsed CRCs, and different baseline/candidate Wasm artifact hashes.
It rebuilds all derived report fields from raw leaves and independently
recomputes Run conservation rather than trusting a serialized Boolean.

Generation identity uses the complete canonical generation apart from unstable
region/live IDs. Edge identity uses that full generation, full exit descriptor,
kind and static target; dynamic last-target metadata remains observational only.
The complete union retains added, removed and unchanged generations and every
zero-count exit. Duplicate identities, changed exit sites and ambiguous formerly
internal provenance fail closed. Newly externalized candidate edges retain their
raw candidate count and `baselineCount: null`; no former baseline-internal dynamic
count is reconstructed. Whole-Run external-transfer totals are mechanism
accounting only, and the analysis contains no duration, throughput or speedup
verdict. The verifier independently checks frozen bytes, inventory hashes and a
fresh semantic analysis, including coordinated raw/derived/analysis/inventory
tampering tests.

Strict TDD began with three missing-module failures and later reproduced the
review's mixed internal/external baseline-history bug as the sole failure in a
75-test run. The fixed suite passes 75/75 independently, with 98.45% line,
99.49% branch and 100% function coverage; full native CTest passes 11/11.
Independent re-review approved with no remaining findings. Real frozen hashes
remain intentionally empty until evidence is collected, so the real verifier
currently reports pins pending rather than fabricating a pass.

The Release diagnostics-plus-profile builds completed successfully with examples
enabled and test exceptions disabled. Baseline build
`build-em-region-diagnostics-profile` has explicitly empty focus; candidate build
`build-em-a64-region-focus-coremark` has the exact six descriptors. Their JS
SHA-256 is identical (`ee7dc93082bcd47a59d2e486dfb8cf6946349d5f82fee78566ec138b3f4f6852`),
while baseline/candidate Wasm SHA-256 values are respectively
`a3797eb1111f405007937be4648a4c604b02d74539fa07b7eaf12cff0cc789d5` and
`8d6689b7226237a03ce462bdd98bea9f2bb9e85945163128083825a250f7a868`.
No fresh baseline/candidate workload report, transfer comparison or performance
qualification has run. Next: commit this tested tooling, collect the fresh pair
from that clean revision, require conservation and complete mechanism accounting,
and stop if transfers are merely displaced or compile cost is disproportionate.

## Focused CoreMark whole-Run mechanism evidence (2026-10-27)

Fresh baseline and exact-focus candidate reports were collected sequentially at
clean source `b0a98606a748d6e4ba9f64bffaa30f823438846b`, using the pinned
200,000-iteration CoreMark workload, A64 numeric memory mode `0`
(callbacks/thunks), diagnostics and profiling. Both contain 111 aligned compile
and transfer generations, exactly one Run, zero Steps, normal halt and matching
native/guest CRC `0x4983`. Independently recomputed conservation passes with
dispatch count 171: baseline entries/transfers are 298,200,235/298,200,064 and
candidate entries/transfers are 232,600,235/232,600,064. The raw reports and
collection commands are frozen under
`internals/measurements/2026-10-27-focused-region-transfer/evidence/`.

The first strict analyzer invocation failed with
`missing focused return descriptors: 0x0000000000012418` and created no output.
Its synthetic fixture had incorrectly required `returnSite: true`; real required
return blocks are present, emitted and entry-reachable with `returnSite: false`.
The fixture and oracle were corrected through TDD, including negative tests for
missing, non-emitted and non-entry-reachable returns. The unchanged raw reports
then passed. This failure and correction are preserved in `commands.md` and the
README rather than hidden.

Whole-Run candidate-minus-baseline external transfers are -65,600,000:
-32,800,000 direct calls, -32,800,000 RSB returns and zero other-tail delta.
The complete comparison retains 108 unchanged, three removed and three added
generations; 439 unchanged, 29 removed and 33 added exits; and every zero-count
exit (376 unchanged-both-zero, 21 removed and 25 added). Three newly
externalized candidate direct edges have exact raw counts 27,999,999, 4,799,999
and 4,799,999, totaling 37,599,997. Their baseline-internal dynamic counts remain
unknown and are serialized as null. Mixed measured/internal baseline histories
fail closed. The candidate did not amplify compile-module count: both profiles
contain 111 modules; `regionBlocks` changes from 4,166 to 4,168 and emitted bytes
from 677,583 to 680,103 (+2,520). These are mechanism observations, not speedup
evidence.

The final tooling and real-evidence suite passes 79/79, native CTest passes
11/11, the frozen verifier passes and all four inventory hashes match. The
verifier independently pins raw bytes, source/workload/artifact identities,
partition counts, whole-Run totals, focus application and newly externalized
records, then recomputes fresh analysis from the raw reports. Independent review
caught one README-only aggregate error (37,599,998 instead of 37,599,997); the
narrative was corrected to match the already-correct raw rows and verifier.
No paired performance qualification has run. The mechanism package is the
prerequisite for a separately documented complete-duration baseline/candidate
experiment, not a substitute for it.

## qemu-wasm exact-workload comparison (2026-10-28)

The original unmodified 200,000-iteration A64 CoreMark ELF was run under the
official qemu-wasm `raspi3ap` runtime using a generic-loader adapter for the
existing SVC exit/output/microsecond-timer ABI. The guest, adapter, loader, Wasm
and worker hashes are frozen in
`internals/measurements/2026-10-28-qemu-wasm-comparison/`; no Linux guest or rebuilt
CoreMark was used. Native system QEMU ran the same guest/adapter generation to
CRC `0x4983`, providing a correctness oracle only.

qemu-wasm reported 364.169999 guest seconds and 549.194059 CoreMark/s, but final
CRC was `0xf2d2`, not `0x4983`; the process later hit the 900-second timeout.
The individual list/matrix/state CRCs passed, so CoreMark's own correctness line
did not catch the accumulated-final-CRC mismatch. No valid performance
comparison or formal speedup is claimed. Dividing the existing eight-pair
loaded-host dynarmic references (3310.419766 baseline and 3342.018013 candidate
CoreMark/s) by this incorrect qemu-wasm execution gives diagnostic ratios of
6.03x and 6.09x only. These are retained explicitly as invalid-execution
observations, not benchmark results.

The harness review found and the follow-up source fixed nonzero-exit ambiguity,
missing SVC exception-class checks, clock multiplication overflow and incomplete
failure termination. The rebuilt adapter then passed native system QEMU with
`0x4983` and an explicit exit marker; it was not substituted into the frozen
qemu-wasm observation. Next: diagnose the qemu-wasm hot-TB/Wasm correctness
failure, obtain a valid `0x4983` run, and only then collect a fresh controlled
cross-implementation cohort. Preserve the failed observation unchanged.

## Native three-way CoreMark comparison (2026-10-28)

Added a standalone optimized native x86-64 dynarmic A64 runner and matched
static Linux-musl CoreMark builds, then completed a fresh-process five-round
comparison of x86 native, qemu-user AArch64 and native dynarmic AArch64. All 15
samples exited normally with all expected CRCs and accumulated `0x4983`; all
five dynarmic samples used flat-window mode 2 and normal `UserDefined1` halt.
Median guest-kernel times and derived throughput are:

- x86-64 native: 5.167138 s, 38,706.15 CoreMark/s.
- qemu-user 6.2.0: 20.287766 s, 9,858.16 CoreMark/s.
- native dynarmic x86-64 JIT: 18.368734 s, 10,888.07 CoreMark/s.

Native dynarmic therefore delivered 1.1045x qemu-user throughput (10.45% higher,
9.46% lower guest time) and 28.13% of x86-native throughput on this fixed
workload. This is not an official CoreMark score or a pure engine-only ratio:
qemu-user executes a hosted Linux-musl build while dynarmic executes the pinned
freestanding guest, and x86 samples are below ten seconds. Guest ticks, dynarmic
Run time and process wall time remain separate.

The complete implementation, tests, build/reproduction commands, raw streams,
artifact manifests and analysis are under
`internals/measurements/2026-10-28-native-coremark-comparison/`. The final combined
comparison/analyzer/CoreMark protocol suite passes 52/52; JavaScript/Python
syntax checks pass, and a fresh analysis is byte-identical to the frozen report
(SHA-256 `16ffd6638b41d7639a00088dd5da7a0ab9d9ef08077fcf8d283020315990fd73`).
The collector and analyzer bind native executable identity to an optimized
post-link build manifest and independently reparse stdout/stderr. The complete
pre-review cohort remains preserved and excluded because it predates those
protocol fixes; it was not edited or selectively discarded. Alternate memory
mode 0/1 correctness streams are retained separately and were not used as
performance samples. Independent re-review approved the final protocol.

## Four-way dynarmic.js Wasm CoreMark comparison (2026-10-28)

Extended the native comparison harness with a separate config/completion/
analysis-v2 protocol and collected a fresh five-round, 20-process cohort at
clean harness revision `045107cd21d0e2bbd9fc8cf43baef74e5bea0a31`. The four
implementations rotate within every round: x86-64 native, qemu-user AArch64,
native dynarmic and dynarmic.js Wasm. Both dynarmic implementations execute the
same pinned freestanding A64 guest in flat-window memory mode 2; every Wasm
sample is a fresh Node v24.19.0/V8 13.6.233.17-node.51 process. All 20 samples
exited normally with every expected CRC and `crcfinal=0x4983`, and both JITs
reported normal `UserDefined1` halt.

Median guest-kernel times and throughput from this complete cohort are:

- x86-64 native: 6.295871 s, 31,766.85 CoreMark/s.
- qemu-user 6.2.0: 24.703262 s, 8,096.10 CoreMark/s.
- native dynarmic x86-64 JIT: 23.479227 s, 8,518.17 CoreMark/s.
- dynarmic.js Wasm JIT: 28.971689 s, 6,903.29 CoreMark/s.

Native dynarmic delivered 1.0521x qemu-user and 1.2339x Wasm throughput;
qemu-user delivered 1.1728x Wasm throughput. Native dynarmic and Wasm took
3.7293x and 4.6017x x86-native time respectively. All ratios use only median
guest ticks from this cohort; none mix the older three-way samples, process
wall time, Run time or diagnostic counters. Wasm `runMs` surrounds only
`_arm_run`, includes cold compilation/publication and excludes module loading,
RAM setup and JIT construction. The x86 samples are below ten seconds, so this
fixed-workload comparison is not an official CoreMark score and the whole
cohort is not duration-qualified. Hosted/freestanding differences still
preclude treating qemu/dynarmic ratios as pure engine overhead.

The Wasm production artifact was rebuilt from the clean harness commit with
Release `-O3`, profiling/region diagnostics/focus disabled and examples
enabled. An `arm_jit` POST_BUILD manifest binds source/upstream revisions,
clean dirty-state observation, actual compiler flags, empty configure/build
`EMCC_CFLAGS`, build scripts and actual JS/Wasm hashes. The collector freezes
that manifest and independently freezes runtime and all runner-module hashes;
children receive only a fixed minimal environment. Raw stdout/stderr bytes,
hashes and sizes are preserved. Collector and analyzer share fail-closed
validation against direct or indirect build-flag injection, and the analyzer
independently reparses every stream and all provenance.

The expanded suite passes 240/240 with 91.03% line, 92.36% branch and 96.51%
function coverage. Independent code and security reviews approved formal
collection with no CRITICAL, HIGH or MEDIUM findings; one LOW suggestion
remains to add a synthetic real-CMake configure/build fixture for environment
capture, although the actual clean production build did exercise and record
that path. A standalone Wasm correctness smoke and one-round four-process
integration queue passed before formal collection. JavaScript/Python syntax
checks and `git diff --check` pass. Original v1 evidence remains byte-identical
under fresh analysis with SHA-256
`16ffd6638b41d7639a00088dd5da7a0ab9d9ef08077fcf8d283020315990fd73`.

Complete raw four-way streams, records, manifests and analysis are preserved
under `internals/measurements/2026-10-28-native-coremark-comparison/
evidence-four-way/`. Independent reanalysis of both the temporary cohort and
the preserved copy was byte-identical; frozen analysis SHA-256 is
`d3c7bed8ae3bb431b1111a33db765c783ed17d9f1caeb6371e0ad224b8a4c5fc`.
The primary same-guest performance gap is now explicit: native dynarmic has
23.39% higher throughput than the Wasm backend on this workload. Next use
controlled Wasm/native profiling to attribute that gap to hot generated-code
and dispatch paths; compile counts and module sizes remain diagnostics, never
speedup evidence.

## Board return chaining continuation (2026-10-29)

The joint xhalf-hle performance request explicitly authorizes backend changes.
Board-mode `PopRSBHint` now uses the same full-descriptor fast-dispatch cache as
ordinary indirect branches. It retains budget, halt, board flags and conservative
hook checks. Exact single-step returns before either cache path; table-disabled
returns remain ordinary dispatch. The legacy non-board RSB path is unchanged.

Validation: native build, 10 CTest targets, generated IR smoke (returns and
indirect edges both tested for hit/miss, descriptor mismatch, budget, halt,
board flags and hooks), Emscripten instance tests (cold/warm return, budget,
range invalidation, both hook specialization modes), and
`tests/regress.sh build-em build-native` passed (`regression done fail=0`,
including available differential fuzz and 18 browser normal/fault halt runs).
The historical `region_corpus` raw-byte test fails at
`a32_conditional_backward`; rebuilding original HEAD reproduces the same
failure. Its snapshot was not updated. CoreMark was not requested by that gate.

This establishes semantics, not application speedup. Measure corrected warm
navigation with matching production artifacts before integrating the new pin.
Logs currently live in `/tmp/xhalf-return-*`; no firmware data is involved.

## Optional post-tick board observer (2026-10-29)

`CreateA32Jit` accepts an optional native `BoundaryObserver`. Boundary dispatch
invokes it after committing AddTicks and before the next allowance/IRQ query,
with materialized registers. Hosts may consume native interpreter exits and
redirect PC without ending/reentering Run. Halt/cache invalidation and active
module ownership remain the existing JIT's responsibility; reentrant execution
or destruction is forbidden. Ordinary construction and Step do not invoke it.

Native build, 10 CTest targets (the previously reproduced historical corpus
failure remains separate), generated IR execution, Emscripten instance/lifecycle
hosts passed. The independent instance test observes charged ticks before PC
redirection and checks that observer installation does not leak between JITs.
No host integration or application throughput claim is made by this API commit.

## A32 board local cycle budget prototype (2026-10-29)

Cycle-counted board modules retain their signed 64-bit remaining budget in a
Wasm local and commit it before every host call, return and tail transfer.
Host calls reload the budget afterwards. The generated per-block boundary
checks read the local; ordinary A32/A64 counting keeps its existing path.
Native 10-target CTest, generated boundary/static/indirect IR checks including
negative and >32-bit allowances, and Emscripten instance/lifecycle hosts passed.
The previously documented `region_corpus` baseline mismatch remains separate.
Application measurement is pending; this is a tested prototype, not a speedup
claim. Raw current logs: `/tmp/xhalf-local-cycle-*`.

## Native host-call IR lowering (2026-10-29)

The existing `CallHostFunction` IR (zero to three i64 arguments) now lowers to
an indirect Wasm table call, using the existing register/cycle flush and reload
hooks. Modules containing this IR import the table even without branch links.
Independent generated modules validate/execute all four signatures; the full
IR smoke remains green. This unblocks hosts retaining native system work in a
region; host timing/accounting is still the embedding application's contract.

## Explicit active A32 tick commit (2026-10-29)

`CommitA32ExecutionTicks` commits the materialized budget from a callback in the
active Run and advances the accounting origin, preventing Core from charging it
twice. Boundary observers see a consumed origin after AddTicks. Active callback
ownership is scoped/restored across nested runs of other instances; same-JIT
reentry remains forbidden. The instance observer checks repeated commit after
AddTicks is a no-op. Emscripten instance and native build passed. Integration
must still test timer/IRQ ordering and callback faults before adoption.

## End of joint optimization round (2026-10-29)

The user requested stopping after this round. The smaller-region prototype is
withdrawn: corrected 100-navigation runs measured 16-block P95 33.11 ms versus
64-block P95 31.73 ms, with effectively equal work (two charged units difference).
The register-cache ablation was incomplete and failed real firmware startup;
its revert is already committed. Production defaults are restored. No 60 FPS
or <=16.7 ms P95 claim is established.

Retained semantic/API work this round: guarded board returns (`0450333`), native
post-tick observer (`cc89511`), local cycle budget (`07d7ff2`), host-call IR
(`0e9c42c`) and explicit active tick commit (`8e00414`). Local budgets passed
`tests/regress.sh build-em build-native` with fail=0 including available fuzz
and 18 browser normal/fault halt runs. Later APIs passed generated IR/instance
checks; they are not a release certification or an application speedup claim.
The pre-existing A32 region_corpus byte mismatch is reproduced on the original
HEAD and was not changed. General frontend submodule remains untouched.

## A32 page-contained accesses (2026-10-30)

New joint optimization request targets warm navigation processing P95 <=16 ms.
A32 page-table accesses now check the full access fits the selected 4 KiB page.
This fixes the generic path's assumption that adjacent guest pages are contiguous
and permits unaligned accesses within a page. xhalf's old patch instead rejected
all unaligned loads/stores, including common raster memory operations.
Reference: https://github.com/copy/v86/blob/master/docs/how-it-works.md (read over
network today), whose fast path similarly checks page containment before loading.

Native IR tests pass all 1/2/4/8-byte loads/stores, both pointer representations,
unaligned offsets, last-fitting offsets, noncontiguous page crossings, null fast
entries and missing second-page faults. Results are checked against independent
byte-addressed callbacks, including callback counts. Full regression/application
performance evaluation follows; this commit does not claim a measured speedup.

## Joint round closed (2026-10-30)

Page-contained memory fix `a613273` passed full regression (fail=0, available
fuzz and 18 browser normal/fault halt runs), 10 native CTest cases excluding
the pre-existing region_corpus mismatch, 24 xhalf CPU contracts and five real
firmware regressions. It remains a generic correctness fix, not a measured
application speedup. Three alternating application pairs had mixed tails and
two slower candidate medians; xhalf retains its previous pin/alignment guard.

Guarded singleton loops plus unused board RSB-write removal (`30935a6`) passed
IR, instance, the full backend regression, 24 host contracts and five firmware
regressions. They are withdrawn after three alternating matched 100-move pairs
against the original application baseline (candidate also included a613273):

| Pair | Baseline median/P95 ms | Combined candidate median/P95 ms |
| --- | --- | --- |
| 1 | 23.345 / 35.696 | 24.720 / 36.568 |
| 2 | 22.365 / 33.612 | 23.636 / 30.421 |
| 3 | 23.087 / 38.171 | 27.126 / 45.722 |

All runs charged exactly 288,222,314 units. No stable end-to-end gain;
no separate causal claim for RSB removal or singleton caching is supported.
The application is restored to backend 8e00414 and its matched runtime.
The <=16 ms processing P95 target remains unmet. Firmware timing was preserved.
This round is closed; no further optimization is active. Raw evidence remains
in xhalf-hle `.private/performance/warm16-20261030/`; see its WARM16.md summary.
References consulted over the network include v86 page-contained fast memory,
QEMU TCG chaining, and https://v8.dev/docs/wasm-compilation-pipeline (preserve
bounded loop re-entry because optimized Wasm has no on-stack replacement).

## WASM IR semantic audit and packed SIMD lowering (2026-10-30)

Reviewed the current project documents and shared lowering with three parallel
integer/vector/FP audits, cross-review, and a separate final read-only review.
See [IrAudit.md](IrAudit.md) for scope, independent references and remaining gaps.
Starting revision: `930278597e9a288fd1e835f2814644eca5fe289b`. Upstream and unrelated
untracked `node_modules/` / `test-results/` remain unchanged.

Confirmed fixes: A64 1–16-byte memory operations now require page containment;
WithFlag scalar saturation no longer spuriously writes FPSCR.QC; VectorEqual128
now validates; VectorSignExtend64 uses the low lane's sign; widening doubling
multiply ignores unused high lanes for QC; scalar/vector numeric min/max share
correct sNaN/DN/FZ/signed-zero/IOC/IDC handling. Packed saturating arithmetic now
uses Wasm SIMD. Variable shifts discard redundant emission, and doubling
multiplies reuse their saturation masks.

Verification commands and results:

- `cmake --build build-native -j4` and `cmake --build build-em -j4`: succeeded.
- Four new CTest/regress gates passed: integer 1,115,100 cases; vector 83,180;
  FP numeric min/max 77,440 result/FPSR checks; memory 2,880 byte-reference cases
  plus null/tagged mappings and read/write faults.
- `ctest --test-dir build-native --output-on-failure`: 14/15 passed. The known
  `region_corpus` A32 `a32_conditional_backward` raw-byte mismatch remains; its
  earlier and final logs are retained. Its CMake driver rebuilds the recorder,
  so it must not be run while shared native artifacts are used for a baseline.
- `tests/regress.sh --strict build-em build-native`: exit 0, `fail=0`, including
  A32/Thumb/A64 Unicorn campaigns, all memory modes, saved corpus, and 18
  browser-host normal/fault halt runs under Node. No dependency skip. CoreMark
  and actual browser engines were not measured in this round.
- Five-process fixed-suite before/after: all 12 comparison cases pass. These
  sequential cohorts are a regression screen, not a controlled speedup claim.
  A64 page-table throughput ratio is 0.9659 and emitted-byte ratio is 1.0316.

The [packed experiment](measurements/2026-10-30-packed-saturation/README.md)
retains matched five-pair samples, loaded/unmatched pilot cohorts and frozen
modules. Its eight 64-operation kernels reduce median elapsed time by
57.14–73.03% and module bytes by 87.03–93.16%. This is a microbenchmark result.
The [vector experiment](measurements/2026-10-30-ir-audit/vector/README.md)
reduces 40 fixture modules by 430 bytes, but two of five timing pairs are slower;
no stable throughput improvement is claimed. Correcting FP semantics increases
48 fixture modules from 14,192 to 19,996 bytes overall, despite shrinking the
ordinary scalar/32-bit vector paths. All source/artifacts and raw validation
outputs are recorded in the [audit evidence](measurements/2026-10-30-ir-audit/README.md).

Next concrete semantic work: minimized guest reproducers for fixed-to-FP
rounding and FMULX input flushing, then a shared strict FP policy for general
arithmetic FPSR/FPCR behavior. The existing double-to-single ties-away
approximation remains. These changes do not certify complete FP semantics,
all IR combinations, application performance or a production release.
