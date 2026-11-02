# Tuning the ARM QuickJS guest

Investigation completed **2026-10-31**. Scope: the pinned QuickJS 2025-09-13
interpreter compiled to ARM32 and executed by dynarmic.js. All **196 process
observations passed** across screening, follow-up and confirmation. The default
remains **O2 / ARM / switch / no LTO / no passes disabled**: the smaller Thumb
candidate did not establish a consistent throughput improvement. Compiler
hypotheses, static findings and measured results are documented separately below.

## Fixed baseline and source controls

The baseline uses `arm-none-eabi-gcc 10.3.1 20210621`, `-O2`, ARM state,
`-march=armv7-a -mfpu=vfpv3 -mfloat-abi=softfp`, switch dispatch and no LTO.
Preserve `-fwrapv`, `-fno-strict-aliasing`, source version, memory/stack policy,
allocator, UTC adaptation and the benchmark inputs across candidates.

The existing embedding defines `EMSCRIPTEN` for **both** C targets. In the
pinned `quickjs.c` this selects switch dispatch, disables OS-dependent atomics
and `CONFIG_STACK_CHECK`, and changes the default usable-allocation-size path.
Removing that macro to enable computed goto would therefore change several
properties at once. Keep it and change only the exact `DIRECT_DISPATCH` block
in a separately identified ARM source copy. The direct Wasm target must
continue compiling its original switch-dispatch source. Record the generated
source hash and transformation in the manifest.

The current [build script](../examples/quickjs/build.py) exposes:

```text
--arm-opt O2|O3|Os       default O2
--arm-dispatch switch|threaded
                         default switch; threaded means computed goto
--arm-lto                default off
--arm-isa arm|thumb      default arm
--arm-no-gcse            default off; adds only -fno-gcse
--arm-no-crossjumping    default off; adds only -fno-crossjumping
```

The [browser CMake configuration](../examples/browser/CMakeLists.txt) exposes
all six choices, including the two pass switches. Use these cache settings
with the existing Emscripten browser build configuration:

| CMake cache setting | Accepted values | Default | ARM manifest field |
| --- | --- | --- | --- |
| `DYNARMIC_QUICKJS_ARM_OPT` | `O2`, `O3`, `Os` | `O2` | `optimization` |
| `DYNARMIC_QUICKJS_ARM_DISPATCH` | `switch`, `threaded` | `switch` | `dispatch` |
| `DYNARMIC_QUICKJS_ARM_ISA` | `arm`, `thumb` | `arm` | `isa` |
| `DYNARMIC_QUICKJS_ARM_LTO` | `ON`, `OFF` | `OFF` | `lto` |
| `DYNARMIC_QUICKJS_ARM_NO_GCSE` | `ON`, `OFF` | `OFF` | `noGcse` |
| `DYNARMIC_QUICKJS_ARM_NO_CROSSJUMPING` | `ON`, `OFF` | `OFF` | `noCrossjumping` |

For example, the confirmed Thumb candidate uses the defaults above with
`-DDYNARMIC_QUICKJS_ARM_ISA=thumb`. CMake records the selected builder arguments
in a generated dependency file so changing a cache setting rebuilds the guest.
The [source packager](../examples/browser/package_source.py) validates all six
recorded `targets.arm` fields and maps them back to explicit CMake settings in
the rebuild instructions; boolean values become `ON` or `OFF`. Thus packaged
sources preserve the selected configuration rather than silently rebuilding
with defaults.

These options affect only the ARM guest. The two pass-disabling options are
narrow, recorded experiments; neither disables all related merging passes.
The unmodified Wasm source, effective flags and output hashes should remain
identical during the ARM matrix. Use a stable source/cache path when comparing
binary hashes: embedded `__FILE__` diagnostics can otherwise make equivalent
builds differ merely because their directories differ. Serialize operations
sharing a build cache and copy completed, identified artifacts to variant
directories before starting the next build.

## Evidence behind the candidates

The following official GCC documentation was retrieved over HTTPS during this
review. References use GCC 10.3, matching the compiler family in the workspace.

| Candidate | Compiler evidence and dynarmic-specific hypothesis | Constraint |
| --- | --- | --- |
| `-O2` / `-O3` / `-Os` | [Optimization options](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Optimize-Options.html) describe O3's extra loop, vectorization, cloning and path transforms; Os prioritizes code size. Upstream QuickJS's own Makefile uses O2 and supports optional LTO. Smaller or differently arranged guest blocks can reduce translation/emission work, while additional optimization can reduce executed guest work. | Neither larger optimization levels nor smaller binaries guarantee better performance through a JIT. Report warm execution and compilation separately. Keep FP semantics unchanged. |
| Computed goto | [Labels as Values](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Labels-as-Values.html) documents label addresses, dispatch tables and threaded interpreters. QuickJS already contains a 256-entry table and `goto *dispatch_table[...]`; no bytecode-handler rewrite is required. | Hardware indirect-branch-prediction benefits from native interpreter studies do not transfer directly to dynarmic's generated Wasm. The relevant effect is the actual ARM control flow and resulting dynamic dispatch/cache behavior. |
| `-fno-gcse` with computed goto | GCC's [GCSE option](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Optimize-Options.html#index-fgcse) explicitly says programs using computed gotos “may get better run-time performance” by adding `-fno-gcse`. | This is a candidate, not a promise. Installed GCC confirms GCSE is enabled at O2/Os; O3 also enables `-fgcse-after-reload`, which remains enabled with `-fno-gcse`. Record effective options instead of assuming every related pass is off. |
| `-fno-crossjumping` | GCC [documents cross-jumping](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Optimize-Options.html#index-fcrossjumping) as merging equivalent code to save size, and states that the result “may or may not perform better”. Keeping dispatch tails separate could alter the guest's indirect-branch sites and JIT regions. | This dispatch-site explanation is a hypothesis; GCC does not promise a computed-goto speedup. More distinct tails can increase generated code, translation cost and cache pressure. Check disassembly. |
| `-flto` | GCC [LTO documentation](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Optimize-Options.html#index-flto) requires compatible optimization/LTO options at compilation and final link. It exposes function bodies across translation units. | Preserve the ARM startup, C ABI, externally used functions and linker script. A static label-address initializer prevents GCC from inlining/cloning the containing function, per Labels as Values; QuickJS's threaded `JS_CallInternal` has such an initializer. LTO and dispatch can therefore interact. |
| `-mthumb` | [ARM options](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/ARM-Options.html) distinguish ARM and Thumb instruction generation and floating-point ABI. Thumb-2 can change instruction density and control-flow shape. | Keep ARMv7-A/VFPv3/softfp fixed. The explicit `.arm` startup remains ARM, with normal interworking to compiled C. Validate linker relocations and execution; the previously fixed Thumb ITSTATE bug is a reason to retain its regression test, not to exclude Thumb. |

Keep [the signed-overflow contract](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Code-Gen-Options.html#index-fwrapv)
intact. Do not use `-Ofast`/`-ffast-math`, remove required language features,
shrink benchmark inputs, or weaken assertions as performance tuning. A change
from softfp to hard-float is an ABI change requiring matching libraries and
startup conventions, not an isolated scheduling option. `-mtune=cortex-*`
models a physical ARM CPU; dynarmic executes emitted Wasm on the host, so such
native scheduling advice has lower priority than measured dispatch/code-size
changes. PGO is also deferred until a representative training corpus and a
held-out evaluation prevent tuning specifically to the ten published cases.

## Completed measurement stages

The actual first matrix covers **O2/O3/Os × switch/threaded × LTO off/on**,
all in ARM state: **12 variants**. It supersedes the initial proposal to test
six non-LTO builds before selectively adding LTO. This full three-axis matrix
can reveal optimization/dispatch/LTO interactions directly. Four targeted
follow-ups add O2/O3 switch Thumb, O2 threaded without GCSE, and Os threaded
without cross-jumping: **16 distinct configurations tested in total**.

| Stage | Variants | Purpose | Status |
| --- | --- | --- | --- |
| A: screen | 12 ARM variants × 5 tasks × 1 round | Test optimization level, dispatch and LTO against `o2-switch`. | **60/60 passed**, zero failures |
| B: follow-up | Four new variants plus `o2-switch`, `o3-switch`, `o3-threaded`; 7 × 5 tasks × 2 rounds | Evaluate Thumb and the two isolated pass switches alongside retained controls. | **70/70 passed**, zero failures; timing caveat below |
| C: confirmation | `o2-switch` and `o2-switch-thumb`; 2 × 11 tasks × 3 rounds | All ten V8 cases plus the full kernel trio, alternating order. | **66/66 passed**, zero failures; original default retained |

If results show interactions or no clear winner, expand the relevant cells
instead of assuming a sequential local optimum is globally best. For an O3
GCSE experiment, preserve and report `-fgcse-after-reload`; disable it only in
a separately named follow-up, not silently as part of `-fno-gcse`.

The recorded screening experiment in
`build-quickjs-arm-tuning/screen-results/experiment.json` runs Richards,
DeltaBlue, Crypto/Encrypt, Splay and the full LCG/sieve/pi trio for each of the
12 variants: **60 tasks in one round**, sequentially rotating variant order by
case. Each task is a fresh Node process. This is preliminary screening, not
full eight-suite coverage or repeated independent evidence. Its environment
records Node 24.19.0, Linux x64 and an Intel Core i7-12700; CPU affinity spans
all 20 logical processors rather than a single controlled core.
The follow-up and confirmation experiments use CPU affinity `8,10,12,14`.
The completed analyses and raw experiment/result archives are retained in
[the measurement evidence directory](../internals/measurements/2026-10-31-quickjs-arm-tuning/).
The original screen analysis is `screen-analysis-original.json` in the build
tree; the durable copy is `screen-analysis.json`. Follow-up results are in
`followup-analysis.json` in both locations.

Screening can reject broken or obviously poor variants. Confirmation ran
**all ten V8 v7 cases on the ARM path**, retaining the upstream one-second
warmup/measurement batches and minimum 32 invocations. Candidate/baseline order
alternated over three complete rounds, with each task in a fresh Node process.
The workstation was shared rather than exclusive, so CPU affinity did not
guarantee isolation from other load. One official result is an aggregate over
iterations, not a set of independent process samples or a confidence interval.
Do not infer universal steady state merely from the one-second warmup.

## Independent static build review

The twelve saved ELFs under `build-quickjs-arm-tuning/objects/` were inspected
with `arm-none-eabi-nm` and `arm-none-eabi-objdump`, without compiling or running
another benchmark. Converting each ELF to a temporary flat image reproduced
its corresponding matrix binary **byte for byte**, so these are the objects
used by the collector rather than a later shared-cache build. Independent
hashing confirmed identical basic/benchmark dynarmic hosts, direct QuickJS
Wasm JS/Wasm, official source/adapter/inventory and kernel source across all
twelve variant directories.
The reviewed `build-matrix.json` SHA-256 was
`18bf6f016c73e587a1435039035ec164d88ffa28f410c02ae51bab95b3efb958`.

| Variant | ARM binary bytes | `JS_CallInternal` bytes | Register-to-PC dispatch sites |
| --- | ---: | ---: | ---: |
| O2 switch | 683,252 | 37,444 | 0 |
| O2 switch + LTO | 689,892 | 37,508 | 0 |
| O2 threaded | 685,748 | 38,948 | 298 |
| O2 threaded + LTO | 692,452 | 39,076 | 300 |
| O3 switch | 815,516 | 37,828 | 0 |
| O3 switch + LTO | 844,108 | 38,216 | 0 |
| O3 threaded | 818,588 | 39,452 | 266 |
| O3 threaded + LTO | 847,628 | 40,044 | 266 |
| Os switch | 524,980 | 26,536 | 0 |
| Os switch + LTO | 521,132 | 26,668 | 0 |
| Os threaded | 529,140 | 29,668 | 1 |
| Os threaded + LTO | 525,292 | 29,776 | 1 |

“Register-to-PC dispatch sites” counts static `mov pc, r3` instructions inside
the interpreter function, not executed branches or performance events. All
threaded builds contain a read-only **1024-byte `dispatch_table.78`** matching
the source's 256 ARM label pointers; all switch builds lack that object. Switch
builds instead contain the main range-checked PC-relative jump table and two
other internal switch tables. Threaded builds retain the two internal switch
tables as well.

For example, the O2 switch entry at `0x1a61c` loads an opcode, subtracts one,
compares against 246 and dispatches with `ldrls pc, [pc, r3, lsl #2]`. The O2
threaded entry loads the table base `0xa43e0`, reads an opcode-indexed pointer
with `ldr r3, [r6, r4, lsl #2]`, then executes `mov pc, r3` at `0x1a61c`.
Numerous opcode tails repeat that form. Thus computed goto is present in the
machine code and is not merely a manifest label.

Os is a useful caution: both threaded Os binaries have only **one** such
indirect jump site despite the threaded source. The optimized output converges
on a shared dispatch point. This supports investigating tail merging/GCSE if
the runtime results warrant it; it does not identify which compiler pass caused
the convergence or prove that separate sites are faster. O2/O3 threaded code
has more sites and a larger interpreter body, but only the dynarmic measurements
can determine the benefit or cost. No winner is selected from these static
counts or sizes.

Additional read-only GCC queries narrow the follow-up experiments. GCC 10.3
[documents](https://gcc.gnu.org/onlinedocs/gcc-10.3.0/gcc/Optimize-Options.html)
that it factors computed gotos early and later unfactors them subject to
`--param=max-goto-duplication-insns`. The installed compiler reports that
threshold as **8** at O2, O3 and Os. It reports `min-crossjump-insns` as **1 at
Os**, versus **5 at O2/O3**; all three also enable tree tail merging. Therefore
Os-threaded with `-fno-crossjumping` and O2-threaded with `-fno-gcse` were selected
as focused follow-ups. Their dispatch-site changes were checked before
interpreting timings; a combined pass-disabling variant was not measured. The
parameter values explain what to investigate, not which pass definitively
caused the observed single dispatch site. Keep any explicit goto-duplication
or tree-tail-merge experiment separate from those two switches.

The [follow-up static audit](../internals/measurements/2026-10-31-quickjs-arm-tuning/followup-static-review.json)
verifies all four new ELFs against their actual binaries and confirms unchanged
fixed controls. Both Thumb builds contain Thumb-2 interpreter code and Thumb
function-symbol bits; their ARM startup uses BLX at `0x10018` to enter
`guest_main`, preserving interworking.

| Follow-up | Binary bytes | Change versus matching original configuration | Dispatch finding |
| --- | ---: | ---: | --- |
| O2 switch Thumb | 512,644 | −170,608 | Thumb-2; interpreter body 28,676 bytes |
| O3 switch Thumb | 604,604 | −210,912 | Thumb-2; interpreter body 27,886 bytes |
| O2 threaded, `-fno-gcse` | 691,252 | +5,504 | Register-to-PC sites fall from 298 to 210; interpreter body grows by 504 bytes |
| Os threaded, `-fno-crossjumping` | 544,564 | +15,424 | Sites remain at 1; interpreter body grows by 368 bytes |

Thus disabling cross-jumping alone did not restore separate dispatch tails,
and disabling GCSE did not simply create more sites. The flags are confirmed
in each manifest's effective ARM options; these static changes are not speed
measurements.

The compiler's multilib selection was also queried with the exact ARMv7-A,
VFPv3 and softfp flags. In this GCC 10.3.1 installation, **both `-marm` and
`-mthumb` select `thumb/v7-a+fp/softfp`**, with identical resolved paths and
archive hashes for `libc.a`, `libm.a` and `libgcc.a`; see the
[multilib review](../internals/measurements/2026-10-31-quickjs-arm-tuning/multilib-review.json).
This establishes equal input
archives, not identical extracted members or final linked library code. The
Thumb comparison still covers the complete linked guest, including call
interworking and layout effects; other toolchains need their own multilib check.

## Measurements, fairness and publication

Keep these observations for each identified artifact:

- Exact source/archive hashes, transformation, GCC version, effective flags,
  linked libraries, ELF/binary hashes and byte size. Inspect `JS_CallInternal`
  disassembly to confirm switch/computed-goto and ARM/Thumb are actually different.
- Per-case raw counts, elapsed measurement batches and microseconds per
  invocation; whole-call and preparation times separately. Retain failed cases
  and errors rather than averaging only successful rows. The adaptive harness
  can choose different iteration counts, so whole-call time is not a fixed-work
  throughput comparison between configurations.
- dynarmic compile phases, emitted blocks and bytes, and memory capacity. These
  counters describe the complete guest call and must not be added to a timed
  interval that already contains them. Use production runtime builds for speed
  measurements; diagnostics/profiling are separate investigations.
- Browser/Node version, visibility changes, runtime order and independent
  repetitions. Do not mix Node and Chromium samples into one average. File
  download size, transfer compression and startup time are separate from warm
  execution throughput.

For the primary suite summary, within each process round compute each case's
paired baseline/candidate time ratio, combine Crypto's two cases and
EarleyBoyer's two cases geometrically, then take the geometric mean across the
eight suites. Report the median and range of those complete round ratios;
do not first combine each case's median across different rounds. Also show all
ten case ratios and any regressions. The ten-case equally weighted mean is a useful
secondary statistic, but it weights those two multi-case suites twice. Keep
direct QuickJS Wasm unchanged while isolating ARM improvements. If the final
page compares the tuned ARM binary against an unchanged O2 switch Wasm build,
publish that distinction; do not imply identical compiler configuration.

The prior local Chromium result (about 13.93× more time for ARM/dynarmic versus
direct Wasm with equal-suite weighting) is context for this investigation,
not proof that a new compiler setting closes the gap. Native compiler studies
and GCC recommendations select candidates; only measurements through this
project establish whether they improve it.

## Measured results

The 60-task screen and 70-task follow-up completed successfully. They establish
correctness for the selected workloads, not full-suite superiority. External
`ctest` load substantially affected the early part of follow-up **round 0**.
All observations remain retained; neither that round nor a two-round aggregate
is treated as a controlled performance verdict. Round 1 informed the choice of
confirmation candidate, but was not sufficient to declare a winner.

Ratios below are **baseline time / candidate time**; above 1 favors the
candidate. “Selected 7” combines the four V8 cases and three kernel timings
equally within one round; it is not the eight-suite summary. The V8 column is
the geometric mean of the four selected cases, not four complete suites
(Crypto/Decrypt was not part of screening).

| Follow-up configuration | Round 0 selected 7 | Round 1 selected V8 cases | Round 1 selected 7 |
| --- | ---: | ---: | ---: |
| O2 switch baseline | 1.000 | 1.000 | 1.000 |
| O3 switch | 0.927 | 1.000 | 0.984 |
| O3 threaded | 0.823 | 0.987 | 0.955 |
| O2 switch Thumb | 0.840 | **1.009** | **1.015** |
| O3 switch Thumb | 0.897 | 0.916 | 0.934 |
| O2 threaded, no GCSE | 0.939 | 0.951 | 0.948 |
| Os threaded, no cross-jumping | 0.777 | 0.808 | 0.761 |

O2 switch Thumb was selected for full confirmation because its guest binary
is 512,644 versus 683,252 bytes (**24.97% smaller**), with near-baseline selected
timings in follow-up round 1. That round still contained regressions: DeltaBlue's
ratio was 0.894, Encrypt 0.962 and LCG 0.960; Richards, Splay, sieve and pi were
1.127, 1.068, 1.037 and 1.078. The complete confirmation retained all ten cases
instead of selecting only the favorable subset.

The [completed confirmation analysis](../internals/measurements/2026-10-31-quickjs-arm-tuning/confirmation-analysis.md)
contains **66/66 passing tasks**: two variants × (ten official V8 cases + one
full kernel-trio task) × three independent process rounds. It ran on Node
24.19.0, Linux x64, Intel Core i7-12700, with CPU affinity `8,10,12,14` on a
shared workstation. These are Node measurements, not browser performance
validation.

Each row below summarizes the three **paired baseline-time / Thumb-time
ratios** for the official per-invocation metric. A ratio above 1 favors Thumb.
The ratios are computed within each round before taking their median and
range; dividing independently aggregated baseline and candidate medians would
lose the pairing and can give a different answer.

| Official case | Paired ratio median | Minimum | Maximum |
| --- | ---: | ---: | ---: |
| Richards/Richards | 0.952 | 0.944 | 0.958 |
| DeltaBlue/DeltaBlue | 0.977 | 0.652 | 1.004 |
| Crypto/Encrypt | 0.946 | 0.939 | 1.040 |
| Crypto/Decrypt | 0.949 | 0.896 | 0.952 |
| RayTrace/RayTrace | 0.997 | 0.996 | 1.181 |
| EarleyBoyer/Earley | 1.010 | 0.933 | 1.039 |
| EarleyBoyer/Boyer | 0.992 | 0.975 | 1.013 |
| RegExp/RegExp | 0.972 | 0.855 | 1.008 |
| Splay/Splay | 1.013 | 1.008 | 1.025 |
| NavierStokes/NavierStokes | 1.040 | 1.022 | 1.058 |

For the primary eight-suite summary, the paired case ratios were combined
geometrically within Crypto and EarleyBoyer, then geometrically across the
eight equally weighted suites in each round:

| Aggregation | Round 0 | Round 1 | Round 2 | Median across rounds |
| --- | ---: | ---: | ---: | ---: |
| Eight suites, equal weight | 0.919 | 0.994 | 1.013 | **0.994** |
| Ten cases, equal weight (secondary) | 0.929 | 0.990 | 1.005 | 0.990 |

The primary round range is **0.919–1.013**. Richards and Crypto/Decrypt
regressed in all three rounds; Splay and NavierStokes improved in all three.
The separate kernel ratios had medians of 0.975 for LCG, 1.089 for sieve
and 1.062 for pi; kernels are not part of the eight-suite aggregate.

**Retain the original O2 / ARM / switch / no-LTO default, with both
pass-disabling options off.** Thumb offers a measured 24.97% guest-binary size
reduction, but the mixed case results, regressions and round variation do not
establish a consistent throughput benefit for this workload. Three rounds on
a nonexclusive machine do not support a general ARM-versus-Thumb speed claim,
nor a browser performance conclusion. The selectable configuration remains
available for reproducing this size/performance tradeoff.
