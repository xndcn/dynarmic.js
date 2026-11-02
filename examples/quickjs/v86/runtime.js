/* Host services for an unmodified v86 release running the bare-metal guest.
 * Used by both Node smoke checks and the browser comparison Worker. */
(function (global) {
  'use strict';
  const RAM_BYTES = 256 * 1024 * 1024;
  const LIMIT_BYTES = 192 * 1024 * 1024;
  const SOURCE_ADDRESS = RAM_BYTES - 8 * 1024 * 1024;
  const PORTS = {clock: 0xe100, stdout: 0xe104, exit: 0xe108};

  async function runV86({V86, wasm, elf, source, timeoutMs = 600000}) {
    const started = performance.now();
    const bytes = new TextEncoder().encode(source + '\0');
    if (!source.length || bytes.length >= 4 * 1024 * 1024) throw new Error('Invalid bundled source length');
    let emulator, timer, startedRun, loadMs, clockReads = 0, text = '', settled = false;
    let resolveResult, rejectResult;
    const completion = new Promise((resolve, reject) => {resolveResult = resolve; rejectResult = reject;});
    const fail = error => {
      if (!settled) {settled = true; clearTimeout(timer); rejectResult(error instanceof Error ? error : new Error(String(error)));}
    };
    try {
      timer = setTimeout(() => fail(new Error(`v86 guest timed out after ${timeoutMs} ms; output: ${text.slice(-2000)}`)), timeoutMs);
      emulator = new V86({
        // Instantiate exactly the supplied, hashed release bytes. The ordinary
        // wasm_path loader can silently fall back to a different Wasm binary.
        wasm_fn: async imports => {
          try {return (await WebAssembly.instantiate(wasm, imports)).instance.exports;}
          catch (error) {fail(error); return new Promise(() => {});}
        },
        multiboot: {buffer: elf}, memory_size: RAM_BYTES, autostart: false,
        disable_jit: false, disable_keyboard: true, disable_mouse: true,
        disable_speaker: true, network_relay_url: '',
      });
      emulator.add_listener('emulator-loaded', () => {
        try {
          const cpu = emulator.v86.cpu;
          if (cpu.memory_size[0] !== RAM_BYTES) throw new Error('Unexpected v86 guest RAM');
          const device = {};
          cpu.io.register_read(PORTS.clock, device, undefined, undefined, () => {
            clockReads++;
            return Math.floor(performance.now() * 1000) | 0;
          });
          cpu.io.register_write(PORTS.stdout, device, value => {
            if (text.length >= 1024 * 1024) {fail(new Error('Guest output exceeded 1 MiB')); return;}
            text += String.fromCharCode(value & 255);
          });
          cpu.io.register_write(PORTS.exit, device, undefined, undefined, value => {
            if (settled) return;
            const runMs = performance.now() - startedRun;
            if ((value >>> 0) !== 0) {fail(new Error(`QuickJS x86 exit ${value >>> 0}: ${text}`)); return;}
            if (!clockReads) {fail(new Error('Guest did not use the host clock')); return;}
            // Count existing JIT functions after timing. Do not wrap or alter
            // code generation, hotness thresholds or scheduling during a run.
            const table = cpu.wm.wasm_table;
            let compiledSlots = 0;
            for (let i = 1024; i < table.length; i++) if (table.get(i)) compiledSlots++;
            settled = true; clearTimeout(timer);
            resolveResult({text, loadMs, runMs, exitCode: value >>> 0,
              memoryBytes: cpu.wasm_memory.buffer.byteLength, guestRamBytes: RAM_BYTES,
              quickjsMemoryLimitBytes: LIMIT_BYTES, stats: {compiledSlots, clockReads},
              jit: {enabled: true, scheduling: 'upstream asynchronous run', tableOffset: 1024}});
          });
          emulator.write_memory(bytes, SOURCE_ADDRESS);
          const mailbox = new Uint8Array(24), request = new DataView(mailbox.buffer);
          [2, SOURCE_ADDRESS, bytes.length - 1, LIMIT_BYTES, RAM_BYTES, RAM_BYTES - 65536]
            .forEach((value, index) => request.setUint32(index * 4, value, true));
          emulator.write_memory(mailbox, 0xf000);
          loadMs = performance.now() - started;
          startedRun = performance.now();
          // A synchronous main_loop would starve v86's asynchronous JIT.
          emulator.run().catch(fail);
        } catch (error) {fail(error);}
      });
      return await completion;
    } finally {
      clearTimeout(timer);
      if (emulator?.v86) await emulator.destroy();
    }
  }
  const api = {runV86, RAM_BYTES, LIMIT_BYTES, PORTS};
  if (typeof module === 'object' && module.exports) module.exports = api;
  else global.QuickJSV86 = api;
})(globalThis);
