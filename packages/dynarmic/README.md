# dynarmic.js

Run ARM32, Thumb and AArch64 machine code in Node.js or a browser Worker using
Dynarmic's WebAssembly JIT. This is the **experimental `0.1.0-alpha.0` JavaScript
API**. Pin the exact version: the API may change before 1.0.

Download a versioned `.tgz` from [GitHub Releases](https://github.com/xndcn/dynarmic.js/releases)
and install it directly. If that version has not been released yet, build the
same tarball using the [release procedure](https://github.com/xndcn/dynarmic.js/blob/main/docs/Releasing.md#npm-package).
Registry publication is not required. Use your downloaded file's name:

```sh
npm install ./dynarmic.js-0.1.0-alpha.0.tgz
```

No compiler, postinstall script or runtime npm dependency is needed by consumers.
Use Node.js 22 or newer, or a browser with WebAssembly SIMD, tail calls and BigInt.

## Node.js

Save this as `hello.mjs` and run `node hello.mjs`:

```js
import {createEmulator} from 'dynarmic.js';

const cpu = await createEmulator({architecture: 'a32', memorySize: 65536});
try {
  // mov r0, #40; add r0, r0, #2; svc #7 (little endian ARM)
  cpu.writeMemory(0x1000, Uint8Array.of(
    0x28, 0x00, 0xa0, 0xe3, 0x02, 0x00, 0x80, 0xe2, 0x07, 0x00, 0x00, 0xef,
  ));
  cpu.setPC(0x1000);
  const result = cpu.run({maxTicks: 1000});
  console.log(result.reason, result.svc, cpu.getRegister(0)); // svc 7 42
} finally {
  cpu.dispose();
}
```

CommonJS applications can use `const {createEmulator} = await import('dynarmic.js')`
inside an async function. The package is ESM; it does not ship a synchronous
CommonJS implementation. Importing the module alone does not instantiate Wasm.
TypeScript declarations are included; A32 registers use `number`, A64 uses `bigint`.

## Browser Worker and Vite

Create a module Worker from the page:

```js
const worker = new Worker(new URL('./worker.js', import.meta.url), {type: 'module'});
worker.onmessage = ({data}) => console.log(data);
```

In `worker.js`, use Vite's explicit asset import to retain the Wasm file in the
production build:

```js
import {createEmulator} from 'dynarmic.js';
import wasmURL from 'dynarmic.js/runtime.wasm?url';

const cpu = await createEmulator({
  architecture: 'a64', memorySize: 65536,
  wasmURL: new URL(wasmURL, self.location.href),
});
try {
  // mov x0, #42; svc #0
  cpu.writeMemory(0x1000, Uint8Array.of(0x40, 0x05, 0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4));
  cpu.setPC(0x1000n);
  const result = cpu.run();
  postMessage({reason: result.reason, value: cpu.getRegister(0).toString()});
} finally {
  cpu.dispose();
}
```

Use `worker: {format: 'es'}` in the Vite configuration. When serving without a
bundler, copy `index.js`, `runtime.mjs` and `runtime.wasm` together and import
`createEmulator` from the hosted `index.js` URL. The default Wasm URL is relative
to that module. Serve `.mjs` as JavaScript and `.wasm` as `application/wasm`.
For a relocated asset, pass an absolute `wasmURL`; to load it yourself, pass
`wasmBinary` as `Uint8Array` or `ArrayBuffer`. Pass only one override.

Guest execution is synchronous. Use a Worker for responsiveness and terminate
the Worker for a hard cancellation. `maxTicks` is a cooperative block-boundary
budget, not a wall-clock deadline or exact instruction count. The default is
1,000,000; `0` returns immediately. A `svc` result returns control to your host;
implement the requested service, then call `run()` to resume.

## Optimization options

ARM/Thumb and A64 can share compiled regions across internal entry points:

```js
const cpu = await createEmulator({
  architecture: 'a32',
  optimizations: {regionMultiEntry: true},
});
// Load and run guest code, then release the instance.
cpu.dispose();
```

All optimization switches default to `false` and are fixed when creating an
instance. Unknown names and non-boolean values reject initialization.

| JavaScript option | Applies to | Effect |
| --- | --- | --- |
| `regionMultiEntry` | ARM/Thumb/A64 | Share compiled regions across internal entry points |
| `sharedExit` | ARM/Thumb multi-entry regions | Share register/cycle submission at normal and tail-call exits; requires `regionMultiEntry` |
| `constantMemory` | ARM/Thumb page-table accesses | Specialize immediate addresses while retaining live page mappings and callbacks |

For example, `optimizations: {regionMultiEntry: true, sharedExit: true, constantMemory: true}`
selects those policies independently. Create a new instance to change them.
Flags do nothing when their applicable instruction, memory path or region layout
is absent. The package's flat RAM window already bypasses page-table lookups, so
`constantMemory` does not change that path. The host manages cycle counting and
boundary state for `regionMultiEntry`.

These policies can reduce emitted input or compilation work while slowing warm
execution. Choose flags using your workload and engine; enabling every flag is
not a performance recommendation.

## Contract and limitations

- Guest RAM starts at address zero; `memorySize` is 64 KiB–256 MiB in 4 KiB
  increments (default 16 MiB). Memory methods use numeric byte offsets for both
  architectures. Reads return copies; host writes invalidate translated code.
- Use `getRegister`/`setRegister`, `getPC`/`setPC`, `getSP`/`setSP` and
  `getStatus`/`setStatus`. A32 registers are R0–R15; A64 registers are X0–X30.
  A32 starts in ARM system mode; `setStatus(0x3f)` selects Thumb system mode.
- `step()` executes one instruction. `run()` reports `svc`, `budget`,
  `memory-fault`, `unsupported-instruction`, `exception` or `halted`; `step()`
  may additionally report `step`. Always inspect the reason.
- `dispose()` releases the CPU and translated modules and is safe to repeat.
  Every other operation fails after disposal. Separate factories have separate
  memory and CPU state. Never share an instance between threads.
- This package provides a CPU and bounded RAM, not an ELF loader, OS, device
  model or hostile-code security sandbox. Floating-point fidelity has known
  limitations. Use the C++ interface for custom devices and memory callbacks.

See the [JavaScript guide](https://github.com/xndcn/dynarmic.js/blob/main/docs/JavaScript.md),
[embedding guide](https://github.com/xndcn/dynarmic.js/blob/main/docs/Embedding.md) and
[known limitations](https://github.com/xndcn/dynarmic.js/blob/main/docs/Roadmap.md).

## License and source

MIT; dependencies retain their own licenses. Each package includes
`LICENSE`, `THIRD_PARTY_NOTICES.md`, `runtime-licenses.txt`, a complete
`source.tar.gz` with recursive dependencies, `REBUILD.md` rebuild instructions,
and manifests binding those files to the runtime. Keep the matching source and
notices available when redistributing the JavaScript/Wasm. `node verify.mjs`
checks the packaged file hashes without a compiler or network access.

[Source](https://github.com/xndcn/dynarmic.js) ·
[Issues](https://github.com/xndcn/dynarmic.js/issues) ·
[Security](https://github.com/xndcn/dynarmic.js/security/policy)
