# Restored-default Run-scoped CPU profiles — 2026-10-27

Source: `339939c59a3cbb84a2c3af4a7a415ed14625ef9d`; upstream dynarmic:
`a41c380246d3d9f9874f0f792d234dc0cc17c180`. The checkout was clean during
collection. This is a diagnostic milestone only: no backend lowering or region
selection changed, and no speedup is claimed.

The rejected structured register-cache candidate had been reverted before this
collection. A64 flat-window emitted bytes are 711,948 in every process, matching
the restored default rather than the rejected candidate's 675,969 bytes.

## Method

A separate Release Emscripten host was built at `/tmp/dynarmic-restored-profile`
with `--profiling-funcs`, `DYNARMIC_JS_PROFILE=OFF` and region diagnostics OFF.
Its JS/Wasm SHA-256 values, source/workload identities and collection script are
in `evidence.tar.gz`.

Three fresh processes per case profiled exactly one `_arm_run(0x10000)` after
initialization, with generated guest names enabled before initialization. The
six-case order rotated each round. All 18 Runs used the 200,000-iteration
CoreMark workload, affinity `0,2,4,6`, empty `NODE_OPTIONS` and no concurrent
builds/tests. Node v24.19.0 / V8 13.6.233.17-node.51 requested 100 μs sampling;
observed medians were 153 μs and p95 values were 161–162 μs.

Every profile passed normal-halt, CoreMark parsing and native/guest CRC checks;
every sample lasted at least ten seconds and had an empty stderr file. The
offline verifier recomputes all 36 summaries and both aggregate reports, binds
them to raw profile hashes, and checks all 97 payload hashes.

Instrumented Run values below are diagnostics, **not benchmark scores or paired
comparisons**. Shares are unweighted sample observations, not exact timings.

| Case | Median instrumented Run | Guest subtree / all samples |
| --- | ---: | ---: |
| A32 thunk | 72.260 s | 90.94% |
| A32 page-table | 33.109 s | 99.71% |
| A32 flat-window | 25.758 s | 99.58% |
| A64 thunk | 65.164 s | 92.55% |
| A64 page-table | 32.982 s | 99.58% |
| A64 flat-window | 24.532 s | 99.39% |

A32 page-table emitted bytes varied across processes (876,353, 850,546 and
850,546), consistent with its variable compilation history. These static values
are retained per process and are not averaged into a performance claim.

## Observed hotspots

The fresh cohort reproduces the earlier A64 hot-region distribution. Values are
medians of each function's per-process guest self-sample shares:

| A64 region | CoreMark symbol | Flat-window | Page-table |
| --- | --- | ---: | ---: |
| `a64_1209c_region` | `core_bench_state` | 16.82% | 15.15% |
| `a64_11700_region` | `matrix_test` | 16.75% | 15.79% |
| `a64_10b00_region` | `iterate` | 14.11% | 16.33% |
| `a64_10b18_region` | `iterate` | 12.37% | 17.78% |
| `a64_1202c_region` | `core_bench_state` | 10.56% | 9.68% |

The two `iterate` regions therefore account for 26.48% of flat-window and 34.11%
of page-table guest self samples when their median shares are summed. This
keeps bounded call/return-boundary investigation relevant, but function-level
profiles cannot distinguish arithmetic, memory operations, state transfers and
external calls inside a generated function. They do not establish that boundary
elimination will improve throughput.

Thunk profiles again show a different mechanism: the combined memory callback
and `dynarmic_wasm_*read*/write*` self-sample share is 49.36–49.61% for A32 and
56.52–56.68% for A64. These are separate memory-path observations, not evidence
that required callbacks or fallback checks can be removed.

The next profiling task is to obtain diagnostic-only exact external direct-call,
RSB-return and other tail-transfer counts for the hot regions before choosing a
runtime optimization. Static external-site counts alone are insufficient. Any
subsequent candidate must preserve the 64-block cap and pass correctness,
compile-time, emitted-byte and complete-duration throughput gates. Do not revive
the rejected cache-pruning candidate or resume global cap sweeps.

## Reproduce and audit

Use new output paths; the profiler refuses to overwrite existing files. The
archived `collect-round.sh` is the exact host-specific collection record, not a
portable CLI. Before rerunning it, replace its `root`, `host`, `workload` and
`out` variables for the new checkout, diagnostic host, workload and empty output
directory; it takes round index 0, 1 or 2. To audit without rerunning the JIT:

```sh
mkdir /tmp/restored-runtime-cpu-audit
tar -xzf internals/measurements/2026-10-27-restored-runtime-cpu/evidence.tar.gz \
  -C /tmp/restored-runtime-cpu-audit
node internals/measurements/2026-10-27-restored-runtime-cpu/verify.mjs \
  /tmp/restored-runtime-cpu-audit
```

Expected result:

```text
restored runtime CPU evidence PASS profiles=18 files=97
```
