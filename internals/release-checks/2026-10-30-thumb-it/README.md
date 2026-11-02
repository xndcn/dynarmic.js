# ARM/Thumb dynamic-exit descriptor correction

Review date: **2026-10-30**. This is a backend correctness fix discovered while
running an ARM QuickJS guest. The page and QuickJS example are separate changes.

## Failure and cause

QuickJS initialization failed in `js_free_shape0` under the Wasm backend, while
the equivalent bare-metal guest completed under QEMU. The apparent invalid
JavaScript value was downstream stack corruption: newlib's Thumb `strlen`
returned with SP eight bytes too high on a later call.

The reduced program revisits one ARM `BLX` call site, first satisfying and then
failing a Thumb `ITT EQ` sequence. Its callee saves `r4/r5` with `STRD`, executes
the IT sequence, restores them with `LDRD`, and returns with `BX LR`. Before the
fix, the three observed stack pointers were:

```text
00ff0000
00ff0008
00ff0008
```

The Wasm `A32BXWritePC` emitter preserved the runtime entry descriptor's IT bits
and changed only T. The frontend had already advanced ITSTATE while translating
the block, so an indirect return after the IT sequence leaked stale ITSTATE
into its destination. A later Thumb entry could consequently predicate
instructions that were unconditional, including the stack save. `LDRD` itself
was not the faulty operation.

Disabling region selection/conditional-branch folding and disabling IR
GetSetElimination/ConstProp did not remove the failure. Executing the original
guest through `Step()` did: **225,781,435 instructions** completed with the
expected results. Forcing a block boundary within the tail of `strlen` also
removed the failure. These experiments narrowed the defect to descriptor state
across multi-instruction blocks rather than QuickJS source or workload inputs.

## Correction and upstream comparison

[`emit_wasm_a32.cpp`](../../../src/dynarmic/backend/wasm/emit_wasm_a32.cpp)
now follows the pinned upstream x64 backend's dynamic-exit rules:

- `A32BXWritePC` obtains the upper descriptor from the block's **EndLocation**,
  clears the single-step marker, and inserts the target's T bit. It aligns Thumb
  destinations to two bytes and ARM destinations to four bytes.
- `A32UpdateUpperLocationDescriptor` writes the completed descriptor when a
  block has no `A32BXWritePC`. This covers table branches, supervisor calls,
  exceptions and the frontend's other explicit update sites. It retains the
  configured always-little-endian mask. A block containing BX leaves the update
  to BX, matching upstream ordering.

The comparison points are `A32EmitX64::EmitA32BXWritePC`,
`EmitA32UpdateUpperLocationDescriptor` and `EmitSetUpperLocationDescriptor` in
`externals/dynarmic/src/dynarmic/backend/x64/a32_emit_x64.cpp`, at pinned upstream
revision `a41c380246d3d9f9874f0f792d234dc0cc17c180`. This is a Wasm
backend defect; these upstream implementations already use the completed
location descriptor. No upstream source change or QuickJS workaround is needed.

## Reproduction and regression coverage

[`thumb_it_return_test.cpp`](../../../tests/hosts/thumb_it_return_test.cpp)
contains fixed instruction streams, so the regression needs no ARM compiler or
QuickJS download. It covers:

1. The repeated ARM→Thumb→ARM call, checking SP and callee-saved registers in
   all three memory modes, with cycle counting and IR optimizations on/off
   (**12 configurations**).
2. SVC in the last IT slot, checking that the host observes cleared ITSTATE.
3. TBB in the last IT slot, checking that the destination's `MOVS` updates Z
   rather than inheriting predication from the source block.
4. BX to an even ARM target with bit 1 set, checking word alignment.

The last three cases each run with cycle counting and IR optimizations on/off
(**12 additional configurations**). CTest registers the host as
`runtime_thumb_it_return_test`; the strict regression script also executes it.

```sh
cmake --build build-em -j2 --target thumb_it_return_test
node build-em/tests/thumb_it_return_test.js
ctest --test-dir build-em -R runtime_thumb_it_return_test --output-on-failure
```

The fixed repeated-call reproducer returned `00ff0000` on all three calls.
The original ARM QuickJS light workload then completed normally and returned
LCG **2778886796**, sieve **1028**, and pi×1e6 **3141542**, without changing its
guest machine code. Separate pre-fix checks also demonstrated the additional
descriptor defects: the SVC case retained CPSR `0x4000043f` (ITSTATE set), and
the TBB case returned `r0=2` instead of `1` because Z was not updated.

Temporary compiler dumps and instrumented guest binaries were used only for
diagnosis and are not source artifacts. Release measurements must use the
ordinary uninstrumented build; the diagnostic runs above are correctness
evidence, not performance measurements.

## Final local verification

- All **24** focused configurations passed with the final emitter.
- `ctest --test-dir build-em -L runtime --output-on-failure`: **9/9** passed.
- `tests/regress.sh --strict build-em build-native`: **exit 0**, no required
  input skips. This includes the ARM/Thumb/AArch64 fuzz matrix, checked-in
  corpus, integer/vector/FP/memory IR checks, all guest memory paths and the
  browser host. External CoreMark was not requested; its application gate is
  not claimed by this run.

`source-sha256.json` identifies the implementation tested. `regression.log` and
`runtime-ctest.log` preserve summaries. `regression.tar.gz` retains the strict
run's exact invocation, command stdout/stderr, guest outputs, fuzz logs/corpora
and timing observations. Its recorded revision is the pre-commit HEAD; the
source hashes bind it to this fix. The worktree also contained the separate
QuickJS/Pages candidate; its UI tests were exercised but are not part of this
backend commit.
