import assert from 'node:assert/strict';

export const implementations = Object.freeze(['x86Native', 'qemuUser', 'dynarmicNative']);
export const fourWayImplementations = Object.freeze(['x86Native', 'qemuUser', 'dynarmicNative', 'dynarmicWasm']);
const expectedCrc = Object.freeze({seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'});
const median = values => {
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

export function parseDynarmicDiagnostics(stderr) {
  assert(typeof stderr === 'string', 'missing dynarmic diagnostics');
  const one = (source, label) => {
    const matches = [...stderr.matchAll(new RegExp(source, 'gm'))];
    assert.equal(matches.length, 1, `missing/duplicate ${label}`);
    return matches[0][1];
  };
  const diagnostics = {
    memoryMode: Number(one('^DYNARMIC_MEMORY_MODE=(\\d+)$', 'memory mode')),
    guestBytes: Number(one('^DYNARMIC_GUEST_BYTES=(\\d+)$', 'guest size')),
    runMilliseconds: Number(one('^DYNARMIC_RUN_MS=([\\d.]+)$', 'Run time')),
    haltReason: one('^DYNARMIC_HALT_REASON=0x([0-9a-f]{8})$', 'halt reason'),
  };
  const lines = stderr.split('\n');
  assert(lines.length === 5 && lines[4] === '' && lines.slice(0, 4).every(line =>
    /^DYNARMIC_(?:GUEST_BYTES=\d+|MEMORY_MODE=\d+|RUN_MS=[\d.]+|HALT_REASON=0x[0-9a-f]{8})$/.test(line)),
  'unexpected dynarmic stderr');
  assert.equal(diagnostics.memoryMode, 2, 'unexpected memory mode');
  assert(Number.isSafeInteger(diagnostics.guestBytes) && diagnostics.guestBytes > 0, 'invalid guest size');
  assert(Number.isFinite(diagnostics.runMilliseconds) && diagnostics.runMilliseconds > 0, 'invalid Run time');
  assert.equal(diagnostics.haltReason, '01000000', 'abnormal halt reason');
  return diagnostics;
}

export function rotatingOrder(round) {
  assert(Number.isInteger(round) && round >= 0, 'invalid round');
  return implementations.map((_, index) => implementations[(index + round) % implementations.length]);
}

export function rotatingFourWayOrder(round) {
  assert(Number.isInteger(round) && round >= 0, 'invalid round');
  return fourWayImplementations.map((_, index) => fourWayImplementations[(index + round) % fourWayImplementations.length]);
}

function validateObservation(observation, sequence, options, cohort = implementations, order = rotatingOrder) {
  assert.equal(observation.sequence, sequence, 'incorrect sequence');
  const round = Math.floor(sequence / cohort.length);
  assert.equal(observation.round, round, 'incorrect round');
  assert.equal(observation.implementation, order(round)[sequence % cohort.length], 'incorrect implementation order');
  assert.equal(observation.exitCode, 0, 'nonzero exitCode');
  assert.equal(observation.signal, null, 'unexpected signal');
  assert.equal(observation.timedOut, false, 'timedOut');
  assert(Number.isFinite(observation.wallMilliseconds) && observation.wallMilliseconds > 0, 'invalid wallMilliseconds');
  const metrics = observation.metrics;
  const expectedTicks = metrics.ticks;
  assert(Number.isInteger(expectedTicks) && expectedTicks > 0 && expectedTicks <= 0xffffffff, 'invalid ticks');
  assert(Math.abs(metrics.seconds - expectedTicks / 1e6) <= 1.1e-6, 'inconsistent seconds');
  const expectedRate = options.iterations / (expectedTicks / 1e6);
  assert(Math.abs(metrics.iterationsPerSecond - expectedRate) <= Math.max(1e-5, expectedRate * 1e-8), 'inconsistent throughput');
  assert.deepEqual(metrics.crc, expectedCrc, 'incorrect crcfinal or individual CRC');
  assert.equal(metrics.durationValid, expectedTicks >= 10000000, 'incorrect duration qualification');
  assert(observation.wallMilliseconds + 0.01 >= expectedTicks / 1000, 'kernel time exceeds process wall time');
}

export function summarizeComparison(observations, options) {
  assert(Number.isInteger(options.iterations) && options.iterations > 0, 'invalid iterations');
  assert(Number.isInteger(options.runsPerImplementation) && options.runsPerImplementation > 0, 'invalid run count');
  assert.equal(observations.length, options.runsPerImplementation * implementations.length, 'incorrect observation count');
  observations.forEach((observation, sequence) => validateObservation(observation, sequence, options));
  const summaries = {};
  for (const implementation of implementations) {
    const samples = observations.filter(observation => observation.implementation === implementation);
    assert.equal(samples.length, options.runsPerImplementation, `incorrect ${implementation} sample count`);
    const ticks = samples.map(sample => sample.metrics.ticks);
    const medianTicks = median(ticks);
    const kernelMedianMs = medianTicks / 1000;
    summaries[implementation] = {
      samples: samples.length,
      kernelMedianMs,
      kernelMadMs: median(ticks.map(value => Math.abs(value / 1000 - kernelMedianMs))),
      kernelRangeMs: [Math.min(...ticks) / 1000, Math.max(...ticks) / 1000],
      coreMarkPerSecond: options.iterations / (medianTicks / 1e6),
      wallMedianMs: median(samples.map(sample => sample.wallMilliseconds)),
      durationQualified: samples.every(sample => sample.metrics.durationValid),
    };
  }
  const x86Ticks = summaries.x86Native.kernelMedianMs;
  const qemuTicks = summaries.qemuUser.kernelMedianMs;
  const dynarmicTicks = summaries.dynarmicNative.kernelMedianMs;
  return {
    schema: 'dynarmic.js/native-coremark-comparison-summary-v1',
    iterations: options.iterations,
    implementations: summaries,
    ratios: {
      qemuUserOverX86NativeTime: qemuTicks / x86Ticks,
      dynarmicNativeOverX86NativeTime: dynarmicTicks / x86Ticks,
      qemuUserOverDynarmicNativeTime: qemuTicks / dynarmicTicks,
      x86NativeSpeedupOverQemuUser: qemuTicks / x86Ticks,
      x86NativeSpeedupOverDynarmicNative: dynarmicTicks / x86Ticks,
      dynarmicNativeSpeedupOverQemuUser: qemuTicks / dynarmicTicks,
    },
    validFixedWorkloadComparison: true,
    durationQualified: observations.every(observation => observation.metrics.durationValid),
  };
}

function validateFourWayObservation(observation, sequence, options) {
  validateObservation(observation, sequence, options, fourWayImplementations, rotatingFourWayOrder);
  if (observation.implementation === 'dynarmicWasm') {
    assert.equal(observation.dynarmic, null, 'unexpected dynarmic diagnostics on Wasm');
    assert(observation.wasm && typeof observation.wasm === 'object', 'missing Wasm diagnostics');
    assert(Number.isFinite(observation.wasm.runMilliseconds) && observation.wasm.runMilliseconds > 0, 'invalid Wasm Run time');
    assert(observation.wasm.runMilliseconds + 0.01 >= observation.metrics.ticks / 1000, 'kernel time exceeds Wasm Run');
    assert(observation.wallMilliseconds + 0.01 >= observation.wasm.runMilliseconds, 'Wasm Run exceeds process wall time');
  } else {
    assert.equal(observation.wasm, null, 'unexpected Wasm diagnostics');
    if (observation.implementation === 'dynarmicNative') {
      assert(observation.dynarmic && typeof observation.dynarmic === 'object', 'missing dynarmic diagnostics');
    } else assert.equal(observation.dynarmic, null, 'unexpected dynarmic diagnostics');
  }
}

export function summarizeFourWayComparison(observations, options) {
  assert(Number.isInteger(options.iterations) && options.iterations > 0, 'invalid iterations');
  assert(Number.isInteger(options.runsPerImplementation) && options.runsPerImplementation > 0, 'invalid run count');
  assert.equal(observations.length, options.runsPerImplementation * fourWayImplementations.length, 'incorrect observation count');
  observations.forEach((observation, sequence) => validateFourWayObservation(observation, sequence, options));
  const summaries = {};
  for (const implementation of fourWayImplementations) {
    const samples = observations.filter(observation => observation.implementation === implementation);
    assert.equal(samples.length, options.runsPerImplementation, `incorrect ${implementation} sample count`);
    const ticks = samples.map(sample => sample.metrics.ticks);
    const medianTicks = median(ticks);
    const kernelMedianMs = medianTicks / 1000;
    summaries[implementation] = {
      samples: samples.length,
      kernelMedianMs,
      kernelMadMs: median(ticks.map(value => Math.abs(value / 1000 - kernelMedianMs))),
      kernelRangeMs: [Math.min(...ticks) / 1000, Math.max(...ticks) / 1000],
      coreMarkPerSecond: options.iterations / (medianTicks / 1e6),
      wallMedianMs: median(samples.map(sample => sample.wallMilliseconds)),
      durationQualified: samples.every(sample => sample.metrics.durationValid),
    };
  }
  const x86Ticks = summaries.x86Native.kernelMedianMs;
  const qemuTicks = summaries.qemuUser.kernelMedianMs;
  const dynarmicTicks = summaries.dynarmicNative.kernelMedianMs;
  const wasmTicks = summaries.dynarmicWasm.kernelMedianMs;
  return {
    schema: 'dynarmic.js/native-coremark-comparison-summary-v2',
    iterations: options.iterations,
    implementations: summaries,
    ratios: {
      qemuUserOverX86NativeTime: qemuTicks / x86Ticks,
      dynarmicNativeOverX86NativeTime: dynarmicTicks / x86Ticks,
      qemuUserOverDynarmicNativeTime: qemuTicks / dynarmicTicks,
      x86NativeSpeedupOverQemuUser: qemuTicks / x86Ticks,
      x86NativeSpeedupOverDynarmicNative: dynarmicTicks / x86Ticks,
      dynarmicNativeSpeedupOverQemuUser: qemuTicks / dynarmicTicks,
      dynarmicWasmOverX86NativeTime: wasmTicks / x86Ticks,
      dynarmicWasmOverQemuUserTime: wasmTicks / qemuTicks,
      dynarmicWasmOverDynarmicNativeTime: wasmTicks / dynarmicTicks,
      x86NativeSpeedupOverDynarmicWasm: wasmTicks / x86Ticks,
      qemuUserSpeedupOverDynarmicWasm: wasmTicks / qemuTicks,
      dynarmicNativeSpeedupOverDynarmicWasm: wasmTicks / dynarmicTicks,
    },
    validFixedWorkloadComparison: true,
    durationQualified: observations.every(observation => observation.metrics.durationValid),
  };
}
