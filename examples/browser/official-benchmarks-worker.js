// Each official JS case runs in a fresh Worker and QuickJS context. No network
// requests or parsing are counted as benchmark iterations; the upstream harness
// supplies the measurement windows and correctness assertions.
'use strict';
let busy = false;
const names = ['Richards/Richards', 'DeltaBlue/DeltaBlue', 'Crypto/Encrypt', 'Crypto/Decrypt',
  'RayTrace/RayTrace', 'EarleyBoyer/Earley', 'EarleyBoyer/Boyer', 'RegExp/RegExp', 'Splay/Splay', 'NavierStokes/NavierStokes'];
const memoryLimitBytes = 192 * 1024 * 1024;
const now = () => performance.now();
async function file(name) {
  const response = await fetch(name);
  if (!response.ok) throw new Error(`Could not load ${name}: HTTP ${response.status}. Rebuild browser_page and serve its complete dist directory.`);
  return response;
}
function armFeatures() {
  const tail = [0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,10,6,1,4,0,18,0,11];
  const simd = [0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,123,3,2,1,0,10,8,1,6,0,32,0,253,15,11];
  if (typeof WebAssembly !== 'object' || !WebAssembly.validate(new Uint8Array(tail)) || !WebAssembly.validate(new Uint8Array(simd))) {
    throw new Error('The ARM path requires WebAssembly tail calls and SIMD. Use a current browser.');
  }
}
self.onmessage = async ({data}) => {
  if (busy) { self.postMessage({ok: false, error: 'Worker is already running.'}); return; }
  busy = true;
  const {runtime, caseId} = data || {};
  try {
    if (!['dynarmic', 'wasm', 'native'].includes(runtime) || !names.includes(caseId)) throw new Error('Invalid official benchmark request.');
    self.postMessage({type: 'progress', runtime, caseId, phase: 'loading'});
    const startLoad = now();
    let execute, stats = () => ({}), memory = () => null;
    if (runtime === 'native') {
      importScripts('official-v8.js', 'official-v8-adapter.js');
      execute = () => OfficialV8Bench.run(caseId, {}, now);
    } else {
      const [source, adapter] = await Promise.all(['official-v8.js', 'official-v8-adapter.js'].map(name => file(name).then(r => r.text())));
      const bundled = `${source}\n${adapter}\nJSON.stringify(OfficialV8Bench.run(${JSON.stringify(caseId)}, {}, benchNow));\n`;
      const bytes = new TextEncoder().encode(bundled + '\0');
      if (runtime === 'wasm') {
        importScripts('quickjs-wasm.js');
        const lines = [], module = await createQuickJS({print: line => lines.push(String(line))});
        if (module._qjs_set_memory_limit(memoryLimitBytes) !== 0) throw new Error('Could not configure QuickJS memory.');
        const pointer = module._malloc(bytes.length);
        if (!pointer) throw new Error('Could not allocate the official benchmark source.');
        module.HEAPU8.set(bytes, pointer);
        memory = () => module.HEAPU8.byteLength;
        execute = () => {
          try {
            const code = module._qjs_eval(pointer, bytes.length - 1);
            if (code !== 0) throw new Error(`QuickJS Wasm failed (${code}): ${lines.join('\n')}`);
            return JSON.parse(lines.join('\n').trim());
          } finally { module._free(pointer); }
        };
      } else {
        armFeatures();
        importScripts('arm_bench_jit.js');
        const [module, bin] = await Promise.all([createArmJit(), file('quickjs-arm.bin').then(r => r.arrayBuffer())]);
        const ram = module._arm_ram(), size = module._arm_ram_size(), sourceAddress = size - 8 * 1024 * 1024;
        if (size < 256 * 1024 * 1024 || !bin.byteLength || bin.byteLength > sourceAddress - 0x10000 || bytes.length >= 4 * 1024 * 1024) throw new Error('Official benchmark does not fit guest RAM. Rebuild arm_bench_jit.');
        module.HEAPU8.fill(0, ram, ram + size);
        module.HEAPU8.set(new Uint8Array(bin), ram + 0x10000);
        module.HEAPU8.set(bytes, ram + sourceAddress);
        const mailbox = new DataView(module.HEAPU8.buffer, ram + 0xf000, 24);
        [2, sourceAddress, bytes.length - 1, memoryLimitBytes, size, size - 64 * 1024].forEach((value, i) => mailbox.setUint32(i * 4, value, true));
        module._arm_console_clear();
        if (module._arm_init(2, 0) !== 0) throw new Error('Could not initialize the ARM32 guest.');
        memory = () => module.HEAPU8.byteLength;
        stats = () => {
          const raw = module.dynarmicWasmStats || {};
          return Object.fromEntries(Object.entries(raw.instances?.[String(raw.activeInstance)]?.compile || {})
            .filter(([, value]) => typeof value === 'number' && Number.isFinite(value)));
        };
        execute = () => {
          const halt = module._arm_run(0x10000);
          const text = new TextDecoder().decode(module.HEAPU8.subarray(module._arm_console(), module._arm_console() + module._arm_console_len()));
          if (halt !== 0x01000000 || module._arm_reg(0) !== 0) throw new Error(`QuickJS ARM failed (halt 0x${(halt >>> 0).toString(16)}, exit ${module._arm_reg(0)}): ${text}`);
          return JSON.parse(text.trim());
        };
      }
    }
    const loadMs = now() - startLoad;
    self.postMessage({type: 'progress', runtime, caseId, phase: 'executing'});
    const startRun = now(), official = execute(), runMs = now() - startRun;
    self.postMessage({ok: true, runtime, caseId, official, loadMs, runMs, stats: stats(), memoryBytes: memory(),
      quickjsMemoryLimitBytes: runtime === 'native' ? null : memoryLimitBytes});
  } catch (error) { self.postMessage({ok: false, runtime, caseId, error: String(error?.stack || error)}); }
  finally { busy = false; }
};
