// One fresh Worker per path; loading never blocks the page. Termination is the
// cancellation boundary even while a synchronous guest/interpreter is running.
'use strict';
let busy = false;
const now = () => performance.now();
const finiteStats = object => Object.fromEntries(Object.entries(object || {})
  .filter(([, value]) => typeof value === 'number' && Number.isFinite(value)));
function armFeatures() {
  const tail = [0,97,115,109,1,0,0,0,1,4,1,96,0,0,3,2,1,0,10,6,1,4,0,18,0,11];
  const simd = [0,97,115,109,1,0,0,0,1,6,1,96,1,127,1,123,3,2,1,0,10,8,1,6,0,32,0,253,15,11];
  if (typeof WebAssembly !== 'object' || !WebAssembly.validate(new Uint8Array(tail)) || !WebAssembly.validate(new Uint8Array(simd))) {
    throw new Error('The ARM path requires WebAssembly tail calls and SIMD. Update your browser and try again.');
  }
}
async function file(name) {
  const response = await fetch(name);
  if (!response.ok) throw new Error(`Could not load ${name}: HTTP ${response.status}. Rebuild browser_page and serve the complete dist directory.`);
  return response;
}
function parseOutput(lines) {
  // The host emits one JSON line. Never accept partial/truncated guest output.
  return JSON.parse(lines.join('\n').trim());
}
self.onmessage = async ({data}) => {
  if (busy) { self.postMessage({ok: false, error: 'Worker is already running.'}); return; }
  busy = true;
  const {runtime, profile} = data || {};
  try {
    if (!['dynarmic', 'wasm', 'native'].includes(runtime) || !['light', 'full'].includes(profile)) throw new Error('Invalid comparison request.');
    self.postMessage({type: 'progress', runtime, phase: 'loading'});
    const t0 = now();
    let execute, stats = () => ({}), memory = () => null;
    if (runtime === 'native') {
      importScripts('quickjs-workloads.js');
      execute = () => QuickJSBench.run(profile, now);
    } else if (runtime === 'wasm') {
      importScripts('quickjs-wasm.js');
      const lines = [];
      const module = await createQuickJS({print: line => lines.push(String(line))});
      memory = () => module.HEAPU8.byteLength;
      execute = () => {
        const code = module._qjs_bench(profile === 'full' ? 1 : 0);
        if (code !== 0) throw new Error(`QuickJS Wasm failed (${code}): ${lines.join('\n')}`);
        return parseOutput(lines);
      };
    } else {
      armFeatures();
      importScripts('arm_jit.js');
      const [module, bin] = await Promise.all([createArmJit(), file('quickjs-arm.bin').then(r => r.arrayBuffer())]);
      const ram = module._arm_ram(), size = module._arm_ram_size();
      if (!bin.byteLength || bin.byteLength > size - 0x10000) throw new Error('QuickJS ARM image does not fit guest RAM.');
      module.HEAPU8.fill(0, ram, ram + size);
      module.HEAPU8.set(new Uint8Array(bin), ram + 0x10000);
      new DataView(module.HEAPU8.buffer).setUint32(ram + 0xf000, profile === 'full' ? 1 : 0, true);
      module._arm_console_clear();
      if (module._arm_init(2, 0) !== 0) throw new Error('Could not initialize the ARM32 guest.');
      memory = () => module.HEAPU8.byteLength;
      stats = () => {
        const raw = module.dynarmicWasmStats || {};
        return finiteStats(raw.instances?.[String(raw.activeInstance)]?.compile);
      };
      execute = () => {
        const halt = module._arm_run(0x10000);
        const text = new TextDecoder().decode(module.HEAPU8.subarray(module._arm_console(), module._arm_console() + module._arm_console_len()));
        if (halt !== 0x01000000) throw new Error(`QuickJS ARM fault: 0x${(halt >>> 0).toString(16)}. ${text}`);
        if (module._arm_reg(0) !== 0) throw new Error(`QuickJS ARM exited with status ${module._arm_reg(0)}. ${text}`);
        return parseOutput([text]);
      };
    }
    const loadMs = now() - t0;
    self.postMessage({type: 'progress', runtime, phase: 'executing'});
    const start = now();
    const benchmark = execute();
    const runMs = now() - start;
    self.postMessage({ok: true, runtime, benchmark, loadMs, runMs, stats: stats(), memoryBytes: memory()});
  } catch (error) {
    self.postMessage({ok: false, runtime, error: String(error?.message || error)});
  } finally { busy = false; }
};
