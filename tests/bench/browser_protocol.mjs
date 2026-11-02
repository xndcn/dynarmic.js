import assert from 'node:assert/strict';
import {median} from './protocol.mjs';

export const browserCases = [
  ['arm32', 'a32'], ['aarch64', 'a64'],
];
export const browserKernels = ['lcg', 'sieve', 'pi'];

export function summarizeBrowser(samples) {
  return {
    totalMinMs: Math.min(...samples.map(row => row.total)),
    totalMedianMs: median(samples.map(row => row.total)),
    kernelMinMs: Object.fromEntries(browserKernels.map((kernel, i) =>
      [kernel, Math.min(...samples.map(row => row.kernels[i]))])),
    kernelMedianMs: Object.fromEntries(browserKernels.map((kernel, i) =>
      [kernel, median(samples.map(row => row.kernels[i]))])),
  };
}

export function validateBrowserReport(report) {
  assert.equal(report.schema, 'dynarmic.js/browser-benchmark-v1');
  assert(typeof report.timestamp === 'string' && Number.isFinite(Date.parse(report.timestamp)));
  assert(typeof report.node === 'string' && report.node.length > 0);
  assert(typeof report.v8 === 'string' && report.v8.length > 0);
  for (const key of ['armJitSha256', 'wasmSha256', 'demoA32Sha256', 'demoA64Sha256']) {
    assert(typeof report.source?.[key] === 'string' && /^[a-f0-9]{64}$/.test(report.source[key]), `invalid source.${key}`);
  }
  assert.deepEqual(Object.keys(report.kernels).sort(), ['lcg', 'pi', 'sieve']);
  const expected = browserCases.flatMap(([name, arch]) =>
    ['flat-window', 'thunk'].map(mode => `${name}:${arch}:${mode}`));
  assert.deepEqual(report.cases.map(item => `${item.name}:${item.architecture}:${item.memoryMode}`).sort(),
    expected.sort(), 'missing, duplicate or unknown browser case');
  for (const item of report.cases) {
    assert(['a32', 'a64'].includes(item.architecture));
    assert(['thunk', 'flat-window'].includes(item.memoryMode));
    assert(Number.isInteger(item.runs) && item.runs > 0 && item.runs <= 100);
    assert.equal(item.samples.length, item.runs);
    for (const sample of item.samples) {
      assert.equal(sample.kernels.length, 3);
      assert.equal(sample.correct, true, 'guest correctness failure');
      assert(Number.isFinite(sample.total) && sample.total > 0);
      sample.kernels.forEach(ms => assert(Number.isFinite(ms) && ms > 0));
      assert(sample.kernels.reduce((sum, ms) => sum + ms, 0) <= sample.total + 1e-6,
        'kernel times exceed total Run time');
    }
    for (const key of ['totalMinMs', 'totalMedianMs']) assert(Number.isFinite(item.summary[key]) && item.summary[key] > 0);
    for (const key of ['kernelMinMs', 'kernelMedianMs']) {
      assert.deepEqual(Object.keys(item.summary[key]).sort(), ['lcg', 'pi', 'sieve']);
      Object.values(item.summary[key]).forEach(ms => assert(Number.isFinite(ms) && ms > 0));
    }
    assert.deepEqual(item.summary, summarizeBrowser(item.samples), 'incorrect browser summary');
  }
  return report;
}
