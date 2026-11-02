# Compilation boundary experiment — 2026-10-22–23

Baseline: `87732d2`. The change keeps C++ phase accumulation in C++ and avoids
an unnecessary main-table scan when registering a fresh generated Wasm entry.
It does not change guest instruction semantics or region selection. All failed
intermediate comparisons are retained; the acceptance budgets stay unchanged.

## Evidence and implementation

A Node CPU profile of the existing 4,096-entry flat-window cache-population
workload exposed the `js_record_compile_phase` import and UTF-8 conversion.
Summed across all call-tree occurrences, that subtree accounted for 8.468 ms of
sampled time. The phase-only candidate replaces string keys with compile-time
`CompilePhase` selection and direct additions to the owning `CompileStats`.
Native phase writes remain no-ops. The engine-side timers stay in JS and the
public statistics schemas stay unchanged.

The first five-process sequential comparisons failed several existing budgets.
A 25-round alternating follow-up still failed profile cache-population warm
latency at 64 entries: candidate/baseline ratios were 3.041, 3.183 and 3.341
for thunk, page-table and flat-window modes. The phase-only fixed cohort also
failed A32 page-table warm throughput at 0.933. These were not waived.
Production population passed its paired gate.

GC traces of the profile thunk case localized the bimodal warm latency:
all five phase-only candidate traces had a Scavenge between the cold and warm
markers (roughly 0.53–0.76 ms), while the corresponding baseline traces had
none. Removing cold allocations had shifted the collection into the short
cache revisit. A combined GC/optimization trace also changed the collection
position, so diagnostic timings must not be treated as production scores.

Heap sampling with collected objects included then identified a larger cost.
Emscripten's first `addFunction` creates a WeakMap and scans every entry of the
main module's table, materializing Wasm function wrappers and a table mirror.
The phase-only 64-entry diagnostic sampled about 2,242,440 bytes in total;
`updateTableMap`, `getWasmTableEntry` and WeakMap `set` accounted for about
733,752 bytes. Fresh generated entries have unique function identities, so
this scan cannot find a duplicate of their entry function.

The registration path uses Emscripten's shared `getEmptyTableSlot`
and `setWasmTableEntry` helpers and updates `functionsInTableMap` only if it
already exists. Release clears the mirror/table and returns the index to the
same free list, removing a map entry when present. An embedding's later ordinary
`addFunction` still scans all currently live entries; generic and generated
functions can share freed slots safely. The dependencies are explicit through
`EM_JS_DEPS`, rather than relying on an incidental host export list.

The resulting heap diagnostic sampled about 1,490,536 bytes: the initial scan
and its wrapper/map allocations disappear. Three new GC traces measured
0.200–0.223 ms for the 64-entry profile revisit with no intervening Scavenge.
These figures are diagnostic allocation samples, not RSS or complete engine
memory measurements. They motivated another controlled cohort.

The table-only candidate passed the profile population, fixed and ten-round
CoreMark gates, but production population warm latency still failed at 512 and
4,096 entries (ratios 1.188–1.269 in the failing cases). Production GC traces
showed an occasional 0.44 ms Scavenge during a 4,096-entry revisit; they did not
explain all of the 512-entry variation. These reports are retained too.

An unchanged production publication still crossed Wasm→JS on every Run,
including u64-to-BigInt boxing, before the JS fast path could return. The final
candidate moves that unchanged-data check to C++: it caches only the last
compile-only owner and all eight double totals. It skips JS only for an exact
same-owner, same-value publication. Profile publications invalidate this cache
and continue to update every time; owner changes, phase-only updates and
compile-only/profile transitions remain observable. A potentially throwing JS
publication invalidates the cache before the call. Snapshot records remain
read-only and retained compile/profile leaves preserve their values.

## Test scope

`runtime_table_test` checks fresh instances, slot release before the generic
map exists, ordinary native/generated deduplication after lazy initialization,
bidirectional reuse between generic host and generated entries, mirror/map
cleanup and two 128-entry allocation/release cycles with stable table size.
Live entries must still execute correctly throughout. Linking this test against
the retained baseline library fails the scan-avoidance assertion as expected.
The production and profile candidate test executables pass.

`stats_test` also exercises actual phase accumulation with nonzero engine
counters, repeated additions, null pointers, two owners and publication after
heap growth. Existing frozen historical leaves and field-name oracles remain
in place. A property-access probe verifies that an unchanged compile-only publication
never reads the JS statistics object, while profiling still enters JS. Both
configurations pass. Table interoperability also passes with Emscripten
`ASSERTIONS=2`, and a normal population host runs with
`EXPORTED_RUNTIME_METHODS=[]`, checking the explicit library dependencies.
Runtime tests are in the strict regression
and profile-matrix browser workflow; hosted CI was not run locally.

## Reproduction and interpretation

Normal cohorts use Release builds, Node v24.19.0 / V8, Emscripten 6.0.13 and
an i7-12700 with affinity `0,2,4,6`. Baseline JS/Wasm/library files were copied
before source changes. Build configurations and artifact hashes are retained
in the reports; git metadata observes the configured checkout and is not a
build-time source attestation. No builds or other tests ran alongside the
performance collections. CPU frequency was not locked.

The ordinary paired driver alternates builds for each case and rotates cases
between rounds. `paired-coremark.mjs` instead alternates complete six-case
collections between builds, retaining every per-round report and child status.
It delegates process, CRC and provenance validation to the normal CoreMark
collector and revalidates the aggregated reports before comparison. This
experiment uses 4,000 iterations to study cold complete-workload behavior;
short results are not duration-qualified CoreMark scores or a steady-state
throughput claim.

```sh
taskset -c 0,2,4,6 node tests/bench/paired_run.mjs fixed \
  <baseline>/tests build-em/tests <before-template.json> <after-template.json> \
  /tmp/new-fixed --runs 25
# Repeat with 'population', and separately with profile builds/templates.
taskset -c 0,2,4,6 node internals/measurements/2026-10-22-compile-accounting/paired-coremark.mjs \
  <baseline>/examples/browser build-em/examples/browser /tmp/dynarmic-coremark-v1-final \
  /tmp/new-coremark 10
```

Use fresh output directories. Diagnostic CPU profiling used
`--cpu-prof --cpu-prof-interval=100`; GC attribution used `--trace-gc` and,
separately, `--trace-opt --trace-deopt`. `heap-profile.cjs` records inspector
allocation samples including objects collected by minor/major GC. The raw
profiles, intermediate reports and source/artifact hashes are retained in this
archive. This is a local compiler/runtime milestone, not a release sign-off or
performance-leadership claim.

## Final controlled results

`boundary-*` identifies the final implementation, including the C++ publication
cache. Earlier `final-paired-*` and `coremark-paired` filenames belong to the
table-only intermediate candidate; those names were retained to keep the
original observations intact.

| Final cohort | Fresh processes per case | Result |
| --- | ---: | --- |
| Production fixed, alternate each case | 25 | PASS all original budgets |
| Production population, alternate each case | 25 | PASS all original budgets |
| Profile population, alternate each case | 25 | PASS all original budgets |
| Production CoreMark short workload, alternate complete cohorts | 10 | PASS all original budgets; durationQualified=false |

Final production fixed cold compile-phase median ratios (candidate/baseline)
are 0.873–0.993, and warm throughput ratios are 0.957–1.108. Production
population cached-revisit ratios are 0.390–0.993; profile population ratios are
0.644–0.968. Cold population ratios vary: production 0.796–1.088 and profile
0.816–0.984. Not every cold case is faster. These are observations from this
host/cohort; warm fixed-kernel fluctuations are not claims of faster guest
instruction lowering.

The one-entry table-registration median falls from 2.312–2.506 ms to
0.149–0.179 ms in the production cohort and from 1.162–1.181 ms to about
0.090 ms in the profile cohort. Both cohorts use the same original counters
and timing boundaries. Table scan removal has a direct structural explanation;
frequency, scheduling and engine/GC timing still affect absolute measurements.

Fixed and population generated-byte totals are unchanged, as are profile cache
peaks. CoreMark kernel-time ratios are 0.975–1.038, Run ratios 0.975–1.035 and
compile-phase ratios 0.927–1.043. CoreMark byte-median ratios are 0.984–1.000:
A32 compiled-module counts vary from 112 to 115 across samples on both builds,
while A64 stays at 107. The complete program processes measured times/rates
when printing its output. Do not interpret this small A32 byte-median movement
as an emitter code-size improvement; use the deterministic fixed/population
workloads for exact emitted-size checks. All CoreMark CRCs pass.

## Archive and final verification

`evidence.tar.gz` holds 221 raw reports, process/build/test logs, profiles and
intermediate patches. `manifest.json` records every member's SHA256, the archive
hash, final implementation/driver source hashes and benchmark JS/Wasm hashes
for all retained stages. No workload source checkout or Wasm executable is
bundled. `heap-profile.cjs` and `check-browsers.py` preserve the diagnostic
recipes. Relative profile names `after` mean phase-only and `final` mean
table-only; the final accepted performance reports use `boundary` names.

```sh
mkdir /tmp/compilation-boundary-evidence
tar -xzf internals/measurements/2026-10-22-compile-accounting/evidence.tar.gz \
  -C /tmp/compilation-boundary-evidence
node tests/bench/compare.mjs \
  /tmp/compilation-boundary-evidence/boundary-paired-fixed/fixed-before.json \
  /tmp/compilation-boundary-evidence/boundary-paired-fixed/fixed-after.json
```

Native CTest passes 4/4. Both full strict regression runs, explicitly including
CoreMark, finish with `regression done fail=0`. Chrome 133 and Firefox Worker
smokes pass in production and profile builds. The final tagged-entry table test
passes both builds and `ASSERTIONS=2`, and the retained baseline library fails
its no-scan assertion as expected. The no-exported-runtime-method host also
passes. Browser smoke checks and diagnostic timings are not substituted for
the isolated controlled cohorts. No release/leadership claim is made.

Final archive verification checked the archive hash, all 221 member hashes and
sizes, all 16 source/driver hashes and all 28 final benchmark/browser artifact
hashes. Recomputing the four final comparisons from a fresh archive extraction
validated their input reports and reproduced all 42 case comparisons exactly;
every original gate still passes.
