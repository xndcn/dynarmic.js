# Rejected global 128-block region experiment — 2026-10-24

Base: `8092826`; upstream Dynarmic: `a41c380`. The only candidate source change
was `RegionSelectionLimits.max_blocks: 64 -> 128`. The 2,000-IR-instruction and
4,096-byte guest-distance limits remained unchanged. Production source retains
64. This was a diagnostic screen, not an accepted optimization or a complete
performance comparison.

## Method

Two independent Emscripten Release builds were made from the same isolated
checkout, with profile counters OFF and identical configuration. The baseline
was built before the one-line edit. Each diagnostic child loaded the 120,000-
iteration A64 CoreMark binary, enabled generated guest names, and captured one
CPU-profiled flat-window Run with affinity `0,2,4,6`. Generated modules were
dumped outside benchmark collection. The native executable supplied an
independent CRC oracle. Node v24.19.0 / V8 13.6.233.17-node.51 and Emscripten
6.0.13 were used on the development host.

CPU-sampled Runs and phase values below are diagnostics, not benchmark scores.
The first candidate capture overlapped a native build; its raw data is retained
as `candidate-concurrent.*` and excluded from all timing observations. The
matched baseline and candidate were then run sequentially without another
workload. No samples were discarded to seek a desired result.

## Objective screening result

| Metric | 64 blocks | 128 blocks | Candidate / baseline |
| --- | ---: | ---: | ---: |
| Compiled entries | 111 | 95 | 0.856 |
| Emitted bytes, including diagnostic names | 711,948 | 875,375 | **1.22955** |
| Instrumented Run (diagnostic only) | 13.875 s | 13.638 s | 0.983 |
| Guest kernel (diagnostic only) | 13.751 s | 13.512 s | 0.983 |

All three Runs exited normally and passed native/guest CRC checks. The emitted-
byte ratio exceeds the existing 1.10 acceptance limit, so the candidate fails
before a performance cohort is justified. The apparent single-Run time
reduction is not a speedup claim: it is within run-to-run variation, includes
compilation and tiering, and uses sampling instrumentation.

Generated-code inspection shows substantial growth and duplication, not a
simple removal of hot boundaries. `externalTransfers` and `dispatchTargets`
below are **static** WAT counts; table labels include the default label. They
are not dynamic call counts.

| Entry | Side | Wasm bytes | External transfer sites | `br_table` labels |
| --- | --- | ---: | ---: | ---: |
| `a64_11700_region` | baseline / candidate | 22,829 / 22,829 | 4 / 4 | 46 / 46 |
| `a64_11fd0_region` | baseline / candidate | 28,207 / 42,497 | 22 / 12 | 67 / 93 |
| `a64_1202c_region` | baseline / candidate | 21,340 / absent | 19 / — | 67 / — |
| `a64_1209c_region` | baseline / candidate | 21,622 / absent | 19 / — | 67 / — |
| `a64_10b00_region` (`iterate`) | baseline / candidate | 21,973 / 59,220 | 10 / 24 | 67 / 131 |
| `a64_10b18_region` (`iterate`) | baseline / candidate | 21,973 / 59,220 | 10 / 24 | 67 / 131 |

The separate `1202c` and `1209c` entries disappear into larger regions, while
both `iterate` entries still exist with equal larger bodies. This demonstrates
changed region shape, but does not establish that dynamic hot calls or their
state flushes were profitably eliminated. Existing dispatch and link-patch
counters cannot answer that question: chained tail calls bypass dispatch and
link patches count resolution, not executions.

## Decision and next work

**Reject 128 and retain 64.** Do not start a 96/128/256 complete-duration sweep:
the first larger candidate already violates the byte budget and does not provide
dynamic absorption attribution. No full paired CoreMark, fixed/cache comparison,
browser performance run or release claim was made.

A future region follow-up must first add diagnostic per-region selected-
descriptor/duplication records and, if needed, diagnostic-only execution counts
for external direct-call, RSB-return and other tail transfers. Any such counters
must stay out of production generated code and timing builds. A selective
hot-call policy would require fresh evidence and all existing correctness,
cache-invalidation, compilation, byte and complete-duration gates. Neutral or
noisy results leave the default unchanged.

Focused native `region_selector` and `region_plan` tests pass (2/2), and static
review found no new correctness defect in the constant change. An initial build
command mistakenly requested the nonexistent `cfg_planner_test` target; the
retained log records that command failure. The corrected `region_plan_test`
build and CTest invocation passed. Large-boundary emission, SMC beyond block 64,
full differential regression, sanitizers and browser checks were **not** run for
this rejected screen; these results are not release qualification.

## Retained evidence and audit

`evidence.tar.gz` retains the candidate patch, environment/artifact observations,
workload manifest and native output, three raw CPU profiles and recomputed
summaries, ordinary sample records, hot baseline/candidate Wasm and folded WAT,
module-shape summaries, generation scripts, and focused test logs. SDKs,
compiled hosts and external CoreMark binaries are not vendored. Local hashes and
source observations are not independent provenance attestation.

From the repository root:

```sh
mkdir /tmp/region-size-evidence-audit
tar -xzf internals/measurements/2026-10-24-region-size/evidence.tar.gz \
  -C /tmp/region-size-evidence-audit
node internals/measurements/2026-10-24-region-size/verify.mjs \
  /tmp/region-size-evidence-audit
```

The verifier checks native/guest CRCs, recomputes all profile summaries and
derived observations, validates hot Wasm/WAT shape counts and build-config
compatibility, and checks every retained file hash. Expected output:

```text
region-size evidence PASS profiles=3 files=45
```
