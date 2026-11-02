import assert from 'node:assert/strict';
import {parseCoremark} from '../../../../tests/bench/coremark_protocol.mjs';
import {compileTimers} from '../../../../tests/bench/protocol.mjs';

const prefix = 'COREMARK_SAMPLE ';
const sampleKeys = ['compile', 'haltReason', 'profile', 'runMs', 'stdout'];
const compileKeys = ['blocks', 'bytes', ...compileTimers];
const plainObject = value => value !== null && typeof value === 'object' && !Array.isArray(value) &&
  [Object.prototype, null].includes(Object.getPrototypeOf(value));

export function parseWasmCoremarkSample(stdout, iterations, guestBytes) {
  assert(typeof stdout === 'string', 'missing host stdout');
  assert(Number.isSafeInteger(guestBytes) && guestBytes > 0, 'invalid guest size');
  const lines = stdout.split(/\r?\n/);
  const rows = lines.map((line, index) => line.startsWith(prefix) ? index : -1).filter(index => index >= 0);
  assert.equal(rows.length, 1, 'missing/duplicate guest sample');
  lines.forEach((line, index) => assert(index === rows[0] || line.trim() === '', 'unexpected host stdout'));
  const sample = JSON.parse(lines[rows[0]].slice(prefix.length));
  assert(plainObject(sample), 'invalid sample shape');
  assert(!Object.hasOwn(sample, 'regionDiagnostics'), 'unexpected regionDiagnostics');
  assert(!Object.hasOwn(sample, 'regionTransferDiagnostics'), 'unexpected regionTransferDiagnostics');
  assert.deepEqual(Object.keys(sample).sort(), sampleKeys, 'invalid sample shape');
  assert.equal(sample.haltReason, 0x01000000, 'abnormal halt reason');
  assert(Number.isFinite(sample.runMs) && sample.runMs > 0 && sample.runMs < 4294967.296,
    'invalid Run time or timer-wrap ambiguity');
  assert.equal(sample.profile, null, 'production sample must have a null profile');
  assert(plainObject(sample.compile), 'invalid compile statistics');
  assert.deepEqual(Object.keys(sample.compile).sort(), [...compileKeys].sort(), 'invalid compile statistics shape');
  for (const key of compileKeys) {
    assert(Number.isFinite(sample.compile[key]) && sample.compile[key] >= 0, `invalid compile.${key}`);
  }
  for (const key of ['blocks', 'bytes']) {
    assert(Number.isSafeInteger(sample.compile[key]) && sample.compile[key] > 0, `invalid compile.${key}`);
  }
  assert(compileTimers.reduce((sum, key) => sum + sample.compile[key], 0) <= sample.runMs + 0.01,
    'compile phases exceed Run');
  const metrics = parseCoremark(sample.stdout, iterations);
  assert.equal(metrics.crc.crcfinal, '4983', 'wrong crcfinal');
  return {
    metrics,
    wasm: {
      architecture: 'a64', memoryMode: 2, guestBytes, haltReason: '01000000',
      runMilliseconds: sample.runMs, compile: {...sample.compile}, profile: null,
    },
  };
}
