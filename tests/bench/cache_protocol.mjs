import assert from 'node:assert/strict';
import {profileCounters} from './protocol.mjs';
import {validateEnvironment} from './environment.mjs';

export const cacheCases = [
  {name: 'cache-thunk', memoryMode: 'thunk', argv: ['0']},
  {name: 'cache-page-table', memoryMode: 'page-table', argv: ['1']},
  {name: 'cache-flat-window', memoryMode: 'flat-window', argv: ['2']},
];

export const cacheMethodology = {
  cycles: 8,
  workload: 'Each cycle patches one A32 instruction, invalidates its range, and reruns it.',
  timing: 'Each cycle wall time includes code-cache invalidation handling and recompilation.',
  comparison: 'Compare only equal memory modes, build identity and cycle count.',
};

const compileKeys = ['blocks', 'bytes', 'translateMs', 'optMs', 'emitMs', 'compileMs', 'instMs', 'tableMs'];
const sha256 = value => assert(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value), 'invalid SHA256');

export function validateCacheSample(sample, profileRequired) {
  assert(Number.isSafeInteger(sample.cycle) && sample.cycle >= 0 && sample.cycle < cacheMethodology.cycles,
    'invalid cache cycle');
  assert(Number.isFinite(sample.ms) && sample.ms > 0, 'invalid cache timing');
  assert.equal(sample.value, sample.cycle + 1, 'cache guest result mismatch');
  for (const key of compileKeys) {
    assert(Number.isFinite(sample.compile?.[key]) && sample.compile[key] >= 0, `invalid compile.${key}`);
  }
  assert(Number.isSafeInteger(sample.compile.blocks) && Number.isSafeInteger(sample.compile.bytes),
    'invalid compiled module counters');
  assert(sample.compile.blocks > 0 && sample.compile.bytes > 0, 'cache cycle did not compile');
  if (profileRequired) {
    assert(sample.profile && sample.profile.schema === 'dynarmic.js/profile-v1');
    assert.equal(sample.profile.enabled, true);
    for (const key of profileCounters) {
      assert(Number.isSafeInteger(sample.profile[key]) && sample.profile[key] >= 0, `invalid profile.${key}`);
    }
    assert.equal(sample.profile.runCount, 1, 'cache cycle must execute one Run');
    assert.equal(sample.profile.stepCount, 0, 'cache cycle unexpectedly stepped');
    assert.equal(sample.profile.compiledBlocks, sample.compile.blocks, 'cache module count mismatch');
    assert.equal(sample.profile.emittedBytes, sample.compile.bytes, 'cache byte count mismatch');
    if (sample.cycle === 0) {
      assert.equal(sample.profile.invalidationCount, 0, 'cold cache cycle invalidated unexpectedly');
      assert.equal(sample.profile.invalidatedBlocks, 0, 'cold cache cycle dropped blocks');
      assert(sample.profile.peakCachedBlocks >= sample.compile.blocks);
      assert(sample.profile.peakCachedBytes >= sample.compile.bytes);
    } else {
      assert.equal(sample.profile.invalidationCount, 1, 'missing range invalidation');
      assert(sample.profile.invalidatedBlocks > 0, 'range invalidation dropped no modules');
    }
  } else {
    assert.equal(sample.profile, null, 'production cache report unexpectedly has profile');
  }
  return sample;
}

function median(values) {
  assert(values.length > 0);
  const sorted = [...values].sort((a, b) => a - b);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
}

export function summarizeCache(samples, profileRequired) {
  return {
    cycleMedianMs: Array.from({length: cacheMethodology.cycles}, (_, cycle) => median(
      samples.filter(s => s.cycle === cycle).map(s => s.ms))),
    totalMedianMs: median([...new Set(samples.map(s => s.process))].map(process =>
      samples.filter(s => s.process === process).reduce((sum, s) => sum + s.ms, 0))),
    compileBytesMedian: Array.from({length: cacheMethodology.cycles}, (_, cycle) => median(
      samples.filter(s => s.cycle === cycle).map(s => s.compile.bytes))),
    peakCachedBytesMedian: profileRequired ? median(samples.filter(s => s.cycle === 0)
      .map(s => s.profile.peakCachedBytes)) : null,
  };
}

export function validateCacheReport(report, requireProfile = false) {
  assert.equal(report.schema, 'dynarmic.js/cache-benchmark-v1');
  validateEnvironment(report);
  assert(typeof report.runtime.cpuAffinity === 'string' && report.runtime.cpuAffinity.length > 0, 'missing CPU affinity');
  assert.deepEqual(report.methodology, cacheMethodology);
  assert.equal(typeof report.build?.profile, 'boolean');
  if (requireProfile) assert.equal(report.build.profile, true, 'profile build required');
  assert.equal(report.cases.length, cacheCases.length);
  for (const spec of cacheCases) {
    const item = report.cases.find(candidate => candidate.name === spec.name);
    assert(item, `missing case ${spec.name}`);
    assert.equal(item.architecture, 'a32');
    assert.equal(item.memoryMode, spec.memoryMode);
    sha256(item.workloadSha256);
    sha256(item.artifact?.jsSha256);
    sha256(item.artifact?.wasmSha256);
    assert(Number.isInteger(item.runs) && item.runs > 0 && item.runs <= 100);
    assert.equal(item.samples.length, item.runs * cacheMethodology.cycles);
    for (let process = 0; process < item.runs; ++process) {
      const processSamples = item.samples.filter(s => s.process === process);
      assert.equal(processSamples.length, cacheMethodology.cycles);
      assert.deepEqual(processSamples.map(s => s.cycle), [...Array(cacheMethodology.cycles).keys()]);
      processSamples.forEach(sample => validateCacheSample(sample, report.build.profile));
    }
    assert.deepEqual(item.summary, summarizeCache(item.samples, report.build.profile));
  }
  return report;
}
