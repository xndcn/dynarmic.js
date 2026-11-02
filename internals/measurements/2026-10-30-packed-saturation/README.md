# Packed saturation lowering and integer IR audit (2026-10-30)

The eight A32 `PackedSaturated{Add,Sub}{U,S}{8,16}` operations now use Wasm
SIMD saturating arithmetic. The old implementation extracted and clamped every
lane in scalar i32 arithmetic. The replacement splats each packed i32, performs
one SIMD saturation, and extracts the low i32. These ARM media operations do not
update CPSR.Q or FPSCR.QC; the discarded SIMD lanes therefore have no side effects.

A separate correctness fix prevents `SignedSaturated{Add,Sub}WithFlag32` from
writing FPSCR.QC when its overflow pseudo-operation is absent. These operations
expose overflow as an IR value for CPSR.Q handling; removing that value does not
make them floating-point saturation operations. The upstream x64 backend's
`has_overflow_inst` specialization confirms this distinction. The old emitter
reproduces a failure for `1 + INT32_MAX`: FPSR changes from `0x00400015` to
`0x08400015`, although it should remain unchanged. The original assertion output
is retained in `evidence-regression/`.

## Semantic checks

`tests/ir/integer_edge_test.cpp` emits 64 small modules. The accompanying Node
runner uses independent per-lane integer arithmetic and BigInt references. Its
1,115,100 deterministic cases pass with the candidate (seed `0x32c0ffee`):

- All packed saturating, wrapping, GE-producing and halving add/sub families,
  including exchanged halfwords. Every 8-bit operand pair is exercised, with
  different neighboring lanes; 16-bit operations include boundary values and
  seeded full-word samples.
- Signed scalar saturation with overflow absent/present, ordinary QC-producing
  scalar saturation, initially set/clear QC, and preservation of unrelated FPSR,
  CPSR.Q and GE bits.
- 32/64-bit addition and subtraction with independently varying carry input and
  no flags, overflow-only, or NZCV consumers.
- 32/64-bit shifts and rotations for every dynamic count from 0 to 255;
  32-bit shifts also check carry-out and both carry-in values.
- Signed/unsigned division, including zero divisors and `INT_MIN / -1`, and
  signed/unsigned high-half 64-bit multiplication.

The scalar arithmetic, shift, division, high multiplication and CRC lowering
were inspected against their IR/upstream semantics. No further definite issue
was found in those implementations. CRC table execution is outside this native
module oracle because its tables reside in the Emscripten main module's memory;
CRC behavior remains covered by the existing guest regression suite. Immediate
shift specialization and every possible 32/64-bit operand combination are not
exhaustively covered by this new test.

## Matched generated-code measurements

The final `evidence/` cohort contains five alternating fresh-process pairs,
executed without concurrent agent builds or test workloads. `evidence-loaded/`
preserves the entire earlier five-pair cohort: another agent compiled one object
for several seconds during that cohort. It is excluded from the table below;
no individual sample was removed from either cohort.
Two earlier pilots are retained in `evidence-pilot/`: a 1,000,000-call baseline
and a 5,000,000-call candidate, with different warm-up counts. The candidate
pilot overlapped an Emscripten build. These unmatched pilots are not compared.

Each process measures all eight operations. Each generated module is a chain
of 64 identical packed saturation IR instructions with dynamic register inputs.
The runner checks the chain result independently, warms it for 200,000 calls,
then measures 5,000,000 calls. The measured interval includes JavaScript-to-Wasm
call overhead, excludes instantiation, and performs no guest memory callbacks.
Both variants use the same runner, inputs, operation count and shared-memory ABI.
The modules have no profile instrumentation.

Environment: Intel Core i7-12700, Linux x86-64, Node v24.19.0,
V8 13.6.233.17-node.51. CPU affinity/governor and unrelated host processes were
not controlled. This is a focused throughput experiment on one engine and host,
not an application benchmark or release qualification. Some measured candidate
kernels are shorter than one second; dispersion is retained in `analysis.json`.

| Operation | Baseline median ms | Candidate median ms | Time change | Module bytes |
| --- | ---: | ---: | ---: | ---: |
| `PackedSaturatedAddU8` | 1380.974 | 375.129 | -72.84% | 15113 → 1033 |
| `PackedSaturatedAddS8` | 1358.651 | 371.032 | -72.69% | 15113 → 1033 |
| `PackedSaturatedSubU8` | 1366.784 | 368.564 | -73.03% | 15113 → 1033 |
| `PackedSaturatedSubS8` | 1335.869 | 377.167 | -71.77% | 15113 → 1033 |
| `PackedSaturatedAddU16` | 938.723 | 379.666 | -59.56% | 8457 → 1097 |
| `PackedSaturatedAddS16` | 867.558 | 366.004 | -57.81% | 8457 → 1097 |
| `PackedSaturatedSubU16` | 966.707 | 365.583 | -62.18% | 8457 → 1097 |
| `PackedSaturatedSubS16` | 858.759 | 368.047 | -57.14% | 8457 → 1097 |

All eight kernels improve in this experiment. The byte reductions are 93.16%
for byte operations and 87.03% for halfword operations. This does not establish
a CoreMark, browser, or embedding-application speedup.

## Artifacts and reproduction

The baseline was emitted by the existing native library at repository revision
`930278597e9a288fd1e835f2814644eca5fe289b`. The isolated candidate linked that
same library with newly compiled `emit_wasm_packed.cpp` and
`emit_wasm_saturation.cpp` objects overriding their archive members. All were
compiled with `c++ -std=c++20 -O3 -DNDEBUG` and the repository's Dynarmic, mcl,
fmt and backend include paths. This avoided rebuilding shared artifacts while
other agents audited independent subsystems. The integrated native/Emscripten
build and full regression are reported separately in the handoff.

`generated-wasm.tar.gz` freezes both module directories and their manifests;
`inventory.json` hashes every contained file and the archive. The performance
modules are identical between the initial and final cohorts. `environment.json`
records source and benchmark-module SHA-256 values; each cohort retains all
process exit codes, commands, start times, wall times, stdout JSON and stderr.
The baseline contains the original 38 audit modules; the candidate additionally
contains 26 scalar audit modules added later. Those additions do not change any
of the eight benchmark modules or the measured workload.

From the repository root:

```sh
# Fresh generator output and correctness checks after an ordinary native build:
node tests/ir/integer_edge_test.mjs build-native/tests/integer_edge_test

# Replay exactly the frozen generated-code workload:
mkdir -p /tmp/packed-saturation-replay
tar -xzf internals/measurements/2026-10-30-packed-saturation/generated-wasm.tar.gz \
  -C /tmp/packed-saturation-replay
python3 internals/measurements/2026-10-30-packed-saturation/collect.py \
  /tmp/packed-saturation-replay/baseline /tmp/packed-saturation-replay/candidate \
  /tmp/packed-saturation-replay/new-evidence

# Recompute every retained sample; no trimming:
python3 internals/measurements/2026-10-30-packed-saturation/analyze.py \
  internals/measurements/2026-10-30-packed-saturation/evidence
```
