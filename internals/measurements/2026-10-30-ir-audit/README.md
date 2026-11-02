# IR audit validation and performance screen

The implementation and remaining semantic risks are described in
[the audit report](../../IrAudit.md). The checkout started at
`930278597e9a288fd1e835f2814644eca5fe289b`; the candidate is the accompanying
working-tree change. Existing untracked `node_modules/` and `test-results/`
were preserved.

## Semantic evidence

The four new native generators execute through Node against independent
references. Their final checks pass:

- Integer: 1,115,100 cases, including exhaustive byte pairs and all 8-bit shift
  counts, with result and architectural flags checked.
- Vector: 83,180 cases across 40 modules, including full-width random values.
- FP numeric min/max: 77,440 result/FPSR checks across 48 modules.
- A64 memory: 2,880 byte-reference cases plus null/tagged mappings and read/write
  faults on the next page.

The old memory emitter fails a two-byte store crossing guest address `0x3fff`;
`evidence/memory-baseline.log` retains the exact assertion. The vector frozen
baseline has 210 failures. Integer red/green evidence is in the
[packed saturation report](../2026-10-30-packed-saturation/README.md).
The old FP emitter produces 13,832 mismatches in the same 77,440 checks;
`evidence/fp-baseline.log` retains the output. `generated-memory-fp.tar.gz`
freezes the original and final memory/FP modules together with their state
layout metadata. FP fixtures grew from 14,192 to 19,996 bytes overall while correcting missing
semantics; `evidence/fp-bytes.json` also retains the intermediate larger lowering
before NaN classification was simplified.

The final native and Emscripten builds succeeded. Native CTest passed 14/15:
the only failure is the existing `a32_conditional_backward` golden-byte
mismatch in `region_corpus`. Both the earlier and final failing logs are
retained. This is explicitly an unpassed gate, not a refreshed golden file.

`tests/regress.sh --strict build-em build-native` completed with exit code 0
and `regression done fail=0`. It includes the new IR tests, guest/reference
programs, A32/Thumb/A64 Unicorn campaigns in all three memory modes, the saved
fuzz corpus, and 18 browser-host normal/fault halt runs under Node. The complete
console output is retained in `evidence/regress-strict.log`. No dependency skip
was reported. CoreMark was not requested.

## Whole-suite benchmark screen

`evidence/before.json` and `after.json` each contain five fresh processes for
all 12 fixed benchmark cases. `comparison.json` passes the existing throughput,
emitted-byte and compilation-time budgets for every case. In particular, the
A64 page-table case has throughput ratio 0.9659, byte ratio 1.0316 and compile
time ratio 0.9174 after adding the required page-containment guard.

These are sequential cohorts, not an alternating controlled performance
qualification. The baseline used the existing production `build-em` artifacts
before the changes were built; initial audit work could overlap that collection.
The candidate was collected after final builds and native tests had finished.
CPU affinity, governor and unrelated machine activity were not fixed. Large
apparent improvements in unaffected cases are not attributed to these changes.
Use this as a regression screen; exact modules, paired samples and limitations
for the actual optimizations are recorded separately in the
[packed](../2026-10-30-packed-saturation/README.md) and
[vector](vector/README.md) experiments.

## Commands

Run from the repository root:

```sh
# Before rebuilding the production artifacts:
node tests/bench/run.mjs build-em/tests --runs 5 --output /tmp/dynarmic-ir-audit-20261030/before.json

cmake --build build-native -j4
cmake --build build-em -j4
ctest --test-dir build-native --output-on-failure

# After the final build/tests, without rebuilding during collection:
node tests/bench/run.mjs build-em/tests --runs 5 --output /tmp/dynarmic-ir-audit-20261030/after.json
node tests/bench/compare.mjs /tmp/dynarmic-ir-audit-20261030/before.json /tmp/dynarmic-ir-audit-20261030/after.json
tests/regress.sh --strict build-em build-native
```

CoreMark and an actual browser-engine matrix are separate validation tasks;
this round does not establish application speedup or release qualification.
