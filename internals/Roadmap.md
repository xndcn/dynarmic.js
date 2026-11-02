# Open-source release roadmap

Status: active development; no production release or leadership performance claim.
Owner: repository maintainers. Updated: 2026-10-24.

## Goal and evidence

Build an ARM32/Thumb/AArch64 to WebAssembly JIT with exceptional sustained
throughput, bounded compilation latency and predictable memory consumption.
“Fastest” is an objective to test, not an established property. Claims need
reproducible workloads, equivalent semantics, engine/toolchain revisions,
raw samples and comparisons on the same machine. Correctness is a release
requirement, including slow paths and architectural corner cases.

Keep each migration independently reviewable and commit at verified milestones.
Replace existing code when the new design is better; retain behavior through
independent tests rather than preserving implementation structure. Do not merge
a speculative optimization solely because it wins one synthetic loop.

## Architecture decisions

The existing frontend/IR → emitter → cache → runtime split is the starting
point, not the final decomposition. Migrate towards these explicit interfaces:

| Component | Owns | Must not own |
| --- | --- | --- |
| Frontend adapter | Decode, optimize, guest descriptors, state layout | JS engine policy |
| Region selection | Bounded exploration, eligibility, selection metrics | Wasm byte emission |
| CFG planner | Reachability, dominators, reducibility, layout plan | JitState, callbacks, engine imports |
| Lowering | Typed operations, explicit state effects, local allocation | Cache lifetime |
| Module encoder | Valid Wasm sections, imports, exports, ABI | Guest scheduling |
| Code cache | Module/link ownership, range invalidation, memory budgets | Guest semantics |
| Execution runtime | Run/Step, budgets, halt, callback boundary | Region heuristics |
| Observability | Per-instance snapshots, compile stages, artifact identity | Production hot-path branches |

Start by extracting the pure CFG planner, with independent graph properties
and actual emitted-module regression coverage. Next separate region selection
from cache ownership, then replace mutable emission-state coupling with
explicit lowering plans. Avoid a single rewrite that cannot be compared against
the previous backend.

The current synchronous Worker runtime remains the supported path. Async
compilation requires cancellation/generation ownership, a cold execution path,
and invalidation-during-compile tests before adoption. Tiering, SIMD lowering,
multi-entry regions and register/flag residency are experiments gated by data.

## Delivery sequence and acceptance

| Milestone | Deliverables | Exit evidence |
| --- | --- | --- |
| M0 — trustworthy baseline | Source audit, roadmap/handoff, strict benchmark artifacts and comparator, automated unit tests | Clean builds, regression report, raw baseline with explicit limitations |
| M1 — compiler boundaries | Pure CFG planner; separate region policy and module ownership; explicit ABI/state effects | Graph property tests, Wasm validation, differential regression, no unreviewed code-size/latency/throughput regression |
| M2 — semantic hardening | Run/Step/halt/reset lifecycle, callback visibility, wraparound/zero-length invalidation, cross-page memory and faults, exclusives, FP modes | A32/Thumb/A64 × memory modes × supported engines; reduced failure corpus; sanitizer gate |
| M3 — performance coverage | A64/Thumb fixed kernels; integer/FP/SIMD/branch/indirect/call/memory workloads; CoreMark; per-instance compile metrics; cache pressure | Multiple fresh processes, explicit warm-up/tiering, dispersion, production and profile runs, browser artifacts |
| M4 — measured optimizations | Region/cache/local allocation and helper-boundary experiments selected from profiles | Paired before/after artifacts, emitted-code review, semantic tests; reject unjustified regressions |
| M5 — public release | Supported embedding API, capability probe, install/export packages, examples, CI, licenses/notices, security and contribution policies | Clean checkout build, Node + Chrome + Firefox Worker matrix, documented limits, release checklist signed off |

M0 and the first isolated M1 extraction are the initial delivery. Remaining
milestones must stay visible in the handoff; passing a smoke test does not
close an entire milestone. Do not publish or tag a release before M5.

## Test design

Use independent native references and Unicorn for semantics; preserve failing
seeds and minimal binaries. Separate exact tests from documented NaN/FPSR
allowances. Strict mode must not silently skip an unavailable oracle or browser.
Unit-test graph/layout algorithms and byte encodings without an Emscripten
build. Integration tests must validate emitted Wasm and execute both structured
and irreducible control flow. Run lifecycle tests with multiple live instances
and recycled table slots. Record actual process failures even if stdout contains
a PASS marker; retain failure logs.

Performance gates compare compatible experiments: CPU, engine, build options,
profiling mode, workload identity, instruction count and sampling methodology.
Never use an artifact hash as workload identity: the compiled host changes with
the implementation. Preserve the hash for provenance. Start with existing
review budgets (warm throughput -5%, emitted bytes +10%, compile phases +15%);
collect dispersion and repeat noisy results before making a claim. Cold Run
wall time includes execution and must be reported separately from phase timing.

## Known audit findings

- Statistics publication now reuses unchanged compile snapshots and publishes
  profile counters through a fixed-shape serializer. Snapshot/owner/heap-growth
  tests and both profile modes are part of the runtime gates; paired comparison
  tooling retains failed cohorts and child logs. Measurements and limitations
  are in [the publication experiment](measurements/2026-10-22-publication/README.md).
- The first measured frontend optimization replaces ARM's runtime decode-bucket
  construction with a constant compact index and static dispatch table. Its
  all-bucket/argument equivalence tests, retained failed experiments and final
  passing comparison reports are in
  [the ARM decoder experiment](measurements/2026-10-22-arm-decoder/README.md).
  This advances M4; it does not close the broader performance or release gates.
- The fixed host suite now covers A32, Thumb and AArch64 memory modes,
  AArch64 scalar/NEON arithmetic and A32 call/return. It still cannot establish
  browser-engine leadership, large-cache behavior or application-level
  performance; CoreMark and browser reports remain separate evidence.
- Profile snapshots and compile totals are owned per JIT and retained under
  `Module.dynarmicWasmStats.instances[id]`. Consumers must select an instance
  explicitly when several JITs are live; the legacy top-level mirror is only
  the active instance.
- Deliberate cache invalidation pressure is now covered by the separate
  `cache-benchmark-v1` report and its `cache-comparison-v1` regression gate.
  The separate `cache-population-v1` suite now covers 1–4,096 retained A32
  modules and a complete zero-compilation revisit, with five-process comparison
  gates. Neither suite establishes large application cache behavior, engine
  memory consumption or a configured memory ceiling; those remain open.
- Native ASan/UBSan CI runs decoder equivalence, planner, selector and benchmark
  protocol tests. Emscripten runtime and browser sanitizer coverage is still open.
- The native build now exports a tested static CMake package and installs the
  license inventory. Consumer packaging still needs a clean archive review and
  an Emscripten-facing distribution story.
- The browser workflow now drives both Chromium and Firefox through a
  browser-neutral HTTP result callback. Performance artifacts remain Node and
  Chromium-specific until equivalent Firefox benchmark collection is added.
- Compile phase timers run in unprofiled Emscripten builds too. “Zero profiling
  overhead” only applies to dispatch counter updates, not all instrumentation.
- Default regression permits missing fuzz dependencies and browser artifacts;
  `tests/regress.sh --strict` is the fail-closed release gate and requires
  those inputs plus five-process benchmark collection. Passing it alone is not
  sufficient evidence for a release.
- Fuzz mismatches can be saved as `dynarmic.js/fuzz-case-v1` JSON and replayed
  with `tests/fuzz/replay.py`; six reduced checked-in cases now run through a
  dedicated corpus gate, while broader failure-derived coverage remains open.
- The root MIT license and third-party inventory are now present,
  but the exact tagged distribution archive still needs a final license review
  and installable consumer packages remain open. The Dynarmic submodule and
  bundled libraries retain their own notices; do not collapse them into the
  root license or remove existing files.
- Cache invalidation arithmetic, callback state coherence, exclusives and FP
  exception tolerances need targeted audit beyond existing fuzz coverage.

## CoreMark workload milestone (2026-10-22)

M3 now includes a versioned complete CoreMark collector for A32/A64 across all
three memory modes, a native CRC oracle, build/artifact identities and a strict
comparison protocol. Short correctness checks are distinguished from the
upstream ten-second duration requirement. External source is pinned in CI and
remains unvendored; CI checks both profile modes. The browser host now exposes
unhandled guest faults distinctly from semihost exit, with a runtime regression
for normal/fault/recovery behavior. This does not close M3: controlled full-length
comparisons, Thumb application workloads, browser performance/tiering evidence,
and populated-cache invalidation/engine-memory measurements remain open.


## Compilation boundary milestone (2026-10-23)

Measured follow-up to the complete-workload baseline moved C++ phase accounting
out of JS, avoided the main-table deduplication scan for fresh generated Wasm
functions, and moved unchanged compile-only publication checks ahead of the JS
boundary. Intermediate warm-latency regressions were retained and investigated
with GC/allocation traces; no gate budget was relaxed. The final fixed,
population (both profile configurations) and short CoreMark paired cohorts pass.
Runtime table ownership/reuse, ordinary host-function interoperability and
publication-boundary tests protect the new behavior. See the
[experiment record](measurements/2026-10-22-compile-accounting/README.md).

This completes another measured M4 change, not M3/M4 as a whole. Next, obtain
named compiler profiles for the larger translation/emission costs, combine
populated-cache selective invalidation with retained engine-memory measurements,
and establish browser/tiering and complete-duration application comparisons.
The generic Emscripten table helper dependencies must be revalidated on a
toolchain upgrade. Thread-confined synchronous execution remains the runtime
model; shared-memory/pthread support is not established by this milestone.

## Named CPU profiling milestone (2026-10-23)

M3/M4 now have a validated sample analyzer and a durable 30-process named CPU
cohort spanning fixed A32/Thumb/A64 kernels, populated A32 cache and complete
short A32/A64 CoreMark. The [experiment record](measurements/2026-10-23-named-cpu/README.md)
selects constant A64 decode buckets/static dispatch as the next measured
optimization: runtime table construction dominates the remaining frontend
sample share in both A64 workloads. Upstream SIMD priority exceptions and
ordered-bucket equivalence are explicit acceptance requirements. The profiling
milestone changes no generated code and makes no speedup claim. Cache memory,
browser performance, full-duration comparisons and release packaging remain
required work.

## A64 constant decoder experiment outcome (2026-10-23)

The profile-selected constant A64 decoder candidate passed semantic, sanitizer,
strict runtime and browser checks, and reduced fixed A64 compile medians by
61–79%. It **was not accepted**: all 60 long CoreMark executions passed CRC and
duration checks, but A64 page-table/flat-window kernel budgets and two additional
metrics failed. Large dispersion leaves causal attribution unresolved; the
short combined passing cohort does not supersede the long failure.

Production source remains at the prior behavior. The complete candidate patch,
all failed/passing cohorts and replayable audit are preserved in the
[experiment record](measurements/2026-10-23-a64-decoder/README.md). M4 remains
open.

## Release and sustained-performance work resumed (2026-10-24)

A new explicit long-term release/performance instruction supersedes the
historical pause above. The immediate priority is runtime evidence rather than
another compile-only experiment: rebuilt production and native trees at
`f95d6c9` pass the complete strict regression, and a fresh five-process fixed
baseline is retained with CPU affinity `0,2,4,6`. These warm observations still
include tiering and are not certified steady-state throughput.

The Run-scoped profiling increment is complete. Six full A32/A64 CoreMark
profiles now distinguish thunk callbacks from page-table/flat generated-code
costs, and a separate V8 trace confirms TurboFan compilation of the five hot
A64 regions. Raw profiles, qualified baselines, generated WAT, commands and an
offline verifier are retained in the
[runtime-profile record](measurements/2026-10-24-runtime-cpu/README.md).
The profiler adds no production hot-path instrumentation and makes no speedup
claim.

The bounded hot-call absorption/region-sizing screen is complete and **rejected**.
Raising only the global block cap from 64 to 128 changed the hot CFG but grew
A64 flat diagnostic emitted bytes by 22.95%; both `iterate` entries remained as
separate 59,220-byte modules with larger dispatch tables. Single CPU-sampled
Runs are retained but are not performance evidence. No complete-duration cohort
was started after the objective byte-gate failure. Raw profiles, Wasm/WAT,
candidate patch and an offline verifier are in the
[region-size record](measurements/2026-10-24-region-size/README.md).

Do not sweep larger global caps. Compile-boundary region histories now retain
full descriptors, entry identity and exact module/entry-body sizes under an
independent diagnostic switch. The first complete A64 flat-window CoreMark
history contains 111 regions, 4,177 selections, 862 unique descriptors and
3,315 duplicate selections; raw records and an offline verifier are in the
[region-diagnostics record](measurements/2026-10-24-region-diagnostics/README.md).
These are static duplication observations, not dynamic transfer counts or a
performance result.

The bounded `PushRSB` continuation ablation is complete as a **static diagnostic
screen**, not a production optimization. Suppressing 28 exact return descriptors
only at speculative `PushRSB` discovery reduced module bytes by 18,397 (2.60%)
and entry-body bytes by 18,103 (2.61%); selected blocks fell 23.20% and duplicate
selections 29.02%. All 111 entries remained, six retained hot histories were
unchanged, and no timing cohort was run. The frozen candidate, raw histories,
fail-closed comparator and offline verifier are in the
[PushRSB ablation record](measurements/2026-10-24-push-rsb-ablation/README.md).
Do not generalize its workload-specific descriptor list.

The diagnostic-only v2 coverage milestone is complete. Its history records
layout, actual emission, original-entry reachability, return-site membership and
exact pre-miss live region-generation IDs. The complete ordered v2-to-v1
projection matches the retained history; all 111 fresh production/diagnostic
module pairs also match normalized WAT after masking only classified runtime
addresses. Among 82 later entries, exactly 16 have a live eligible A64 fallback
return-site owner. This passes the pre-registered >=9 threshold for **bounded
prototype planning/TDD only**, not a performance claim. Raw reports, all modules,
the comparison, coverage rows and tamper-testing verifiers are in the
[region-alias coverage record](measurements/2026-10-24-region-alias-coverage/README.md).

The bounded A64 fallback return-site prototype is now implemented and
semantics-qualified. Only actual non-entry return sites in emitted fallback
regions become full-descriptor aliases of one shared allocation. The export
selects by masked `state.pc` and fails closed on unknown PCs; structured,
single-step and A32 emission remain unchanged. Publication preserves existing
owners, and invalidation drops all aliases with exactly-once table/link release
and allocation-based accounting. Integrated tests cover direct links, cold/lazy
and warm RSB returns, fast dispatch, conflicts, mutation, stale-transfer
invalidation, single-step identity, clear/destruction and slot reuse. No
throughput or CoreMark comparison has been run for multi-entry reuse.

Stop after this correctness milestone as requested. Any subsequent performance
collection requires separate authorization; the 16/82 static coverage result
still does not establish dynamic hotness or promised byte savings. Do not resume
global cap sweeps, add dynamic generated-code counters without an unresolved
hot-edge decision, or broaden this into structured multi-entry. Independent
callback/API, bounded-cache/engine-memory, browser-performance and installable
web packaging release gates remain open.

## A64 alias static performance gate (2026-10-24)

The subsequently authorized static CoreMark qualification **failed before
timing**. The registered exact mechanism expected 95 candidate regions (111
baseline minus 16 statically covered later entries); the candidate compiled 99.
No short paired screen or complete-duration comparison was started, and the
18.66% generated-byte reduction is not a performance acceptance result.

The discrepancy is fully explained without changing the existing Boolean
coverage classifier. Fifteen predicted entries were alias-reused; `0x10380`
survived because its only predicted owner was itself absorbed. Five other old
entries became ordinary emitted interior blocks and eight new original entries
compiled. Thus `111 - 15 - 5 + 8 = 99`. Exact live-owner replay finds no
publication conflict explaining the difference. Baseline coverage cannot be
treated as a closed candidate compile-history prediction.

The failed reports, candidate patch, categorized analysis, commands and
fail-closed verifier are retained in the
[alias static-gate record](measurements/2026-10-24-region-alias-static-gate/README.md).
Do not rewrite the expected count to 99, discard the new-entry cohort or start
timing until a separately registered mechanism gate is satisfied. M4 and all
independent release gates remain open.
