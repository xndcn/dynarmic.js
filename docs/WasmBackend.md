# WebAssembly backend reference

This document describes the emitter and generated-code interface. Read the
[architecture](Architecture.md) for ownership boundaries and the
[embedding guide](Embedding.md) for host requirements. These are internal code
interfaces, not a stable binary ABI.

## Main module and generated modules

The Emscripten main module contains Dynarmic, the dispatcher, guest state,
callback thunks and guest RAM. On a code-cache miss, it emits and instantiates
another Wasm module. Generated modules import the main module's linear memory,
function table and selected callback helpers, then export one function:

```wat
(func $f (param $state i32) (param $execution_context i32) (result i32)
  ;; emitted guest block or region
)
```

The returned bits describe the halt reason. A zero return can mean that control
must return to the dispatcher for another guest location. The execution context
also carries pending link requests and cycle state.

The runtime registers `f` in the shared function table; a C function pointer in
the Emscripten host is that table index. Direct block chaining uses
`return_call_indirect`, so a chain does not accumulate host call frames. The
runtime clears dependent transfers before a table slot can be reused.

## Values and locals

| IR value | Wasm storage |
| --- | --- |
| U1/U8/U16/U32, packed flags | `i32` |
| U64 | `i64` |
| U128 | `v128` |

Floating-point IR operands are integer bit patterns. Lowering reinterprets them
at scalar or vector FP operations. `reg_alloc.h` assigns locals and reuses them
once IR uses are consumed; the engine performs host register allocation.

Two caches have different purposes: IR temporaries hold intermediate values,
while the guest register cache keeps architectural registers and flags in
locals across instructions and internal region edges. Dirty guest values are
stored on exit or before state-observing callbacks. Such callbacks can change
state, so cached values are reloaded afterward. Pure helpers and memory
callbacks have narrower contracts and avoid these register transfers.

## Terminal lowering

| IR terminal or hint | Generated behavior |
| --- | --- |
| `ReturnToDispatch` | Materialize the next guest location and return |
| `LinkBlock` / `LinkBlockFast` | Enter an internal region edge, use a patched tail-call slot, or request dispatcher resolution |
| `If` | Evaluate the ARM condition and choose a terminal |
| `CheckHalt` | Test the execution context's halt state |
| `CheckBit` | Test the per-block check-bit local |
| `PopRSBHint` | Try the return-stack entry and then the dispatcher/indirect path |
| `FastDispatchHint` | Probe the direct-mapped indirect-transfer table |
| `Interpret` | Invoke the host's interpreter fallback |

Cycle-counted edges must preserve the scheduler's budget accounting; fast
chaining does not bypass those requirements. A32 conditional-branch folding is
disabled for cycle-counted and single-step translations because the separate
frontend blocks can have different cycle costs.

## Regions and loops

`region_selector.*` explores bounded, compatible local successors.
`region_plan.*` analyzes the selected control-flow graph. Reducible regions use
nested Wasm blocks and loops, with forward edges branching to enclosing labels
and back edges to loop headers. The fallback layout uses a `br_table` dispatcher
inside the generated function.

The emitter can keep guest registers in locals for the entire region. A
self-loop uses a local loop and a bounded iteration allowance so execution can
return to the dispatcher. Returns to eligible internal continuation sites can
remain inside the region. Their RSB effects must match the equivalent external
call/return path.

In board mode, calls omit speculative RSB writes because returns already use
guarded dispatch by the full location descriptor. Internal returns likewise
omit the unused RSB pop. The PushRSB IR operation remains available for finding
region continuation sites; ordinary call/return speculation is unchanged.

A32 location changes also carry Thumb, endianness and IT state. A64 and A32
regions require compatible FP configuration. Single stepping uses separate
location descriptors and does not speculate a multi-block region.

## Memory lowering

`emit_wasm_memory.cpp` and `emit_wasm_a64_memory.cpp` implement callback,
page-table and flat-window paths. Page-table accesses require a non-null mapping
and a complete access within a 4 KiB page. A crossing access goes to the callback
with its original guest address; the host handles the complete operation.

A flat window uses the contiguous leading mapping discovered at JIT
construction. Generated code checks the guest address against the derived limit
and accesses linear memory at the configured base. The limit reserves enough
space for the widest emitted access. Those pages must remain mapped throughout
the JIT lifetime.

A64 callbacks take `i64` addresses, even though linear-memory pointers are
`i32`. A64 page-table lowering checks or mirrors the high guest address bits
according to configuration. Wide callback values that cannot pass directly
through the chosen helper ABI use scratch storage in the JIT state.

Exclusive reads and writes call the monitor-aware thunks. Memory callbacks do
not receive a coherent register snapshot; see the
[callback contract](Embedding.md#callback-state-contract).

## Integer, SIMD and floating-point lowering

Most scalar integer operations map to Wasm integer instructions plus explicit
ARM carry/overflow logic. Saturating and packed operations use SIMD when the
Wasm lane semantics match, or lane-wise masks and clamps otherwise. Sticky ARM
status bits must be updated separately from computing the result.

NEON values use `v128`. The emitter combines SIMD instructions, shuffle patterns
and scalarized helpers for operations without a direct Wasm equivalent. Do not
infer lane width or exception semantics from a similar instruction name: ARM
saturation/QC behavior and Wasm NaN behavior differ in several cases.

FP lowering combines inline Wasm arithmetic with Dynarmic soft-float helpers.
Numeric min/max paths explicitly classify NaNs and retain integer payloads
rather than relying on engine NaN propagation. Other FP families have known
rounding, subnormal and exception gaps, documented in
[the support status](Roadmap.md#known-limitations). Adding an opcode lowering is
not by itself proof of conformance across every FP mode.

## Testing a lowering change

The native test executables under `tests/ir/` emit actual Wasm modules; Node
loads those bytes and compares behavior with independent integer, bit-pattern
or byte-addressed references. Guest programs exercise the frontend-to-runtime
path, while Unicorn fuzzing supplies another semantic oracle.

To inspect one block:

```sh
build-native/tests/tools/block_dump loop 1000 block.wasm
build-native/tests/tools/block_dump --raw loop 1000 raw.wasm
build-native/tests/tools/a32_decode e5933000
```

`block_dump` writes IR to stderr. A flat guest image can be selected with
`file.bin@10000`; `--thumb` and `--a64` choose the frontend. The `--raw` option
skips the normal optimization pipeline. Setting `DYN_DUMP_DIR` for an Emscripten
host records emitted modules, but disables valid benchmark collection because
it adds instrumentation.

See [tests/README.md](../tests/README.md) for test entry points and
[CONTRIBUTING.md](../CONTRIBUTING.md) for semantic and performance evidence
requirements. The original design narrative and dated optimization history are
preserved in [internals](../internals/WasmBackendHistory.md).
