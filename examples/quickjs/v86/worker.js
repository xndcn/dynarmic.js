// The other arm of the comparison uses the existing, unchanged official ARM
// Worker. Both Workers evaluate the same bundled script and host-clock harness.
'use strict';
let busy = false;
self.onmessage = async ({data}) => {
  if (busy) {self.postMessage({ok: false, error: 'Worker is busy'}); return;}
  busy = true;
  const {caseId, timeoutMs = 600000} = data;
  try {
    const started = performance.now();
    self.postMessage({type: 'progress', runtime: 'v86', caseId, phase: 'loading'});
    importScripts('libv86.js', 'v86-runtime.js');
    const file = async (name, type) => {
      const response = await fetch(name);
      if (!response.ok) throw new Error(`Missing ${name}: HTTP ${response.status}`);
      return response[type]();
    };
    const [source, adapter, wasm, elf] = await Promise.all([
      file('official-v8.js', 'text'), file('official-v8-adapter.js', 'text'),
      file('v86.wasm', 'arrayBuffer'), file('quickjs-x86.elf', 'arrayBuffer'),
    ]);
    const bundled = `${source}\n${adapter}\nJSON.stringify(OfficialV8Bench.run(${JSON.stringify(caseId)}, {}, benchNow));\n`;
    const prepareMs = performance.now() - started;
    const result = await QuickJSV86.runV86({V86, wasm, elf, source: bundled, timeoutMs});
    const official = JSON.parse(result.text.trim());
    self.postMessage({ok: true, runtime: 'v86', caseId, official,
      loadMs: prepareMs + result.loadMs, runMs: result.runMs, memoryBytes: result.memoryBytes,
      guestRamBytes: result.guestRamBytes, quickjsMemoryLimitBytes: result.quickjsMemoryLimitBytes,
      stats: result.stats, jit: result.jit, exitCode: result.exitCode});
  } catch (error) {
    self.postMessage({ok: false, runtime: 'v86', caseId, error: String(error?.stack || error)});
  }
};
