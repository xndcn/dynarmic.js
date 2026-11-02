# Vector IR audit evidence

The audit found and fixed three independent semantic defects:

- `VectorEqual128` consumed both copies of its comparison mask in a shuffle,
  leaving `v128.and` without its other operand. The standalone emitted module
  failed WebAssembly validation. Keep a third mask copy for the final AND.
- `VectorSignExtend64` selected the sign of the unused upper source lane.
  Select the lower lane's sign when extending the low 64 bits to 128 bits.
- `VectorSignedSaturatedDoublingMultiplyLong16/32` checked unused upper source
  lanes when setting FPSR.QC. Reuse the actual widened-product saturation mask
  so those lanes cannot spuriously raise QC.

The doubling multiply high and rounding high operations now also reuse their
existing saturation masks instead of recomputing two comparisons. Variable
shift stages no longer leave an unused copy of their input on the Wasm stack.
The removed stack values had been discarded by terminal transfers; this is
an emitted-code reduction, not an additional demonstrated semantic defect.

## Independent semantic checks

`tests/ir/vector_edge_test.cpp` emits 40 standalone modules using the shared
vector IR and A32 state layout. The Node runner executes them against an
arbitrary-precision BigInt reference, with seed `0x7a63d291`. It covers every
signed low-byte shift amount for 8/16/32/64-bit logical, arithmetic, rounding
and saturating shifts; mixed signed/unsigned saturating accumulate; all six
doubling multiply forms; 128-bit equality; 64-to-128 sign extension; isolated
lane saturation; and sticky QC/unrelated FPSR bits. Random 128-bit operands
include both halves. These modules perform no guest memory access.

The frozen baseline has 210 failures: one invalid equality module, six false
QC updates, and 203 sign-extension mismatches. It executes 82,724 cases because
the invalid module's 456 cases cannot run. The candidate validates all modules
and passes all 83,180 cases. Raw reports are in `evidence/*-semantics.log`.

`generated-wasm.tar.gz` preserves the exact baseline and candidate modules.
The original baseline modules were frozen before a later `region_corpus`
CTest invocation indirectly rebuilt the shared native library. Equality and
sign-extension modules were added before their respective fixes. Each
baseline module therefore retains the original implementation it tests.

## Code size and execution diagnostic

Across these 40 fixed modules, total bytes decrease from 26,379 to 25,949
(-430 bytes). Every doubling multiply module saves 48 bytes. Each signed and
unsigned variable-shift module saves 12/16/20/24 bytes at lane widths
8/16/32/64. Equality gains two necessary bytes; sign-extension is unchanged
in size. `bytes.json` records exact sizes and SHA-256 values of changed modules.

Five alternating fresh-process pairs measured the 14 modules with redundant
code removed. Each process uses Node v24.19.0 / V8 13.6.233.17-node.51,
100,000 warm-up calls and 4,000,000 measured calls per module. Workload inputs,
iteration counts, CPU identity, output words, FPSR, module hashes and every
timing are retained in `evidence/`. All baseline/candidate result words and
flags match. The collector preserves stdout, stderr and process status before
checking results. No artifact was rebuilt during collection.

| Pair | Order | Baseline total ms | Candidate total ms |
| --- | --- | ---: | ---: |
| 0 | Baseline, candidate | 600.363 | 530.974 |
| 1 | Candidate, baseline | 535.725 | 537.438 |
| 2 | Baseline, candidate | 540.242 | 529.851 |
| 3 | Candidate, baseline | 534.224 | 542.632 |
| 4 | Baseline, candidate | 544.948 | 527.799 |

The aggregate median is 540.242 versus 530.974 ms (-1.72%). Two pairs are
slower, and the first baseline sample is higher than the other baselines.
These short microbenchmarks include JS-to-Wasm call overhead; they establish
neither a stable throughput improvement nor application speedup. The exact
byte reduction and independent semantic results support the changes.

Reproduce the semantic gate after a native build:

```sh
node tests/ir/vector_edge_test.mjs build-native/tests/vector_edge_test
```

Reproduce the diagnostic using frozen modules, writing a fresh output directory:

```sh
mkdir -p /tmp/vector-ir-frozen
tar -xzf internals/measurements/2026-10-30-ir-audit/vector/generated-wasm.tar.gz -C /tmp/vector-ir-frozen
node internals/measurements/2026-10-30-ir-audit/vector/collect.mjs \
  /tmp/vector-ir-frozen/baseline /tmp/vector-ir-frozen/candidate /tmp/vector-ir-new-evidence
```

`artifacts.json` hashes the frozen modules archive, scripts, byte report and
raw observations. The broader project regression remains a separate gate.
