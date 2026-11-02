# Run-scoped application CPU profiles — 2026-10-24

Base implementation: `f95d6c9`. This milestone adds diagnostic tooling and
measurements; it does not change backend lowering, region selection or ordinary
production-generated code. The checkout during collection contained the new
profiler and its tests. The named diagnostic host was built separately with
`DYNARMIC_JS_PROFILE=OFF` and `--profiling-funcs`; its hashes are retained in
`host-sha256.txt`. These observations do not independently attest source
provenance.

Node v24.19.0 / V8 13.6.233.17-node.51, Emscripten 6.0.13, Linux x86_64,
Core i7-12700. Collections were sequential and frequency was not locked.
Sampling requested 100 microseconds; every six-mode profile had a 153 μs median
and 161 μs p95 sample delta. Instrumented Run values below are diagnostics, not
benchmark scores or paired comparisons.

## Qualified baselines

The archive retains the fixed 12-case five-process report and the complete
CoreMark six-case, three-fresh-process report. Both pass their schemas; CoreMark
passes the native CRC oracle, 120,000-iteration and duration qualification.
Baseline collection used affinity `0,2,4,6`. Warm fixed observations still
contain tiering and are not certified steady-state throughput.

CoreMark kernel medians:

| Architecture | Thunk | Page-table | Flat-window |
| --- | ---: | ---: | ---: |
| A32 | 39.058 s | 18.083 s | 14.158 s |
| A64 | 35.234 s | 18.072 s | 13.349 s |

The workload is CoreMark revision
`1f483d5b8316753a742cbf5590caf5bd0a4e4777`, 120,000 iterations. These are the
reference measurements for a future compatible paired experiment, not evidence
of engine leadership.

## Six-mode Run profiles

Each raw profile wraps exactly one `_arm_run(0x10000)` after loading and
initialization. Debug names were enabled before `_arm_init`; the ordinary
sampler does not open an inspector session. Every application Run completed
with valid stage and final CRCs.

| Profile | Instrumented Run | Guest subtree / all samples |
| --- | ---: | ---: |
| A32 thunk | 39.984 s | 90.87% |
| A32 page-table | 18.554 s | 99.51% |
| A32 flat-window | 14.449 s | 99.32% |
| A64 thunk | 36.104 s | 92.47% |
| A64 page-table | 18.550 s | 99.26% |
| A64 flat-window | 13.590 s | 98.95% |

Shares are unweighted sample observations, not exact timings or speedup bounds.
The guest-root selection can include memory callbacks and other descendants;
all-sample summaries are retained alongside guest summaries to avoid hiding
those costs.

Thunk profiles contain substantial `MemoryRead*`/`MemoryWrite*` and
`dynarmic_wasm_*` samples. Page-table and flat-window profiles are almost wholly
inside generated guest regions. Page-table also changes the hot distribution:
A32 `core_bench_list` rises from 23.24% of flat guest samples to 31.34%, and the
two A64 `iterate` entries rise from a combined 26.54% to 33.98%. Treat these as
separate memory-path hypotheses, not one generic optimization target.

The five hottest A64 flat regions are:

| Generated function | CoreMark symbol | Guest self samples |
| --- | --- | ---: |
| `a64_11700_region` | `matrix_test` | 17.04% |
| `a64_1209c_region` | `core_bench_state` | 16.76% |
| `a64_10b00_region` | `iterate` | 13.96% |
| `a64_10b18_region` | `iterate` | 12.59% |
| `a64_1202c_region` | `core_bench_state` | 10.53% |

A separate synchronous-tiering diagnostic run used:

```sh
node --trace-wasm-compilation-times --no-wasm-async-compilation \
  --wasm-sync-tier-up tests/bench/coremark/sample.mjs \
  /tmp/dynarmic-named-runtime/examples/browser/dist/arm_jit.js \
  /tmp/dynarmic-coremark-120k/coremark_a64.bin a64 2 \
  > /tmp/a64-flat-tiering.log 2>&1
```

Its generated code-section body sizes match TurboFan records for all five hot
regions. The two 21,695-byte `iterate` modules are indistinguishable by size,
but two corresponding TurboFan records exist. Thus the profiles do not support
blaming persistent Liftoff execution for these hotspots. The synchronous flags
change compilation behavior and are diagnostic only.

Comparable WAT for blocks 27, 51 and 53 is retained for all three memory modes.
Flat memory uses an explicit range check before each direct load, with an MMIO
fallback; page-table adds high-address validation, a page-base load and offset
composition; thunk replaces direct accesses with imported memory calls. These
checks cannot simply be deleted without preserving fallback semantics. Static
store counts include cold exits and are not dynamic cost measurements.

The `iterate` regions contain 64 blocks, the current region maximum, and retain
cross-module calls with large guest-state flushes. A separate profile-enabled
A64 flat Run reports 7,397 region candidates and 328 block-budget rejections,
versus zero IR-budget rejections. It also reports 4,166 selected region blocks
across 100 regions for 111 compiled guest blocks. This makes bounded hot-call
absorption a credible next experiment, but not yet an accepted optimization:
larger regions duplicate code and may increase engine compilation latency.
The first candidate must retain compile-size/time and complete-duration gates.

## Tooling and verification

`tests/bench/scoped_cpu_profile.mjs` provides exclusive-output, non-reentrant
V8 capture for sync/async callbacks. It stops and retains profiles after callback
failure, preserves falsy exceptions, disconnects reliably and composes callback
and cleanup errors. `tests/bench/coremark/profile.mjs` reuses the ordinary
CoreMark collector and validates guest faults inside the capture so a cleanup
failure cannot mask the guest error.

Final verification:

- `node --test tests/bench/*.test.mjs`: **188/188 passed**.
- `ctest --test-dir build-native --output-on-failure`: **4/4 passed**.
- Focused profiler integration tests: **10/10 passed**.
- Ordinary sampler refactor: real six-mode short CoreMark smoke passed all CRCs;
  it was not duration-qualified.
- Direct helper coverage: **100% lines, branches and functions**.
- Independent implementation review and re-review found no remaining
  CRITICAL/HIGH/MEDIUM issue.

The full strict regression passed at clean `f95d6c9` before this tooling-only
milestone; it was not rerun after the final test-only error-path additions.
Release, browser-performance, bounded-cache, API and packaging gates remain
open. No runtime speedup is claimed.

## Reproduce and audit

Use the [Run-profile recipe](../../../docs/Performance.md) with fresh output paths.
To audit the retained evidence without rerunning the JIT:

```sh
mkdir /tmp/runtime-cpu-audit
tar -xzf internals/measurements/2026-10-24-runtime-cpu/evidence.tar.gz \
  -C /tmp/runtime-cpu-audit
node internals/measurements/2026-10-24-runtime-cpu/verify.mjs /tmp/runtime-cpu-audit
```

The verifier recomputes every all-sample and guest summary, binds raw input
hashes, checks all six CoreMark outputs, validates both baseline protocols,
checks TurboFan/body-size evidence and profile counters, and verifies the
per-file SHA256 inventory. Expected result:

```text
runtime CPU evidence PASS profiles=6 files=41
```
