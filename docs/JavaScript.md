# JavaScript integration

The npm candidate provides an experimental, typed JavaScript API over a dedicated
Emscripten host. It supports ARM32, Thumb and AArch64, bounded guest RAM, stepping,
cooperative execution budgets and explicit disposal. It has no runtime npm
dependencies and needs no compiler at installation. The existing Pages host and
C++ embedding API remain separate integrations for richer guest environments.

Start with the runnable Node and Vite examples in the
[package README](../packages/dynarmic/README.md). Until a maintainer publishes a
release, install the local tarball built by the [release guide](Releasing.md#npm-package).
The candidate version is `0.1.0-alpha.0`; pin it exactly. Supported runtime:
Node.js 22+, or current Chromium/Firefox with SIMD, tail calls and BigInt.

## Loading the runtime

`import {createEmulator} from 'dynarmic.js'` exposes the only JavaScript entry
point. Initialization is asynchronous and lazy. Each `await createEmulator()`
instantiates its own module and CPU, so simultaneous instances are isolated.
No global factory is installed. CommonJS callers use dynamic `import()`.

| Environment | Asset loading |
| --- | --- |
| Node ESM / CommonJS dynamic import | Default adjacent Wasm; no configuration needed |
| Browser module Worker, no bundler | Host `index.js`, `runtime.mjs`, `runtime.wasm` together and import `index.js` by URL |
| Vite module Worker | Import `dynarmic.js/runtime.wasm?url`, pass its resolved absolute URL, and configure `worker.format: 'es'` |
| Custom asset host | Pass `wasmURL` as an absolute URL string or `URL`; HTTP(S) applies in browsers, Node also supports file URLs |
| Application-controlled loading | Pass `wasmBinary` as `Uint8Array` or `ArrayBuffer` |

`wasmURL` and `wasmBinary` are mutually exclusive. For Node, resolve the exported
asset with `new URL(import.meta.resolve('dynarmic.js/runtime.wasm'))`. Browser
URLs must allow fetches from the Worker origin. For Vite TypeScript consumers,
include `vite/client` types for `?url` imports. The raw generated runtime is
private; only `.`, `./runtime.wasm` and `./package.json` are package exports.

Serve JavaScript with a JavaScript MIME type and Wasm as `application/wasm`.
Content Security Policy must permit Worker scripts, the Wasm fetch and WebAssembly
compilation (typically `worker-src 'self'`, `connect-src 'self'`, and
`script-src 'self' 'wasm-unsafe-eval'`). Adjust sources for your asset origin.
The runtime dynamically compiles Wasm blocks; environments that forbid this
cannot run it. SharedArrayBuffer and cross-origin isolation are not required.

## CPU and memory

Factory options are `architecture: 'a32' | 'a64'` (default A32), `memorySize`
(default 16 MiB), `optimizations`, and the optional asset override. RAM is zero-initialized,
mapped at guest address zero and constrained to 64 KiB–256 MiB in 4 KiB increments.
A64 retains 64-bit CPU values even though this host maps a small RAM window.

For ARM/Thumb/A64, `optimizations: {regionMultiEntry: true}` lets internal region
entries share generated code, reducing repeated compilation. It is **off by
default** because warm execution can regress. The host supplies the necessary
cycle counting and boundary state. Omitting `optimizations`, passing `{}`, or
setting `regionMultiEntry: false` preserves the ordinary construction path.
The independent emission switches `sharedExit` and `constantMemory` also default
to `false`. For example:

```js
const cpu = await createEmulator({
  architecture: 'a32',
  optimizations: {regionMultiEntry: true, sharedExit: true, constantMemory: true},
});
```

`sharedExit` applies to A32/Thumb multi-entry fallback regions, so it also needs
`regionMultiEntry`. `constantMemory` applies
to immediate A32 page-table addresses; the package's flat RAM window is unaffected.
Flags are ignored outside their applicable instructions/layouts. All choices are
fixed at creation; create a new emulator to change them. Non-boolean values and
unknown names reject the factory promise. See the [option table](../packages/dynarmic/README.md#optimization-options)
for scope. Smaller generated modules do not guarantee better warm throughput.

| Method | Contract |
| --- | --- |
| `readMemory(offset, size)` | Returns a copied `Uint8Array`; never a borrowed Wasm view |
| `writeMemory(offset, bytes)` | Copies a `Uint8Array` into RAM and invalidates overlapping translated code |
| `getRegister(index)` / `setRegister(index, value)` | R0–R15 for A32; X0–X30 for A64 |
| `getPC()` / `setPC(value)` | Program counter; 4-byte ARM/A64 or 2-byte Thumb alignment required when running |
| `getSP()` / `setSP(value)` | Stack pointer; A32 aliases R13 |
| `getStatus()` / `setStatus(value)` | Unsigned 32-bit CPSR (A32) or PSTATE (A64) |
| `run({maxTicks})` | Resume execution until an event or cooperative budget boundary |
| `step()` | Execute one instruction, also reporting any event |
| `invalidateCache(offset, size)` / `clearCache()` | Explicit translated-code invalidation |
| `dispose()` | Idempotent cleanup; other methods reject subsequent use |

Register, PC and SP values are unsigned `number` for A32 and unsigned `bigint`
for A64. Memory offsets/sizes and status values are always `number`. Invalid
inputs throw instead of silently truncating. A32 starts with CPSR `0x1f`
(system mode, ARM); `setStatus(0x3f)` selects Thumb with an even PC.

TypeScript overloads infer the correct register type from `architecture`. With
a variable architecture, narrow `cpu.architecture` before passing register values.
Use `try/finally` around every instance. Keep no external references to generated
runtime internals. Dropping a disposed instance permits its Wasm memory to be
garbage collected; disposal cannot force the JavaScript engine to reclaim it.

## Execution and host services

`run()` defaults to 1,000,000 ticks. Accepted budgets are integers from zero to
2³¹−1. Zero runs no instructions and returns `budget`. A tick is a backend work
unit charged at translated block boundaries; execution can overshoot the budget.
It is not an exact CPU cycle, exact instruction count or wall-clock deadline.
Run the CPU in a Worker and terminate that Worker when a hard deadline is needed.

Results include `reason`, `pc`, and the native `haltReason` bitmask. A `svc`
result also includes the immediate `svc` number. PC is ready for resumption after
the SVC: inspect registers/RAM, implement your guest service, update state, and
call `run()` again. No JavaScript callback is invoked on the JIT execution stack.

`memory-fault` includes the guest `address`, `access` (`read`, `write`, `execute`)
and access `size`; `unsupported-instruction` and `exception` expose diagnostic
details. Faults are stop notifications, not transactional rollback: earlier
instructions or writes may already have occurred. A host can correct state and
resume. Invalid Wasm, unsupported engine features or loading failures reject the
factory promise. Do not treat failed output as successful emulation.

For guest self-modifying code, arrange a host stop before re-execution and
invalidate the affected range; only host `writeMemory()` invalidates automatically.
The small JavaScript API does not expose vector/FP register inspection, custom
memory callbacks, devices, an OS or a binary loader. Use
[C++ embedding](Embedding.md) for those. Existing backend floating-point and
exception [limitations](Roadmap.md#known-limitations) still apply.

## Local consumer acceptance

Tests install the real `.tgz` into a temporary project with install scripts
disabled. They execute ARM, Thumb and A64 programs; cover SVC resume, budgets,
faults, memory growth, patching code, independent instances and disposal; and
check Node ESM/CommonJS, strict TypeScript NodeNext/Bundler resolution, publint,
Are the Types Wrong, raw browser Workers and a Vite production Worker under a
nested URL base. See [tests/npm/README.md](../tests/npm/README.md) for commands.
