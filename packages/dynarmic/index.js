const MIN_MEMORY = 64 * 1024;
const MAX_MEMORY = 256 * 1024 * 1024;
const DEFAULT_MEMORY = 16 * 1024 * 1024;
const MAX_TICKS = 0x7fffffff;
const DEFAULT_TICKS = 1_000_000;
const REASONS = ['halted', 'svc', 'budget', 'memory-fault', 'unsupported-instruction', 'exception', 'step'];

function integer(value, name, maximum, minimum = 0) {
  if (typeof value !== 'number' || !Number.isInteger(value) || value < minimum || value > maximum)
    throw new RangeError(`${name} must be an integer between ${minimum} and ${maximum}`);
  return value;
}

function object(value, name) {
  if (value === null || typeof value !== 'object' || Array.isArray(value))
    throw new TypeError(`${name} must be an object`);
  return value;
}

function bytes(value) {
  if (value instanceof ArrayBuffer) return new Uint8Array(value);
  if (ArrayBuffer.isView(value) && Object.prototype.toString.call(value) === '[object Uint8Array]')
    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
  throw new TypeError('wasmBinary must be a Uint8Array or ArrayBuffer');
}

/** Create an independent, synchronous ARM CPU. Initialization is asynchronous. */
export async function createEmulator(options = {}) {
  object(options, 'options');
  const architecture = options.architecture === undefined ? 'a32' : options.architecture;
  if (architecture !== 'a32' && architecture !== 'a64')
    throw new TypeError('architecture must be "a32" or "a64"');
  const optimizations = options.optimizations === undefined ? {} : object(options.optimizations, 'optimizations');
  for (const name of Object.keys(optimizations)) {
    if (name !== 'regionMultiEntry') throw new TypeError(`Unknown optimization: ${name}`);
  }
  // Read before awaiting initialization so caller mutations cannot change the policy.
  const regionMultiEntry = optimizations.regionMultiEntry === undefined ? false : optimizations.regionMultiEntry;
  if (typeof regionMultiEntry !== 'boolean') throw new TypeError('optimizations.regionMultiEntry must be a boolean');
  const memorySize = integer(options.memorySize === undefined ? DEFAULT_MEMORY : options.memorySize, 'memorySize', MAX_MEMORY, MIN_MEMORY);
  if (memorySize % 4096 !== 0) throw new RangeError('memorySize must be a multiple of 4096 bytes');
  if (options.wasmURL !== undefined && options.wasmBinary !== undefined)
    throw new TypeError('Specify either wasmURL or wasmBinary, not both');
  let wasmURL;
  if (options.wasmURL !== undefined) {
    if (typeof options.wasmURL !== 'string' && !(options.wasmURL instanceof URL))
      throw new TypeError('wasmURL must be an absolute URL string or URL');
    try { wasmURL = new URL(options.wasmURL); }
    catch { throw new TypeError('wasmURL must be an absolute URL string or URL'); }
    if (!['file:', 'http:', 'https:', 'data:', 'blob:'].includes(wasmURL.protocol))
      throw new TypeError('wasmURL must use file:, http:, https:, data: or blob:');
  }
  // Snapshot the caller's bytes before awaiting any imports or I/O.
  let wasmBinary = options.wasmBinary === undefined ? undefined : bytes(options.wasmBinary).slice();
  const {default: createRuntime} = await import('./runtime.mjs');
  if (wasmURL) {
    if (wasmURL.protocol === 'file:') {
      if (typeof process !== 'object' || !process.versions?.node)
        throw new TypeError('file: Wasm URLs are supported only in Node.js');
      const {readFile} = await import('node:fs/promises');
      wasmBinary = await readFile(wasmURL);
    } else {
      const response = await fetch(wasmURL);
      if (!response.ok) throw new Error(`Unable to load dynarmic.js Wasm: HTTP ${response.status}`);
      wasmBinary = new Uint8Array(await response.arrayBuffer());
    }
  }
  let runtime = await createRuntime(wasmBinary === undefined ? {} : {wasmBinary});
  try {
    if (runtime._dj_create(architecture === 'a32' ? 0 : 1, memorySize, Number(regionMultiEntry)) !== 0)
      throw new Error('Unable to create the dynarmic.js CPU');
  } catch (error) {
    runtime._dj_destroy();
    runtime = null;
    throw error;
  }

  const is64 = architecture === 'a64';
  const registerCount = is64 ? 31 : 16;
  let failed = false;
  function active() {
    if (!runtime) throw new Error('The emulator has been disposed');
    if (failed) throw new Error('The emulator encountered a runtime failure; dispose it and create a new instance');
    return runtime;
  }
  function value(input, name) {
    if (!is64) return BigInt(integer(input, name, 0xffffffff));
    if (typeof input !== 'bigint' || input < 0n || input > 0xffffffffffffffffn)
      throw new RangeError(`${name} must be an unsigned 64-bit bigint`);
    return input;
  }
  // WebAssembly's i64 exports return signed BigInts even for C++ uint64_t.
  const output = input => is64 ? BigInt.asUintN(64, input) : Number(BigInt.asUintN(32, input));
  function range(address, size) {
    integer(address, 'address', memorySize);
    integer(size, 'size', memorySize);
    if (size > memorySize - address) throw new RangeError('Memory range extends beyond guest RAM');
  }
  function execute(maxTicks, step) {
    const module = active();
    try {
      const haltReason = module._dj_run(maxTicks, step) >>> 0;
      const reason = REASONS[module._dj_event_kind()];
      if (!reason) throw new Error('Unknown dynarmic.js stop event');
      const result = {reason, pc: output(module._dj_get_pc()), haltReason};
      const detail = module._dj_event_detail() >>> 0;
      if (reason === 'svc') result.svc = detail;
      if (['memory-fault', 'unsupported-instruction', 'exception'].includes(reason))
        result.address = output(module._dj_event_address());
      if (reason === 'memory-fault') {
        result.access = detail & 0x200 ? 'execute' : detail & 0x100 ? 'write' : 'read';
        result.size = detail & 0xff;
      }
      if (reason === 'unsupported-instruction') result.instructions = detail;
      if (reason === 'exception') result.exception = detail;
      return result;
    } catch (error) {
      failed = true;
      throw error;
    }
  }
  return Object.freeze({
    architecture,
    memorySize,
    get disposed() { return runtime === null; },
    getRegister(index) {
      const module = active();
      return output(module._dj_get_reg(integer(index, 'register index', registerCount - 1)));
    },
    setRegister(index, input) {
      const module = active();
      module._dj_set_reg(integer(index, 'register index', registerCount - 1), value(input, 'register value'));
    },
    getPC() { return output(active()._dj_get_pc()); },
    setPC(input) { active()._dj_set_pc(value(input, 'PC')); },
    getSP() { return output(active()._dj_get_sp()); },
    setSP(input) { active()._dj_set_sp(value(input, 'SP')); },
    getStatus() { return active()._dj_get_status() >>> 0; },
    setStatus(input) { active()._dj_set_status(integer(input, 'status', 0xffffffff)); },
    readMemory(address, size) {
      const module = active();
      range(address, size);
      const start = (module._dj_ram() >>> 0) + address;
      return module.HEAPU8.slice(start, start + size);
    },
    writeMemory(address, input) {
      const module = active();
      if (!ArrayBuffer.isView(input) || Object.prototype.toString.call(input) !== '[object Uint8Array]')
        throw new TypeError('Memory data must be a Uint8Array');
      range(address, input.byteLength);
      module.HEAPU8.set(input, (module._dj_ram() >>> 0) + address);
      if (input.byteLength) module._dj_invalidate(BigInt(address), input.byteLength);
    },
    invalidateCache(address, size) {
      const module = active();
      range(address, size);
      if (size) module._dj_invalidate(BigInt(address), size);
    },
    clearCache() { active()._dj_clear_cache(); },
    run(runOptions = {}) {
      active();
      object(runOptions, 'run options');
      return execute(integer(runOptions.maxTicks === undefined ? DEFAULT_TICKS : runOptions.maxTicks, 'maxTicks', MAX_TICKS), 0);
    },
    step() { return execute(MAX_TICKS, 1); },
    dispose() {
      if (!runtime) return;
      const module = runtime;
      runtime = null;
      module._dj_destroy();
    },
  });
}
