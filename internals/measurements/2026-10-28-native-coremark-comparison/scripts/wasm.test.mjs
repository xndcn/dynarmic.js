import {test} from 'node:test';
import assert from 'node:assert/strict';
import {compileTimers} from '../../../../tests/bench/protocol.mjs';
import {parseWasmCoremarkSample} from './wasm.mjs';

const iterations = 200000;
const ticks = 10000000;
const compile = {blocks: 123, bytes: 45678, translateMs: 1.25, optMs: 2.5,
  emitMs: 3.75, compileMs: 4, instMs: 0, tableMs: 0.5};
const crcLines = 'seedcrc          : 0xe9f5\n[0]crclist       : 0xe714\n[0]crcmatrix     : 0x1fd7\n' +
  '[0]crcstate      : 0x8e3a\n[0]crcfinal      : 0x4983\n';
function coremarkStdout() {
  const seconds = (ticks / 1e6).toFixed(6);
  const rate = (iterations / (ticks / 1e6)).toFixed(6);
  return '2K performance run parameters for coremark.\nCoreMark Size    : 666\n' +
    `Total ticks      : ${ticks}\nTotal time (secs): ${seconds}\nIterations/Sec   : ${rate}\n` +
    `Iterations       : ${iterations}\nCompiler version : clang test\nCompiler flags   : -O2\n` +
    `Memory location  : STACK\n${crcLines}` +
    `Correct operation validated. See README.md for run and reporting rules.\nCoreMark 1.0 : ${rate} / clang test -O2 / STACK\n`;
}
function sample() {
  return {haltReason: 0x01000000, runMs: 10123.25, stdout: coremarkStdout(), compile: {...compile}, profile: null};
}
function wrapper(value = sample()) {
  return `COREMARK_SAMPLE ${JSON.stringify(value)}\n`;
}
function changed(change) {
  const value = sample();
  change(value);
  return wrapper(value);
}

test('WASM sample parses nested CoreMark output and normalizes production metadata', () => {
  const result = parseWasmCoremarkSample(`\n\t\n${wrapper()}\r\n`, iterations, 196620);
  assert.deepEqual(result.metrics, {
    ticks, seconds: 10, iterationsPerSecond: 20000,
    crc: {seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'},
    durationValid: true, compilerVersion: 'clang test', compilerFlags: '-O2',
  });
  assert.deepEqual(result.wasm, {
    architecture: 'a64', memoryMode: 2, guestBytes: 196620, haltReason: '01000000',
    runMilliseconds: 10123.25, compile, profile: null,
  });
  assert.deepEqual(Object.keys(result.wasm.compile).sort(), ['blocks', 'bytes', ...compileTimers].sort());
});

for (const [name, stdout, pattern] of [
  ['missing wrapper', `${JSON.stringify(sample())}\n`, /missing\/duplicate guest sample/],
  ['empty host stdout', ' \n\t\n', /missing\/duplicate guest sample/],
  ['duplicate wrapper', `${wrapper()}${wrapper()}`, /missing\/duplicate guest sample/],
  ['malformed wrapper JSON', 'COREMARK_SAMPLE {not json}\n', /JSON|Unexpected|Expected/],
  ['extra host stdout', `host diagnostic\n${wrapper()}`, /unexpected host stdout/],
  ['trailing host stdout', `${wrapper()}host diagnostic\n`, /unexpected host stdout/],
]) test(`WASM sample rejects ${name}`, () => {
  assert.throws(() => parseWasmCoremarkSample(stdout, iterations, 196620), pattern);
});

test('WASM sample rejects non-string host stdout', () => {
  assert.throws(() => parseWasmCoremarkSample(null, iterations, 196620), /host stdout/);
});

for (const [name, value] of [
  ['null sample', null],
  ['array sample', []],
  ['missing nested stdout', {haltReason: 0x01000000, runMs: 1, compile, profile: null}],
  ['extra sample field', {...sample(), unexpected: true}],
]) test(`WASM sample rejects ${name}`, () => {
  assert.throws(() => parseWasmCoremarkSample(wrapper(value), iterations, 196620), /sample shape/);
});

for (const [name, change, pattern] of [
  ['abnormal halt reason', value => { value.haltReason = 0x02000000; }, /halt reason/],
  ['string halt reason', value => { value.haltReason = '01000000'; }, /halt reason/],
  ['zero Run time', value => { value.runMs = 0; }, /Run time/],
  ['negative Run time', value => { value.runMs = -1; }, /Run time/],
  ['non-numeric Run time', value => { value.runMs = '10123.25'; }, /Run time/],
  ['non-finite Run time', value => { value.runMs = Infinity; }, /Run time/],
  ['timer-wrap ambiguous Run time', value => { value.runMs = 4294967.296; }, /Run time/],
  ['production profile', value => { value.profile = {enabled: true}; }, /profile/],
  ['region diagnostics', value => { value.regionDiagnostics = {regions: []}; }, /regionDiagnostics/],
  ['region transfer diagnostics', value => { value.regionTransferDiagnostics = {regions: []}; }, /regionTransferDiagnostics/],
  ['wrong nested CRC', value => { value.stdout = value.stdout.replace('0xe714', '0x1234'); }, /crclist/],
  ['wrong final CRC', value => { value.stdout = value.stdout.replace('0x4983', '0x1234'); }, /crcfinal/],
]) test(`WASM sample rejects ${name}`, () => {
  assert.throws(() => parseWasmCoremarkSample(changed(change), iterations, 196620), pattern);
});

for (const guestBytes of [0, -1, 1.5, Number.MAX_SAFE_INTEGER + 1, '196620', null, undefined]) {
  test(`WASM sample rejects invalid guest size ${String(guestBytes)}`, () => {
    assert.throws(() => parseWasmCoremarkSample(wrapper(), iterations, guestBytes), /guest size/);
  });
}

for (const [name, value] of [
  ['null', null],
  ['array', []],
  ['missing statistic', Object.fromEntries(Object.entries(compile).filter(([key]) => key !== 'emitMs'))],
  ['extra statistic', {...compile, unexpected: 0}],
  ['negative leaf', {...compile, optMs: -1}],
  ['non-numeric leaf', {...compile, bytes: '45678'}],
  ['non-finite leaf', {...compile, compileMs: Infinity}],
  ['zero blocks', {...compile, blocks: 0}],
  ['fractional blocks', {...compile, blocks: 1.5}],
  ['unsafe bytes', {...compile, bytes: Number.MAX_SAFE_INTEGER + 1}],
  ['zero bytes', {...compile, bytes: 0}],
  ['phases exceeding Run', {...compile, compileMs: 20000}],
]) test(`WASM sample rejects ${name} compile statistics`, () => {
  assert.throws(() => parseWasmCoremarkSample(changed(sample => { sample.compile = value; }),
    iterations, 196620), /compile/);
});
