# Architecture

dynarmic.js compiles Dynarmic itself to WebAssembly, then uses Dynarmic's ARM
frontends to translate guest code into additional Wasm modules at runtime.
Each generated module imports the main module's memory and function table.
The host runs in Node.js or a browser Worker.

For integration requirements, see [Embedding.md](Embedding.md). The
[backend design notes](WasmBackend.md) explain individual lowering strategies;
[Performance.md](Performance.md) defines measurement and profiling contracts.

## Ownership boundaries

| Layer | Responsibility |
| --- | --- |
| Upstream frontend and IR | Decode ARM/Thumb/AArch64 instructions and run Dynarmic optimization passes |
| Wasm emitter (`emit_wasm*`) | Lower IR into modules, allocate locals, emit memory paths and control flow |
| Region selector (`region_selector.*`) | Explore compatible local successors within explicit budgets |
| Address space (`address_space.*`) | Cache translations, allocate links, instantiate modules and invalidate code |
| Runtime and dispatcher (`core.h`, `wasm_runtime_*`) | Execute table entries, account for cycles and bridge to the engine |

Upstream is a pinned, unmodified submodule. The build applies portability and
integration patches to copies of upstream sources. The ARM decoder adapter
uses upstream instruction definitions with a compact compile-time index; a
native test compares ordered buckets against the original upstream decoder.

The native build uses the same emitter with a stub runtime. It supports
inspection and emitted-module tests, but execution requires the Emscripten
runtime or a test harness that instantiates the emitted bytes.

## Translation and execution

On a cache miss, an address space translates the requested guest location,
optimizes its IR and asks the region selector for compatible local successors.
The emitter serializes a single block or multi-block region into a Wasm module.
The runtime synchronously instantiates it, registers its exported function and
returns a function-table index to the dispatcher.

The generated entry ABI is:

```text
(state: i32, execution_context: i32) -> halt_bits: i32
```

The execution context identifies the active callbacks, cycle budget, link
requests and halt state. Guest registers and frequently used flags stay in Wasm
locals inside a region. State-observing calls and exits materialize them in the
JIT state. Memory callbacks use a narrower contract: they may access guest
memory but must not inspect or change cached guest registers.

Direct edges use `return_call_indirect` once their link slots have been patched.
An eight-entry return stack buffer handles call/return locality; a direct-mapped
fast-dispatch table handles indirect transfers. An unresolved edge returns to
the dispatcher, which looks up or compiles the target and patches the link.

## Bounded regions

Current limits are 256 guest instructions per block, 64 blocks and 2,000 IR
instructions per region, with successors within 4 KiB of the original entry.
Targets must have compatible frontend state and FP modes; single-step execution
uses single-step translations.

Reducible control flow uses nested Wasm `block` and `loop` constructs. Other
regions use an internal `br_table` dispatcher. Guest self-loops can use a local
Wasm loop. Edges that cannot be represented within the selected region leave it
through the ordinary dispatcher/chaining mechanism.

Selection and emission reachability are distinct. A structured layout may omit
selected blocks that are unreachable from its original entry; the fallback
layout serializes the selected blocks. A selected continuation does not by
itself become an externally callable entry. Experimental A64 fallback aliases
remain disabled by default; their tests and qualification history are retained
in [internals](../internals/README.md).

## Memory

Generated loads and stores use one of three paths:

1. Callback thunks for every access when no page table is configured.
2. A 4 KiB page-table lookup with a direct Wasm load/store for a non-null,
   page-contained access; otherwise a callback.
3. A bounds check and direct load/store for an established contiguous flat
   window, with callbacks outside the window.

The host owns guest permissions, MMIO and crossing-access behavior. It must keep
flat-window mappings stable for the JIT lifetime and explicitly invalidate
translated code after guest instruction memory changes. A64 callback virtual
addresses retain 64 bits even though host pointers are wasm32.

## Cache and table lifetime

Each generated module owns its table entry and link slots. Cache invalidation
clears dependent link slots, return-stack entries and fast-dispatch entries
before releasing that module. Range invalidation is conservative: extra
invalidation is acceptable, stale executable code is not. Zero-length ranges
are no-ops; nonzero ranges saturate at the frontend's address-space limit.

Generated functions use Emscripten's shared table allocator and table mirror.
Registration skips the generic function-deduplication scan because each module
exports a fresh function. Ordinary `addFunction` calls remain interoperable:
its lazy scan includes live generated entries, and releases update both table
and mirror. Tests cover slot reuse, isolation and bounded table growth. This
integration must be revalidated when changing Emscripten versions.

`Run()` and `Step()` consume queued cache invalidation without discarding other
halt bits. A callback may request both invalidation and a user halt; only the
cache request is consumed. `Step()` entered with a pending user halt returns
without executing an instruction or adding a step-completion reason.

## Observability

Compile totals are owned by each JIT: C++ records translation, optimization and
emission, while JavaScript records engine compilation, instantiation and table
registration. Snapshots publish at `Run()`/`Step()` boundaries under
`Module.dynarmicWasmStats.instances[id]`. The top-level compatibility mirror
represents the active instance, not a sum of all instances.

`DYNARMIC_JS_PROFILE=ON` adds counters for dispatch, caching, linking,
invalidation and region selection. The default production build removes those
counter updates; compile-phase timers still run. Optional region diagnostics
retain append-only compile histories without generated-code counters. See the
[schemas and measurement rules](Performance.md) before comparing results.

## Design invariants

- No released table index is reachable from a link slot, return-stack entry or
  fast-dispatch entry.
- State-observing callbacks see materialized guest state; memory callbacks obey
  the separate no-register-access contract.
- Region expansion is local and bounded; incompatible or unsupported successors
  leave through the dispatcher.
- The host serializes memory/code changes, compilation, execution and cache
  invalidation on one thread.
- Semantic changes require independent reference tests or minimized
  differential-fuzz cases. Performance changes require compatible artifacts
  from the same workload and memory mode.

The [support status](Roadmap.md) records known limitations and planned work.
Historical experiments and dated validation results are kept separately under
[internals](../internals/README.md).
