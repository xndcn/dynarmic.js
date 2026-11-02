# OP-1 webpage configuration audit

Reference: [op1emu `9ba8d23`](https://github.com/op1emu/op1emu/tree/9ba8d234b561efc77f1894f142b39efbd1f7a2db),
using bcore `df7aee68589424364e74d6cee4def5269721e3e6`.
The comparison follows the webpage's **effective** configuration, including
JavaScript calls after C++ startup. `op1_start()` alone sets `UINT32_MAX`, but
`worker.js` immediately attaches a compiler Worker and changes it to `100000/16`.
Pure copy-and-patch without promotion is not the webpage default.

| Setting | OP-1 webpage | C benchmark |
| --- | --- | --- |
| Tier 0 | Copy-and-patch; hints and split stencils enabled | Same |
| Cold tier | 10 starts; batch 32 blocks; age 65,536 packets | Same |
| Promotion | Threshold 100,000; size reference 16 | Same |
| Compilation | Separate Worker; each returned Wasm function warmed before delivery | Same |
| Tier 1 | LLVM IR O1, codegen O2; light pipeline off | Same |
| Missing-stencil fallback | LLVM tier 0 (IR O0 / codegen O0); no optimized promotion on execution thread | Same |
| LLVM distribution | Deferred split Wasm; LLVM initialized on demand | Same |
| RAM access | Page table; no rawmem fast path; callbacks preserve registers | Same |
| Chaining | Enabled for NAND boot; disabled for LDR mode | Enabled for the C workload |
| Chaining cap | `2^24` packets, shortened by device/event deadlines | Same cap; no device deadlines |
| Chain filter | PC `0xFFA00000` returns to host | Same filter; benchmark never reaches this PC |
| Slice | 8 ms; clock check after 32 runs or 100,000 packets | Same; MessageChannel yields |
| Cycle accounting | Retired packets; host updates CYCLES before dispatch | Same |
| IDLE handling | Wait enabled; SIC supplies wake flag | Enabled; no guest IDLE/device wake events |
| Initial SYSCFG | `0x30` | Same |
| Lazy state / loop blocks | Enabled | Same |
| Profiling / chained-block counts / verification / steady skipping | Disabled in release page | Same |
| Emscripten / LLVM | 4.0.9 / Wasm LLVM 23.1.2 | Same |
| Host build | Release O3; link O1; Wasm exceptions; no pthreads | Same |
| Memory/table growth | 128 MiB initial, 4 GiB maximum, 8 MiB stack; table may grow | Same per Worker |

## Deliberate workload and transport differences

These prevent a claim that this is the full OP-1 application, while keeping
the same C inputs and work counts as the ARM/direct-Wasm comparison.

| Area | OP-1 webpage | C benchmark / implication |
| --- | --- | --- |
| Guest | BF526 boot ROM and firmware, NAND/OTP and devices | bfin GCC O2 BF532 scalar C, no firmware; this is a backend workload comparison |
| Guest RAM/map | 128 MiB SDRAM plus L1, ROM, devices; SP/FP `0x07000000` | 16 MiB RAM on all three paths; entry `0x10000`, stack `0xff0000`, two marker/exit MMIO addresses |
| Device work | Timers, interrupts, display, sensors and audio alter dispatch deadlines | No devices; the host uses the chain cap without fabricated device interruptions |
| Pacing | Audio Worklet at 44.1 kHz, default 100 ms queue; guest-time/device pacing | Unpaced throughput; no artificial sleep to match audio rate |
| Downloads/cache | Brotli stream fallback, Service Worker Wasm/code cache, hashed URLs | SHA-256-verified bytes and relative URLs; same split LLVM structure, different loading UX |
| JavaScript wrapper | ES module; Worker/Node; filesystem/device exports | Classic Worker wrapper; only lab/runtime exports; Node is used to profile the module split |
| Engine lifetime | One persistent firmware session | Fresh engine and compiler Worker per case/round; first + 2 warmups + 3 measured calls |
| Failed background compiler | Can continue at tier 0 | Fails the observation, to avoid presenting a fallback as a valid default-path measurement |

The guest clock never supplies the elapsed time reported by the benchmark.
Host monotonic markers include event-loop yielding and any promotion occurring
inside a call. Loading/initialization remain separate. Two warmups do not
guarantee that a short case reaches tier 1: exports include submitted/installed
jobs and installations before the measured phase. The default-path comparison
does not force completion of background jobs or add extra warmups.

## Source locations

- [Effective Worker defaults and scheduling](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/web/worker.js): `SLICE_MS`, `TIER_THRESHOLD`, `TIER_SIZE_REF`, `tierWorker()`, `run()`.
- [C++ startup and clock checks](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/web/src/op1_web.cpp): cold-tier constants, `op1_start()`, `op1_step()`.
- [CPU setup and dispatch](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/src/cpu/cpu.cpp): core setters, `SetBfinCycles()`, `Run()`, `ChainBudget()`.
- [Chain cap](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/src/cpu/cpu.h): `kMaxChainPackets`.
- [Compiler Worker](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/web/compile-worker.js) and [delivery](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/web/compile-helpers.js).
- [Build and split-module options](https://github.com/op1emu/op1emu/blob/9ba8d234b561efc77f1894f142b39efbd1f7a2db/web/CMakeLists.txt).

The lab manifest records the defaults, pinned upstream revisions, generated
stencil identities and every distributed artifact. The page rejects manifests
for the earlier LLVM-only or promotion-disabled hosts. A separate diagnostic
test observes copy-and-patch Wasm exports; normal runs retain background-tier
counters without enabling bcore's profiler.
