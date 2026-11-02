# Blackfin/bcore vs ARM/dynarmic.js: fixed C workloads

> 归档说明（2026-11-02）：bcore 已退出正式 examples 和 Pages。本文描述历史实验；旧 `examples/...` 命令需先按[独立恢复说明](../../experiments/bcore-comparison/README.md)还原实验快照。测量 JSON、原始压缩包和数值保持不变。

Five independent Chromium processes completed the ordinary Pages **Quick**
comparison, selecting ARM / Blackfin / direct Wasm. All 225 observations and
1,350 checksums passed; 675 measured calls are nested samples, not 675
independent browser repetitions. No failed sessions were discarded or retried.

For the four integer/memory cases, the paired equal-case geometric mean of
bcore time / ARM time was **1.686×**, with session values spanning
**1.583–1.853×**. Integer mix was close (1.208×), branches 2.282×, sequential
memory 2.095× and pointer chasing 1.500×. These are complete execution paths
with different compilers and ISAs.

Float64 was **364.23×** (253.03–395.14× across session summaries). Blackfin has
no hardware binary64: GCC calls libgcc arithmetic helpers executed as Blackfin
instructions. ARM uses hardware-style VFP guest instructions translated to Wasm,
and direct Wasm uses native Wasm f64 operations. Do not interpret this as a
364× general emulator gap. The five-case aggregate is 4.894×, but the separate
integer/memory aggregate is more informative about these non-floating workloads.

## Warm results

Milliseconds per identical call, lower is faster. Time columns are medians of
five session medians. Ratios pair the same round *before* taking session and
cross-session medians, so they need not equal a division of the time columns.
Ranges span five session ratio summaries; they are not confidence intervals.

| Case | Iterations/call | ARM/dynarmic | Blackfin/bcore | Direct Wasm | bcore / ARM | Session range |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Integer mix | 4,000,000 | 7.5 | 9.1 | 7.1 | 1.21× | 1.16–1.30× |
| Branches | 1,000,000 | 12.2 | 29.6 | 3.7 | 2.28× | 2.07–2.47× |
| Sequential memory | 8,000,000 | 16.0 | 33.4 | 4.9 | 2.10× | 1.85–2.14× |
| Pointer chasing | 4,000,000 | 8.1 | 12.0 | 6.7 | 1.50× | 1.36–1.57× |
| Float64 (software on Blackfin) | 4,000,000 | 6.4 | 2381.7 | 5.1 | 364.23× | 253.03–395.14× |

## First calls and preparation

These remain separate from warm timing. Preparation includes asset loading,
host Wasm compilation/initialization and guest input preparation as implemented
by each adapter. A first call includes compilation of guest code reached inside
that timed interval; guest initialization before the first marker is excluded.

| Case | ARM first | bcore first | Wasm first | ARM prepare | bcore prepare | Wasm prepare |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Integer mix | 11.7 ms | 240.3 ms | 10.5 ms | 41.1 ms | 293.2 ms | 8.9 ms |
| Branches | 22.0 ms | 248.9 ms | 6.8 ms | 46.7 ms | 292.4 ms | 9.4 ms |
| Sequential memory | 24.6 ms | 267.1 ms | 12.9 ms | 45.8 ms | 285.8 ms | 12.7 ms |
| Pointer chasing | 12.3 ms | 155.9 ms | 8.0 ms | 46.8 ms | 298.4 ms | 8.0 ms |
| Float64 (software on Blackfin) | 12.5 ms | 3629.6 ms | 9.1 ms | 41.1 ms | 313.0 ms | 14.6 ms |

## Configuration and method

- Intel Core i7-12700, Linux 6.8.0-138, Node 24.19.0, Chrome for Testing
  149.0.7827.55. Collector/browser affinity: CPUs `8,10,12,14`; no exclusive
  CPU reservation or frequency lock. Other machine workloads were not stopped;
  raw sessions retain load averages. Results describe this run, not a universal
  ranking. Two warmups do not establish steady-state optimization.
- Same unmodified `kernels.c`, seed `0x12345678`, Quick counts and independently
  computed reference checksums. Each of three rounds uses one fresh Worker per
  case/path: first call, two warmups, three measured calls. All six are checked.
  Runtime and case order rotate; paths execute serially. The tab stays visible.
- ARM and direct Wasm: Zig 0.17.0 / Clang 22.1.8, O2 scalar settings, unchanged
  kernel artifacts from the existing C lab. ARM host is a Release build without
  profiling or region diagnostics. This is a new paired run, not ratios against
  the historical Extended-profile v86 measurements.
- Blackfin: ADI GCC 4.5.3 (`2014R1_45-RC2`), `-O2 -mcpu=bf532`, no libc,
  vectorization/unrolling/fast-math disabled. GCC 4.5 cannot accept Clang's
  `-ffp-contract=off`; generated binary64 work calls libgcc and cannot use FMA.
  The published toolchain archive SHA-256 is
  `5ef1a625e4dd127c0eda00b0b0be983a298737281864c83363305ccdd60d3c4c`.
  Its gcc, cc1 and libgcc hashes match the locally used installation.
- bcore `df7aee68589424364e74d6cee4def5269721e3e6`: LLVM IR O2 / codegen O2,
  chaining, lazy state, loop blocks and bounded inline RAM. Copy-and-patch,
  promotion, verification and packet-count statistics are disabled. This is
  standalone bcore's optimizing LLVM path, **not OP-1 firmware or its default
  copy-and-patch web configuration**.
- bcore's host uses Emscripten 4.0.9 / Wasm LLVM 23.1.2, Release O3 compile,
  upstream-recommended O1 link. The ARM host uses Emscripten 6.0.13. Each guest
  has 16 MiB RAM; bcore's total Wasm memory starts at 128 MiB and includes LLVM,
  versus 64 MiB for the ARM host and 16 MiB for direct Wasm. These are capacities,
  not peak process memory measurements. Generated modules have additional cost.
- Real monotonic host timestamps bracket Blackfin's guest MMIO markers;
  no simulated cycles or host arithmetic substitutions are timed. Every timing
  is above the 0.1 ms floor. Sources and executable bytes are checked before
  execution, and the collector checks frozen distribution files before and
  after each session. No builds or other benchmark runs were started by this
  task during collection.

Quick was selected in advance after correctness smoke testing showed that
software float64 dominates run time. Extended remains unchanged and was also
verified: all six Blackfin calls passed for all five Extended workloads. The
new page passed corrupt-Wasm rejection, retry, Stop, visibility invalidation
and 320px/desktop layout checks. The existing ARM/v86/direct Quick and Extended
page checks passed as well. Unit checks: seven C lab tests and 23 source-package
Python tests passed.

## Evidence and reproduction

- [summary.json](summary.json): session medians/ranges, first/preparation times,
  paired ratios and separate integer/memory aggregate.
- [samples.csv](samples.csv): all 225 observations with measured and warmup calls.
- [raw-reports.tar.gz](raw-reports.tar.gz): five complete browser reports,
  frozen build/toolchain manifests and hashes, completion record, the collector
  sources used, correctness reports, compiler archive cross-check and Blackfin
  disassembly showing the libgcc calls. The setup script was exercised with a
  separately cloned upstream and freshly unpacked, checksum-verified GCC; the
  guest binary reproduced byte-for-byte. The unchanged host build cache was
  reused; all five distributed artifacts matched the measured distribution.
- [analyze.py](analyze.py): independently recomputes the summaries from the raw
  reports and verifies their inputs/checksums.

Build using the [C lab instructions](../../experiments/bcore-comparison/README.md),
then collect without concurrent builds:

```sh
taskset -c 8,10,12,14 node examples/c-bench/measure.mjs \
  --browser /path/to/chrome149 --dist build-em/examples/browser/dist \
  --comparison bcore --profile quick --sessions 5 --output NEW_RESULTS
python3 internals/measurements/2026-10-31-bcore-c-bench-performance/analyze.py \
  NEW_RESULTS NEW_SUMMARY
```

To audit the retained run, extract `raw-reports.tar.gz` and point the analyzer
at its `performance/` directory. The toolchain/build instructions are wired
into Pages CI; this change was validated locally and has not been deployed.
