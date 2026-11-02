import {test} from 'node:test';
import assert from 'node:assert/strict';
import {implementations, fourWayImplementations, parseDynarmicDiagnostics, rotatingOrder, rotatingFourWayOrder, summarizeComparison, summarizeFourWayComparison} from './compare.mjs';

const crc = {seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'};
function observation(sequence, ticks) {
  const round = Math.floor(sequence / implementations.length);
  const implementation = rotatingOrder(round)[sequence % implementations.length];
  return {
    sequence, round, implementation, exitCode: 0, signal: null, timedOut: false,
    wallMilliseconds: ticks / 1000 + 10,
    metrics: {
      ticks, seconds: ticks / 1e6, iterationsPerSecond: 200000 / (ticks / 1e6), crc: {...crc},
      durationValid: ticks >= 10000000, compilerVersion: 'test compiler', compilerFlags: '-O2',
    },
  };
}
function cohort(ticks = {x86Native: 10000000, qemuUser: 200000000, dynarmicNative: 50000000}) {
  return Array.from({length: 15}, (_, sequence) => {
    const round = Math.floor(sequence / implementations.length);
    return observation(sequence, ticks[rotatingOrder(round)[sequence % implementations.length]]);
  });
}

test('comparison rotates implementations and derives median-based ratios', () => {
  assert.deepEqual(rotatingOrder(0), ['x86Native', 'qemuUser', 'dynarmicNative']);
  assert.deepEqual(rotatingOrder(1), ['qemuUser', 'dynarmicNative', 'x86Native']);
  assert.deepEqual(rotatingOrder(2), ['dynarmicNative', 'x86Native', 'qemuUser']);
  const report = summarizeComparison(cohort(), {iterations: 200000, runsPerImplementation: 5});
  assert.equal(report.implementations.x86Native.kernelMedianMs, 10000);
  assert.equal(report.implementations.x86Native.coreMarkPerSecond, 20000);
  assert.equal(report.implementations.qemuUser.kernelMedianMs, 200000);
  assert.equal(report.implementations.dynarmicNative.kernelMedianMs, 50000);
  assert.equal(report.ratios.qemuUserOverX86NativeTime, 20);
  assert.equal(report.ratios.dynarmicNativeOverX86NativeTime, 5);
  assert.equal(report.ratios.qemuUserOverDynarmicNativeTime, 4);
  assert.equal(report.ratios.dynarmicNativeSpeedupOverQemuUser, 4);
  assert.equal(report.durationQualified, true);
});

test('comparison uses per-implementation medians and MAD without mutating samples', () => {
  const samples = cohort();
  samples[3].metrics.ticks = 210000000;
  samples[3].metrics.seconds = 210;
  samples[3].metrics.iterationsPerSecond = 200000 / 210;
  samples[3].wallMilliseconds = 210010;
  const before = structuredClone(samples);
  const report = summarizeComparison(samples, {iterations: 200000, runsPerImplementation: 5});
  assert.deepEqual(samples, before);
  assert.equal(report.implementations.qemuUser.kernelMedianMs, 200000);
  assert.equal(report.implementations.qemuUser.kernelMadMs, 0);
  assert.equal(report.implementations.qemuUser.kernelRangeMs[1], 210000);
});

test('native dynarmic diagnostics are complete and uniquely matched', () => {
  const stderr = 'DYNARMIC_GUEST_BYTES=196620\nDYNARMIC_MEMORY_MODE=2\n' +
    'DYNARMIC_RUN_MS=18161.604196\nDYNARMIC_HALT_REASON=0x01000000\n';
  assert.deepEqual(parseDynarmicDiagnostics(stderr), {
    memoryMode: 2, guestBytes: 196620, runMilliseconds: 18161.604196, haltReason: '01000000',
  });
  assert.throws(() => parseDynarmicDiagnostics(stderr + 'DYNARMIC_RUN_MS=1\n'), /duplicate/);
  assert.throws(() => parseDynarmicDiagnostics(stderr + 'unrecorded diagnostic\n'), /unexpected dynarmic stderr/);
  assert.throws(() => parseDynarmicDiagnostics(stderr.replace('DYNARMIC_MEMORY_MODE=2', 'DYNARMIC_MEMORY_MODE=3')), /memory mode/);
  assert.throws(() => parseDynarmicDiagnostics(stderr.replace('0x01000000', '0x02000000')), /halt reason/);
});

for (const [name, change, pattern] of [
  ['wrong final CRC', s => { s[0].metrics.crc.crcfinal = '1234'; }, /crcfinal/],
  ['wrong individual CRC', s => { s[0].metrics.crc.crclist = '1234'; }, /crclist/],
  ['missing process', s => { s.pop(); }, /observation count|sequence/],
  ['out-of-order process', s => { [s[0], s[1]] = [s[1], s[0]]; }, /sequence|implementation/],
  ['failed process', s => { s[0].exitCode = 1; }, /exitCode/],
  ['timeout', s => { s[0].timedOut = true; }, /timedOut/],
  ['wrong iteration count', s => { s[0].metrics.iterationsPerSecond = 100; }, /throughput/],
  ['inconsistent timer', s => { s[0].metrics.seconds = 1; }, /seconds/],
  ['invalid wall time', s => { s[0].wallMilliseconds = 0; }, /wallMilliseconds/],
]) test(`comparison rejects ${name}`, () => {
  const samples = cohort(); change(samples);
  assert.throws(() => summarizeComparison(samples, {iterations: 200000, runsPerImplementation: 5}), pattern);
});

function fourWayObservation(sequence, ticks) {
  const round = Math.floor(sequence / fourWayImplementations.length);
  const implementation = rotatingFourWayOrder(round)[sequence % fourWayImplementations.length];
  const base = observation(sequence, ticks);
  return {
    ...base, round, implementation,
    dynarmic: implementation === 'dynarmicNative' ? {memoryMode: 2, guestBytes: 100,
      runMilliseconds: ticks / 1000 + 5, haltReason: '01000000'} : null,
    wasm: implementation === 'dynarmicWasm' ? {architecture: 'a64', memoryMode: 2, guestBytes: 100,
      haltReason: '01000000', runMilliseconds: ticks / 1000 + 5, compile: {}, profile: null} : null,
  };
}
function fourWayCohort(ticks = {x86Native: 10000000, qemuUser: 200000000,
  dynarmicNative: 50000000, dynarmicWasm: 100000000}) {
  return Array.from({length: 20}, (_, sequence) => {
    const round = Math.floor(sequence / fourWayImplementations.length);
    return fourWayObservation(sequence, ticks[rotatingFourWayOrder(round)[sequence % fourWayImplementations.length]]);
  });
}

test('four-way comparison rotates all implementations and derives v2 ratios', () => {
  assert.deepEqual(implementations, ['x86Native', 'qemuUser', 'dynarmicNative']);
  assert.deepEqual(fourWayImplementations, ['x86Native', 'qemuUser', 'dynarmicNative', 'dynarmicWasm']);
  assert.deepEqual(rotatingFourWayOrder(0), fourWayImplementations);
  assert.deepEqual(rotatingFourWayOrder(1), ['qemuUser', 'dynarmicNative', 'dynarmicWasm', 'x86Native']);
  assert.deepEqual(rotatingFourWayOrder(2), ['dynarmicNative', 'dynarmicWasm', 'x86Native', 'qemuUser']);
  assert.deepEqual(rotatingFourWayOrder(3), ['dynarmicWasm', 'x86Native', 'qemuUser', 'dynarmicNative']);
  const samples = fourWayCohort();
  const before = structuredClone(samples);
  const report = summarizeFourWayComparison(samples, {iterations: 200000, runsPerImplementation: 5});
  assert.deepEqual(samples, before);
  assert.equal(report.schema, 'dynarmic.js/native-coremark-comparison-summary-v2');
  assert.equal(report.implementations.dynarmicWasm.kernelMedianMs, 100000);
  assert.equal(report.implementations.dynarmicWasm.coreMarkPerSecond, 2000);
  assert.equal(report.ratios.dynarmicWasmOverX86NativeTime, 10);
  assert.equal(report.ratios.dynarmicWasmOverQemuUserTime, 0.5);
  assert.equal(report.ratios.dynarmicWasmOverDynarmicNativeTime, 2);
  assert.equal(report.ratios.x86NativeSpeedupOverDynarmicWasm, 10);
  assert.equal(report.ratios.qemuUserSpeedupOverDynarmicWasm, 0.5);
  assert.equal(report.ratios.dynarmicNativeSpeedupOverDynarmicWasm, 2);
  assert.equal(report.ratios.dynarmicNativeSpeedupOverQemuUser, 4);
});

for (const [name, change, pattern] of [
  ['missing Wasm process', s => { s.pop(); }, /observation count|sequence/],
  ['misordered Wasm process', s => { [s[2], s[3]] = [s[3], s[2]]; }, /sequence|implementation/],
  ['missing Wasm diagnostics', s => { s[3].wasm = null; }, /Wasm diagnostics/],
  ['unexpected Wasm diagnostics', s => { s[0].wasm = {}; }, /unexpected Wasm diagnostics/],
  ['mixed native and Wasm diagnostics', s => { s[3].dynarmic = {}; }, /unexpected dynarmic diagnostics/],
  ['Wasm Run below kernel', s => { s[3].wasm.runMilliseconds = 1; }, /kernel time exceeds Wasm Run/],
  ['wall below Wasm Run', s => { s[3].wallMilliseconds = 1; }, /kernel time exceeds process wall|Wasm Run exceeds process wall/],
]) test(`four-way comparison rejects ${name}`, () => {
  const samples = fourWayCohort(); change(samples);
  assert.throws(() => summarizeFourWayComparison(samples, {iterations: 200000, runsPerImplementation: 5}), pattern);
});
