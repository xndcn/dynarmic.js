/* One fresh emulator/module per case and round. Six identical calls retain the
 * compiled code: first, two warmups, then three measured calls. */
'use strict';
importScripts('c-bench-protocol.js');
let busy = false;
const P = CBenchProtocol;
const RAM = 16777216, MAILBOX = 0xf000, MAGIC = 0x43424e31;
async function file(name, record) {
  const response = await fetch(name);
  if (!response.ok) throw new Error(`Could not load ${name}: HTTP ${response.status}. Rebuild browser_page and serve its complete dist.`);
  const bytes = await response.arrayBuffer();
  if (record) {
    const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))].map(v => v.toString(16).padStart(2, '0')).join('');
    if (bytes.byteLength !== record.bytes || hash !== record.sha256) throw new Error(`Build identity mismatch: ${name}`);
  }
  return bytes;
}
const json = async name => JSON.parse(new TextDecoder().decode(await file(name)));
function importVerified(bytes) {
  const url = URL.createObjectURL(new Blob([bytes], {type: 'text/javascript'}));
  try {importScripts(url);} finally {URL.revokeObjectURL(url);}
}
function input(caseIndex, iterations, seed) {
  const data = new Uint8Array(128), view = new DataView(data.buffer);
  [MAGIC, caseIndex, iterations, seed].forEach((v, i) => view.setUint32(i * 4, v, true));
  return data;
}
function output(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(16, true) !== 2 || view.getUint32(20, true) !== 6) throw new Error('C guest returned an incomplete or invalid result');
  return Array.from({length: 6}, (_, i) => view.getUint32(24 + i * 4, true));
}
function durations(marks) {
  if (marks.length !== 12 || marks.some(v => !Number.isFinite(v) || v < 0) || marks.some((v, i) => i && v < marks[i - 1])) throw new Error('Invalid host-clock measurement markers');
  return Array.from({length: 6}, (_, i) => marks[i * 2 + 1] - marks[i * 2]);
}
self.onmessage = async ({data}) => {
  if (busy) {self.postMessage({ok: false, error: 'C benchmark Worker is already running'}); return;}
  busy = true;
  const identity = Object.fromEntries(['runtime','caseId','profile','round','token'].map(k => [k, data?.[k]]));
  const progress = phase => self.postMessage({type: 'progress', ...identity, phase});
  try {
    const {runtime, caseId, profile, round} = identity;
    if (!P.RUNTIMES.includes(runtime) || !P.CASE_IDS.includes(caseId) || !['quick','full'].includes(profile) ||
        !Number.isInteger(round) || round < 0 || round >= 3 || typeof identity.token !== 'string') throw new Error('Invalid C benchmark request');
    progress('loading'); const loadStart = performance.now();
    const manifest = P.validateManifest(await json('c-bench-manifest.json'));
    const artifact = name => {
      const record = manifest.artifacts.find(a => a.path === name);
      if (!record) throw new Error('Manifest omits ' + name);
      return file(name, record);
    };
    const ref = P.validateReference(JSON.parse(new TextDecoder().decode(await artifact('c-bench-reference.json'))));
    const config = ref.profiles[profile], iterations = config.iterations[caseId], seed = config.seed;
    const caseIndex = ref.cases.find(c => c.id === caseId).index, expected = config.expected[caseId];
    let loadMs, samples, checksums, memoryBytes, stats = {};
    if (runtime === 'wasm') {
      const bytes = await artifact('c-bench.wasm');
      const module = await WebAssembly.compile(bytes);
      if (WebAssembly.Module.imports(module).length) throw new Error('Direct C Wasm must not import libc or host services');
      const instance = await WebAssembly.instantiate(module, {}), e = instance.exports;
      if (typeof e.bench_init !== 'function' || typeof e.bench_run !== 'function' || !(e.memory instanceof WebAssembly.Memory)) throw new Error('Missing direct C Wasm exports');
      loadMs = performance.now() - loadStart; progress('executing'); samples = []; checksums = [];
      for (let i = 0; i < 6; i++) {
        if (e.bench_init(caseIndex, iterations, seed) !== 0) throw new Error('Direct C Wasm rejected benchmark input');
        const start = performance.now(), checksum = e.bench_run() >>> 0;
        samples.push(performance.now() - start); checksums.push(checksum);
      }
      memoryBytes = e.memory.buffer.byteLength;
    } else if (runtime === 'dynarmic') {
      const host = await json('wasm-build-manifest.json');
      if (host.schema !== 'dynarmic.js/wasm-coremark-host-build-v1' || host.configuration?.guestRamBytes !== RAM ||
          !['js','wasm'].every(kind => host.artifacts?.[kind] && Number.isSafeInteger(host.artifacts[kind].bytes) &&
            host.artifacts[kind].bytes > 0 && /^[a-f0-9]{64}$/.test(host.artifacts[kind].sha256))) throw new Error('Invalid ARM host build record');
      const [binary, javascript, wasmBinary] = await Promise.all([artifact('c-bench-arm.bin'), file('arm_jit.js', host.artifacts.js), file('arm_jit.wasm', host.artifacts.wasm)]);
      importVerified(javascript);
      const module = await createArmJit({wasmBinary: new Uint8Array(wasmBinary)});
      const ram = module._arm_ram();
      if (module._arm_ram_size() !== RAM || binary.byteLength >= RAM - 0x10000) throw new Error('Unexpected C guest memory layout');
      module.HEAPU8.fill(0, ram, ram + RAM);
      module.HEAPU8.set(new Uint8Array(binary), ram + 0x10000);
      module.HEAPU8.set(input(caseIndex, iterations, seed), ram + MAILBOX);
      if (module._arm_init(2, 0) !== 0) throw new Error('ARM initialization failed');
      loadMs = performance.now() - loadStart; progress('executing');
      const halt = module._arm_run(0x10000);
      if (halt !== 0x01000000 || module._arm_reg(0) !== 0) throw new Error(`ARM guest failed: halt 0x${(halt >>> 0).toString(16)}, exit ${module._arm_reg(0)}`);
      const marks = Array.from(new Float64Array(module.HEAPU8.buffer, module._arm_marks(), module._arm_mark_count()));
      samples = durations(marks);
      checksums = output(module.HEAPU8.subarray(ram + MAILBOX, ram + MAILBOX + 128));
      memoryBytes = module.HEAPU8.byteLength;
      const raw = module.dynarmicWasmStats || {};
      stats = Object.fromEntries(Object.entries(raw.instances?.[String(raw.activeInstance)]?.compile || {})
        .filter(([,v]) => typeof v === 'number' && Number.isFinite(v) && v >= 0));
    } else {
      const [wasm, elf, javascript] = await Promise.all([artifact('c-bench-v86.wasm'), artifact('c-bench-x86.elf'), artifact('c-bench-v86.js')]);
      importVerified(javascript);
      let emulator, settled = false;
      const result = await new Promise((resolve, reject) => {
        const fail = error => {if (!settled) {settled = true; reject(error);}};
        emulator = new V86({wasm_fn: async imports => {
          try {return (await WebAssembly.instantiate(wasm, imports)).instance.exports;}
          catch (error) {fail(error); return new Promise(() => {});}
        }, multiboot: {buffer: elf}, memory_size: RAM, autostart: false, disable_jit: false,
        disable_keyboard: true, disable_mouse: true, disable_speaker: true});
        emulator.add_listener('emulator-loaded', () => {
          try {
            const cpu = emulator.v86.cpu, marks = [], device = {};
            if (cpu.memory_size[0] !== RAM) throw new Error('Unexpected x86 guest RAM');
            cpu.io.register_write(0xe200, device, undefined, undefined, value => {
              if (value !== marks.length % 2 || marks.length >= 12) {fail(new Error('Invalid x86 timing marker sequence')); return;}
              marks.push(performance.now());
            });
            cpu.io.register_write(0xe208, device, undefined, undefined, value => {
              if (settled) return;
              try {
                if ((value >>> 0) !== 0) throw new Error('x86 guest returned status ' + (value >>> 0));
                const values = durations(marks), sums = output(emulator.read_memory(MAILBOX, 128));
                const table = cpu.wm.wasm_table; let compiledSlots = 0;
                for (let i = 1024; i < table.length; i++) if (table.get(i)) compiledSlots++;
                settled = true; resolve({samples: values, checksums: sums, memoryBytes: cpu.wasm_memory.buffer.byteLength, stats: {compiledSlots}});
              } catch (error) {fail(error);}
            });
            emulator.write_memory(input(caseIndex, iterations, seed), MAILBOX);
            loadMs = performance.now() - loadStart; progress('executing');
            emulator.run().catch(fail);
          } catch (error) {fail(error);}
        });
      }).finally(async () => {if (emulator?.v86) await emulator.destroy();});
      ({samples, checksums, memoryBytes, stats} = result);
    }
    const result = {ok: true, ...identity, iterations, seed, expected, checksums, loadMs,
      firstMs: samples[0], warmupMs: samples.slice(1, 3), samplesMs: samples.slice(3), memoryBytes, stats,
      validation: {pass: checksums.every(v => v === expected), actual: checksums[5], expected}};
    P.validateResult(result, identity, ref);
    self.postMessage(result);
  } catch (error) {self.postMessage({ok: false, ...identity, error: String(error?.stack || error)});}
};
