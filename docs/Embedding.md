# Embedding dynarmic.js

dynarmic.js implements Dynarmic's C++ ARM32 and AArch64 JIT interfaces on a
wasm32 host. It translates and executes CPU instructions; the embedding
application owns memory, devices, loading and guest system services.

The complete [browser host](../examples/browser/arm_web.cpp) demonstrates both
architectures. The [Worker](../examples/browser/worker.js) loads guest binaries
and exposes results to JavaScript. These are example APIs, not a versioned
JavaScript SDK.

For the typed JavaScript package with bounded RAM and host-handled SVC events,
start with the [JavaScript guide](JavaScript.md). This guide covers the lower-level
C++ integration for custom memory maps, callbacks and devices.

## Link an Emscripten host

Vendor this repository, including its Dynarmic submodule, and build it with the
same Emscripten toolchain as your host:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_arm_host LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DYNARMIC_JS_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_JS_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/dynarmic.js)

add_executable(my_arm_host host.cpp)
dynarmic_js_host(my_arm_host
    EXPORTS _main
    LINK_OPTIONS -sENVIRONMENT=worker,node
)
```

Configure with `emcmake cmake`, supplying the separate Boost header path as in
[the build guide](GettingStarted.md). Add your exported C entry points, prefixed
with `_`, to `EXPORTS`. The helper links Dynarmic and exports the callback thunks
imported by generated modules, and enables function-table growth. Keep those
settings when customizing the linker flags. The browser example shows
`MODULARIZE` and additional JavaScript runtime exports.

The native install package exports `dynarmic::dynarmic` through
`find_package(dynarmic CONFIG REQUIRED)`. That package supports native emission
and consumer-link checks; the native runtime cannot execute compiled blocks.
Use source integration and `dynarmic_js_host()` for an executing Wasm host.

## Construct and run a JIT

1. Implement `Dynarmic::A32::UserCallbacks` or `Dynarmic::A64::UserCallbacks`.
   Supply code/data reads, writes, supervisor calls, interpreter fallback,
   exception handling and cycle callbacks. A64 also requires `GetCNTPCT()`.
2. Create the matching `UserConfig`, set `callbacks`, and construct `A32::Jit`
   or `A64::Jit`. Keep callbacks, guest storage, page tables and any exclusive
   monitor alive until after the JIT is destroyed.
3. Load guest memory and initialize CPU state. A32 uses `Regs()` and `SetCpsr()`;
   A64 uses `SetRegister()`, `SetPC()` and `SetSP()`.
4. Call `Run()` or `Step()`, then inspect the returned `HaltReason` and CPU state.
   Your callbacks decide how exceptions, unsupported instructions and supervisor
   calls are handled. Provide an interpreter in `InterpreterFallback()` if your
   application needs to continue through unsupported guest instructions.

`HaltExecution()` queues a halt reason. `ClearHalt()` clears a reason before
execution resumes. A pending user halt makes `Step()` return without executing
an instruction. When cycle counting is enabled, implement `GetTicksRemaining()`
and `AddTicks()` consistently with your scheduler; disable cycle counting only
when the guest has another reliable termination mechanism.

A `Run()` call is synchronous. Confine the JIT, its callbacks and memory changes
to one thread, and do not reenter the same JIT from its callbacks. Browser hosts
should execute in a Web Worker. Sending a Worker message cannot interrupt an
ongoing synchronous call; use guest cycle budgets to yield, or terminate and
recreate the Worker when the entire instance must be abandoned.

## Memory modes

| Configuration | Behavior |
| --- | --- |
| No `page_table` | Reads and writes call your callbacks |
| `page_table` set | Page-contained accesses use mapped host memory; null or crossing entries use callbacks |
| `page_table` and `fastmem_pointer` set | A contiguous leading run of mapped pages can use a bounds-checked flat window |

Page size is 4 KiB. Store page tables and backing memory inside the main Wasm
module's linear memory. For A64, explicitly set
`page_table_address_space_bits` to a value greater than 12 and at most 32; the
upstream default is too large for this backend's wasm32 page table. A64 callback
addresses retain all 64 bits. `silently_mirror_page_table` controls whether high
addresses mirror the table or take the callback path.

A page-table fast path requires the complete access to remain within one page.
Your callbacks must correctly handle unaligned and crossing accesses, invalid
addresses and MMIO. A non-null page entry bypasses callbacks, so mark MMIO or
protected pages with null entries. Do not assume a Wasm bounds trap enforces
individual guest mappings.

The flat window is derived when the JIT is constructed. Its mapped pages and
backing allocation must remain stable for the JIT's lifetime. If that mapping
needs to change, recreate the JIT. Null or noncontiguous mappings use the normal
page-table/callback path when a flat window cannot be established.

### Callback state contract

Generated regions cache guest registers and flags in Wasm locals. Memory
callbacks, including exclusive-memory callbacks, **must not inspect or modify
cached guest CPU registers**: those calls deliberately skip register
flush/reload. Use the callback arguments for the address and value. They may
request a halt; inspect and modify CPU state after `Run()` returns.

State-observing calls such as supervisor calls and exception callbacks use the
emitter's register flush/reload hooks. Translation-time callbacks, including
`MemoryReadCode()` and constant-memory queries, must describe the guest memory
being compiled and must not depend on a current executing instruction.

For exclusive accesses, supply a live `ExclusiveMonitor`, valid processor IDs
and the relevant compare-and-write callbacks. Upstream default exclusive-write
callbacks return failure. The backend's single-threaded runtime does not
establish shared-memory or multi-worker atomic semantics.

## Self-modifying code and invalidation

Writes to guest instruction memory do not automatically invalidate translated
code. Quiesce execution, update memory, then call
`InvalidateCacheRange(address, size)` before running the changed code. Use
`ClearCache()` when the affected range is unknown. A zero-size range is a no-op;
nonzero ranges saturate at the guest address-space limit rather than wrapping.

Invalidation also clears cached links, return-stack entries and indirect
transfers that could refer to released modules. A callback can request a cache
clear and a user halt together; the dispatcher consumes the cache request and
preserves the user halt reason. Do not mutate guest code concurrently with
translation or execution.

Backend-specific boundary-hook specialization has a stronger whole-cache
invalidation contract. It is an internal integration feature; consult its
[source declaration](../src/dynarmic/backend/wasm/a32_interface.h) before use.

## Browser deployment

Serve the host JavaScript, Wasm and Worker files over HTTP(S) from compatible
origins. The browser must support WebAssembly tail calls and SIMD. The current
example uses a classic Worker and `importScripts()`. Its Wasm modules compile
synchronously inside that Worker.

Your Content Security Policy must permit the Worker and WebAssembly
compilation; on browsers supporting it, `wasm-unsafe-eval` controls Wasm
compilation without enabling JavaScript string evaluation. Test the actual
policy and browser combination. The current runtime uses unshared memory and
does not require cross-origin isolation for `SharedArrayBuffer`.

Treat browser demo timings as observations from that engine and workload.
Compile-time statistics and optional profile counters are documented in
[Performance.md](Performance.md); known semantic limits are in
[Roadmap.md](Roadmap.md#known-limitations).
