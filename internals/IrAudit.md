# WASM IR audit, 2026-10-30

This audit follows the current architecture, roadmap and contribution contract.
It covers the shared emitter, A32/A64 register and memory lowering, scalar
integer/flags, packed arithmetic, vector integer/QC and scalar/vector FP
families. Three parallel reviews were followed by cross-review of the fixes.
The upstream submodule was not modified. This is targeted semantic evidence,
not a proof that every IR opcode or FP mode is correct.

## Confirmed defects and changes

| IR family | Defect reproduced before the fix | Change |
| --- | --- | --- |
| A64 memory, 8–128 bits | A page-crossing load/store used the first page's pointer for the entire access, bypassing the next page's mapping or fault | Require the whole access to fit the translated 4 KiB page; retain unaligned accesses within a page |
| SignedSaturatedAdd/SubWithFlag32 | Removing the overflow pseudo-op made integer saturation unexpectedly set FPSCR.QC | Keep the explicit-overflow variants free of implicit QC writes |
| VectorEqual128 | `i8x16.shuffle` consumed both operands needed by the following AND; the emitted module failed validation | Supply the original equality mask separately to AND |
| VectorSignExtend64 | The high result half used the sign of the unused upper input lane | Extend the sign of the low input lane |
| VectorSignedSaturatedDoublingMultiplyLong16/32 | Unused upper input lanes containing INT_MIN falsely set QC | Derive QC from the actual widened product's saturation mask |
| FP[Vector]Min/MaxNumeric32/64 | Signaling NaN, default NaN, input flushing and cumulative exception handling differed between implementations | Share width-aware lowering for sNaN priority, quiet payloads, DN, FZ, signed zero and sticky IOC/IDC |

For example, copying a 16-bit value to guest address `0x3fff` with the next
guest page backed by nonadjacent host storage previously wrote the second byte
outside that mapping. The memory reference checks both read and write
crossings independently, pointer tags, absolute pointers, mirrored high
addresses, tagged null mappings, and missing-next-page faults. Callback virtual
addresses retain all 64 bits. The contiguous flat-window path is unchanged.

## Optimizations

Eight PackedSaturatedAdd/Sub{S,U}{8,16} operations now use Wasm SIMD saturation
and extract the low 32 bits. They preserve CPSR.Q, GE and FPSCR.QC. The test
exhausts every pair of byte values with independently varied neighboring lanes,
and checks 16-bit edge cases and deterministic random inputs. Before/after
modules, process samples and limitations are in the
[packed saturation measurement](measurements/2026-10-30-packed-saturation/README.md).

Variable vector shifts no longer leave one unnecessary `local.get` on the
operand stack at every barrel-shift stage. Doubling multiply-high and
multiply-long reuse the saturation mask already computed for their result,
removing duplicate operand comparisons. Across the 40 vector fixtures, emitted
bytes change from 26,379 to 25,949; this includes the equality correctness fix.
The [five-pair vector measurement](measurements/2026-10-30-ir-audit/vector/README.md)
has mixed timing results. No general application speedup is inferred from it.

The FP consolidation reduces duplicated source, but additional semantic checks
have a generated-code cost, especially for the previously incomplete 64-bit
vector path. NaN comparisons only classify operands; payload selection uses
the original integer bits and does not rely on engine NaN canonicalization.
The 48 FP fixtures total 14,192 bytes before and 19,996 after. Default-mode
scalar32/scalar64/vector32 fixtures shrink (214/249/370 to 198/213/294 bytes),
while vector64 grows to implement the missing semantics. This cost is retained
explicitly rather than presenting source consolidation as a general code-size win.

## Independent regression coverage

All four tests generate actual modules with the native C++ backend, validate
and execute them in Node, then compare to independently expressed references.
They are registered with CTest and `tests/regress.sh`.

| Test | Coverage |
| --- | --- |
| `integer_edge_test` | Packed arithmetic/GE/halving/saturation; 32/64-bit add/sub and flags, all 256 dynamic shift amounts/carry, signed/unsigned division edge cases, high-half multiplication; Number/BigInt reference |
| `vector_edge_test` | Equality, sign extension, logical/arithmetic/rounding/saturating shifts, mixed-sign saturating accumulation, doubling multiply, ignored upper lanes and sticky QC; BigInt reference |
| `fp_edge_test` | Scalar and SIMD 32/64-bit numeric min/max; all DN/FZ combinations, standard versus controlled FP mode, signed zeros, subnormals, infinities, qNaNs/sNaNs with payloads and cumulative flags; bit-pattern reference |
| `memory_edge_test` | 1/2/4/8/16-byte accesses, all pointer/mirroring/tag combinations, independent source/destination page offsets and fault paths; byte-addressed callback reference |

```sh
cmake --build build-native -j4
ctest --test-dir build-native --output-on-failure
cmake --build build-em -j4
tests/regress.sh build-em build-native
```

The existing `region_corpus` test has an A32
`a32_conditional_backward` byte-snapshot mismatch, also reproduced before these
changes. Its CMake driver rebuilds the recorder before comparing snapshots.
Do not treat it as a passing gate or refresh its golden bytes as part of this
IR change. See the handoff for the actual validation results.

Final validation: native and Emscripten builds succeeded; CTest passed 14/15
with only that known snapshot mismatch; strict regression completed with
`regression done fail=0`. The five-process, 12-case fixed benchmark comparison
passed existing budgets. Full logs and measurement limitations are in the
[validation record](measurements/2026-10-30-ir-audit/README.md).

## Remaining work

General inline FP arithmetic still needs a separate strict-mode audit of
FPCR rounding/FZ/DN and FPSR exceptions. Source review identified fixed-to-FP
paths that ignore the rounding operand, and FMULX paths that do not flush input
subnormals. The existing double-to-single ties-away approximation also remains.
These paths were not certified by the numeric-min/max regression; exact guest
reproducers and shared slow/fast-path policy are the next concrete FP work.

Memory callbacks retain the existing contract concerning cached guest registers.
Exclusive-monitor behavior, FP exception traps, all browser engines, and every
reachable combination of IR pseudo-operations are outside the new targeted
tests. Existing guest/reference and Unicorn gates remain necessary.
