# OP-1 webpage backend defaults: paired C benchmark comparison

> 归档说明（2026-11-02）：bcore 已退出正式 examples 和 Pages。本文描述历史实验；旧 `examples/...` 命令需先按[独立恢复说明](../../experiments/bcore-comparison/README.md)还原实验快照。测量 JSON、原始压缩包和数值保持不变。

This run replaces the earlier LLVM-only configuration as the page default.
It uses **copy-and-patch followed by background LLVM promotion**, matching the
actual OP-1 webpage Worker. C++ `op1_start()` initially disables promotion,
but `web/worker.js` then sets threshold **100000**, size reference **16**, and
attaches a compiler Worker. Comparing only the C++ startup values misses the
actual default. The [configuration audit](../../experiments/bcore-comparison/OP1-CONFIG.md)
records the settings and the intentional bare-metal differences.

Five independent Chrome 149 processes completed the ordinary Quick page run:
**225 observations, 1,350 matching checksums, no failed or retried sessions**.
There are 675 measured inner calls, not 675 independent browser repetitions.

## Timings and compilation stage

Under this fixed-call protocol, the four integer/memory cases' paired equal-case
geometric mean favors dynarmic.js by **5.782×** (session range **5.516–6.050×**).
The five-case aggregate is 13.026×, but that mixes hardware-style ARM floating
point with Blackfin's libgcc software arithmetic. Float64 alone is 404.84×;
it is not a general emulator speed ratio.

Milliseconds per fixed workload, lower is faster. Time columns are medians of
five session medians. Ratios pair round medians before session/cross-session
aggregation, so they need not equal a division of the displayed time columns.
Ranges span five session summaries, not confidence intervals.

| Case | Iterations/call | ARM/dynarmic | Blackfin/bcore | Direct Wasm | bcore / ARM | Session range |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Integer mix | 4,000,000 | 7.1 | 48.6 | 6.8 | 6.73× | 6.64–7.15× |
| Branches | 1,000,000 | 11.9 | 51.5 | 3.6 | 4.33× | 3.34–4.62× |
| Sequential memory | 8,000,000 | 14.7 | 113.3 | 4.6 | 7.36× | 6.97–7.74× |
| Pointer chasing | 4,000,000 | 7.3 | 39.2 | 6.0 | 5.37× | 5.14–5.49× |
| Float64 (Blackfin software) | 4,000,000 | 5.9 | 2346.2 | 4.7 | 404.84× | 308.29–412.25× |

**Two warmup calls are not enough to guarantee completion of asynchronous
promotion.** The benchmark preserves the existing first + two warmups + three
measured calls; it does not wait for compilation or insert extra warmups. It
therefore measures this default startup/tiering process, not long-running
firmware steady state or a guaranteed tier-1-only kernel.

Each case has 15 Blackfin observations (five sessions × three fresh engines):

| Case | Observations with a promotion installed by completion | With a promotion before measured calls | Jobs submitted / installed per observation |
| --- | ---: | ---: | --- |
| Integer mix | 0 / 15 | 0 / 15 | 1 / 0–0 |
| Branches | 0 / 15 | 0 / 15 | 6 / 0–0 |
| Sequential memory | 14 / 15 | 0 / 15 | 1 / 0–1 |
| Pointer chasing | 0 / 15 | 0 / 15 | 1 / 0–0 |
| Float64 (Blackfin software) | 15 / 15 | 15 / 15 | 36 / 36–36 |

The integer, branch and pointer cases stayed at copy-and-patch throughout their
six calls. Streaming often changed tier during the measured phase. Float64 had
all 36 promotions installed before measurement in every observation. There
were no refused/failed jobs and no LLVM fallback loads on the execution Worker
in the measured cases. The compiler Worker did load LLVM and install its
modules. A separate diagnostic observed **11 copy-and-patch modules** for the
integer case, verifying that the page-table configuration actually activates
copy-and-patch rather than silently selecting LLVM.

## First-call and loading costs

| Case | ARM first | bcore first | Wasm first | ARM prepare | bcore prepare | Wasm prepare |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Integer mix | 11.0 ms | 52.8 ms | 10.7 ms | 33.3 ms | 252.9 ms | 8.3 ms |
| Branches | 20.7 ms | 57.4 ms | 6.3 ms | 44.4 ms | 305.6 ms | 8.1 ms |
| Sequential memory | 19.5 ms | 122.4 ms | 10.8 ms | 36.4 ms | 251.5 ms | 11.2 ms |
| Pointer chasing | 12.6 ms | 42.3 ms | 6.4 ms | 40.9 ms | 241.6 ms | 9.6 ms |
| Float64 (Blackfin software) | 11.6 ms | 3302.1 ms | 9.0 ms | 61.7 ms | 320.3 ms | 9.2 ms |

Preparation covers the execution engine and inputs, not waiting for background
LLVM readiness. As in the webpage, the compiler Worker starts asynchronously.
Measured intervals use host monotonic markers and include event-loop yielding
and promotions that arrive inside the call. Guest input resets stay outside.
Loading costs here are not a comparison of OP-1's complete download UX.

## Configuration and scope

- Intel i7-12700, Linux 6.8.0-138, Node 24.19.0, Chrome for Testing
  149.0.7827.55; collector/browser affinity `8,10,12,14`. CPUs were not reserved
  exclusively or frequency-locked; other machine workloads were not stopped.
  Raw reports retain load averages. No builds or other benchmark runs from
  this task ran during collection.
- Same C source, Quick counts and seed `0x12345678`; three balanced-order
  rounds, a fresh execution/compiler Worker pair per Blackfin observation.
  The same independently computed checksums validate all six calls.
- ARM and direct Wasm: Zig 0.17.0 / Clang 22.1.8 O2 scalar settings. Blackfin:
  ADI GCC 4.5.3 (`2014R1_45-RC2`), `-O2 -mcpu=bf532`, scalar/no-fast-math/no-libc,
  libgcc binary64. Compiler/ISA differences remain part of the measured paths.
- bcore `df7aee68589424364e74d6cee4def5269721e3e6`; defaults audited against
  op1emu `9ba8d234b561efc77f1894f142b39efbd1f7a2db` including `web/worker.js`:
  copy-and-patch hints and split stencils; cold 10, batch 32, age 65536;
  remote tier threshold 100000 / size 16; IR O1 / codegen O2, light tier off;
  page-table RAM, chaining, lazy state, loop blocks, host cycles and IDLE waits;
  `SYSCFG=0x30`; chain cap `2^24`; 8 ms slices and clock checks every 32 runs
  or 100000 packets. Profiling, chained-block diagnostic counts, LLVM verification and
  steady-loop skipping are disabled. Compiler functions are warmed before
  delivery, as in OP-1's compiler Worker.
- Emscripten 4.0.9 / Wasm LLVM 23.1.2, Release O3 and link O1, with LLVM split
  out and initialized on demand. Main module 42.46 MiB, deferred LLVM 27.73 MiB
  uncompressed. Each Worker starts with 128 MiB memory; execution and compiler
  instances are separate. Capacities are not peak process memory. The ARM host
  uses Emscripten 6.0.13 and 64 MiB total Wasm memory; direct Wasm uses 16 MiB.
- Deliberate experiment differences: 16 MiB guest RAM shared across paths,
  bare-metal C instead of OP-1 firmware, no NAND/devices/audio pacing or device
  deadlines; no OP-1 Brotli/Service Worker cache. A compiler failure invalidates
  the observation instead of silently continuing to score tier 0. See the
  audit for exact source locations and remaining differences.

## Validation and evidence

All Quick page rounds and all six Blackfin Extended calls for all five cases
passed. Checks also cover generated copy-and-patch modules, actual background
installations, corrupted primary/deferred Wasm rejection, retry, Stop, hidden-tab
invalidation and desktop/320px layouts. A separate unsupported Blackfin packet
triggered exactly one on-demand LLVM load on the execution Worker, exercising
the synchronous verified fallback (the synthetic guest intentionally omits
benchmark markers, so its missing-marker exit status is expected). Eight Node tests and 23 source-package
Python tests passed.

- [summary.json](summary.json): session medians/ranges, paired ratios, separate
  integer/memory aggregate, first/preparation times and compilation-stage counts.
- [samples.csv](samples.csv): all 225 observations, measured calls and warmups.
- [raw-reports.tar.gz](raw-reports.tar.gz): five complete reports, build/toolchain
  manifests and stencil/artifact identities, verification reports, collector
  sources, the exact page/runtime adapters as measured and host/build inputs.
  A later whitespace-only indentation fix to the page Worker is not part of
  the frozen measured snapshot.
- [analyze.py](analyze.py): independent analysis and input/checksum/stage checks.

The [earlier LLVM-only result](../2026-10-31-bcore-c-bench-performance/README.md)
is retained as a separate non-default experiment. Do not use it as the webpage
configuration's score or divide these timings by its historical timings.

Build using the [C lab guide](../../experiments/bcore-comparison/README.md), then:

```sh
taskset -c 8,10,12,14 node examples/c-bench/measure.mjs \
  --browser /path/to/chrome149 --dist build-em/examples/browser/dist \
  --comparison bcore --profile quick --sessions 5 --output NEW_RESULTS
python3 internals/measurements/2026-10-31-bcore-web-defaults-performance/analyze.py \
  NEW_RESULTS NEW_SUMMARY
```

To audit the retained run, extract the raw archive and use its `performance/`
directory as input to the analyzer. Pages CI builds/verifies these artifacts;
this change was tested locally and has not been deployed.
