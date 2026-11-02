import assert from 'node:assert/strict';
import {validateEnvironment} from './environment.mjs';

export const cases = [
  {name: 'memory-thunk', architecture: 'a32', memoryMode: 'thunk', file: 'jit_bench.js', argv: ['0']},
  {name: 'memory-page-table', architecture: 'a32', memoryMode: 'page-table', file: 'jit_bench.js', argv: ['1']},
  {name: 'memory-flat-window', architecture: 'a32', memoryMode: 'flat-window', file: 'jit_bench.js', argv: ['2']},
  {name: 'call-return', architecture: 'a32', memoryMode: 'thunk', file: 'jit_callbench.js', argv: []},
  {name: 'a64-memory-thunk', architecture: 'a64', memoryMode: 'thunk', file: 'jit_a64_bench.js', argv: ['0']},
  {name: 'a64-memory-page-table', architecture: 'a64', memoryMode: 'page-table', file: 'jit_a64_bench.js', argv: ['1']},
  {name: 'a64-memory-flat-window', architecture: 'a64', memoryMode: 'flat-window', file: 'jit_a64_bench.js', argv: ['2']},
  {name: 'a64-neon', architecture: 'a64', memoryMode: 'none', file: 'jit_a64_simd_bench.js', argv: ['neon']},
  {name: 'a64-scalar', architecture: 'a64', memoryMode: 'none', file: 'jit_a64_simd_bench.js', argv: ['scalar']},
  {name: 'thumb-memory-thunk', architecture: 'thumb', memoryMode: 'thunk', file: 'jit_thumb_bench.js', argv: ['0']},
  {name: 'thumb-memory-page-table', architecture: 'thumb', memoryMode: 'page-table', file: 'jit_thumb_bench.js', argv: ['1']},
  {name: 'thumb-memory-flat-window', architecture: 'thumb', memoryMode: 'flat-window', file: 'jit_thumb_bench.js', argv: ['2']},
];
export const compileTimers = ['translateMs', 'optMs', 'emitMs', 'compileMs', 'instMs', 'tableMs'];
export const profileCounters = ['runCount', 'stepCount', 'dispatchCount', 'dispatchCacheHits', 'dispatchCacheMisses',
  'compiledBlocks', 'compiledRegions', 'regionBlocks', 'emittedBytes', 'linkPatches',
  'fastDispatchMisses', 'invalidationCount', 'invalidatedBlocks', 'cycleBudgetExits',
  'regionCandidates', 'regionRejectedIncompatible', 'regionRejectedDistance',
  'regionRejectedBlockBudget', 'regionRejectedIrBudget',
  'peakCachedBlocks', 'peakCachedBytes'];
export const methodology = {
  cold: 'first Run of a new JIT in a fresh process',
  warm: 'eight subsequent Runs of the same JIT; includes engine tiering; all retained',
  warmRunsPerProcess: 8,
  sampleIndex: 'runIndex is 0 for cold and 1..8 for warm Runs in execution order',
  tiering: {backgroundOptimizationIncluded: true, observableEngineEvent: false,
    eventReason: 'Node/V8 exposes no portable completion event for inner WASM module optimization'},
  aggregation: 'median of process warm medians; cold phase totals exclude guest execution',
  timing: 'Run wall time includes profile snapshot if enabled; excludes reset/validation',
};

export function median(values) {
  assert(values.length > 0);
  const sorted = [...values].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

export function validateSample(sample) {
  assert(['cold', 'warm'].includes(sample.phase), 'invalid phase');
  assert(Number.isSafeInteger(sample.runIndex) && sample.runIndex >= 0 && sample.runIndex <= 8,
    'invalid run index');
  assert.equal(sample.phase, sample.runIndex === 0 ? 'cold' : 'warm', 'run index/phase mismatch');
  for (const key of ['ms', 'instructions', 'minstrPerSec']) {
    assert(Number.isFinite(sample[key]) && sample[key] > 0, `invalid ${key}`);
  }
  assert(Number.isSafeInteger(sample.instructions), 'invalid instruction count');
  assert(Math.abs(sample.minstrPerSec - sample.instructions / sample.ms / 1000) < 1e-6, 'inconsistent throughput');
  for (const key of ['blocks', 'bytes', ...compileTimers]) {
    assert(Number.isFinite(sample.compile?.[key]) && sample.compile[key] >= 0, `invalid compile.${key}`);
    if (sample.phase === 'warm') assert.equal(sample.compile[key], 0, `warm run changed compile.${key}`);
  }
  for (const key of ['blocks', 'bytes']) assert(Number.isSafeInteger(sample.compile[key]), `invalid compile.${key}`);
  if (sample.phase === 'cold') assert(sample.compile.blocks > 0 && sample.compile.bytes > 0, 'cold run did not compile');
  if (sample.profile !== null) {
    assert.equal(sample.profile?.schema, 'dynarmic.js/profile-v1');
    assert.equal(sample.profile.enabled, true);
    for (const key of profileCounters) {
      assert(Number.isSafeInteger(sample.profile[key]) && sample.profile[key] >= 0, `invalid profile.${key}`);
    }
    assert.equal(sample.profile.compiledBlocks, sample.compile.blocks, 'module count mismatch');
    assert.equal(sample.profile.emittedBytes, sample.compile.bytes, 'emitted byte mismatch');
    assert.equal(sample.profile.runCount, 1, 'one Run per sample required');
    assert.equal(sample.profile.stepCount, 0, 'unexpected Step in benchmark');
    if (sample.phase === 'cold') {
      assert(sample.profile.peakCachedBlocks >= sample.profile.compiledBlocks,
        'cache block peak below cold compilation');
      assert(sample.profile.peakCachedBytes >= sample.profile.emittedBytes,
        'cache byte peak below cold compilation');
    } else {
      assert.equal(sample.profile.peakCachedBlocks, 0, 'warm run changed cache block peak');
      assert.equal(sample.profile.peakCachedBytes, 0, 'warm run changed cache byte peak');
    }
  }
  return sample;
}

function validateProcess(samples) {
  assert.equal(samples.length, 9, 'missing samples');
  assert.equal(samples[0].phase, 'cold');
  assert.deepEqual(samples.map(s => s.runIndex), [0, 1, 2, 3, 4, 5, 6, 7, 8], 'invalid run order');
  assert(samples.slice(1).every(s => s.phase === 'warm'), 'invalid sample order');
  assert(samples.every(s => s.instructions === samples[0].instructions), 'instruction count changed');
  assert(samples.every(s => (s.profile === null) === (samples[0].profile === null)), 'mixed profiling modes');
}

export function parseHostOutput(stdout) {
  assert(!/WRONG|FAIL/.test(stdout), 'guest correctness failure');
  assert(stdout.split('\n').includes('PASS'), 'missing guest PASS marker');
  const samples = stdout.split('\n').filter(line => line.startsWith('BENCHMARK '))
    .map(line => validateSample(JSON.parse(line.slice(10))));
  validateProcess(samples);
  return samples;
}

export function summarize(samples) {
  const cold = samples.filter(s => s.phase === 'cold');
  const processes = [...new Set(samples.map(s => s.process))];
  const warmMedians = processes.map(p => median(samples.filter(s => s.process === p && s.phase === 'warm').map(s => s.ms)));
  const warmMedianMs = median(warmMedians);
  const warmIndexMedianMs = Array.from({length: 8}, (_, i) => median(
    samples.filter(s => s.phase === 'warm' && s.runIndex === i + 1).map(s => s.ms)));
  return {
    coldMedianMs: median(cold.map(s => s.ms)),
    coldCompileMedianMs: median(cold.map(s => compileTimers.reduce((sum, key) => sum + s.compile[key], 0))),
    emittedBytesMedian: median(cold.map(s => s.compile.bytes)),
    warmMedianMs, warmMinMs: Math.min(...samples.filter(s => s.phase === 'warm').map(s => s.ms)),
    warmProcessMadMs: median(warmMedians.map(ms => Math.abs(ms - warmMedianMs))),
    warmIndexMedianMs,
    warmMinstrPerSec: samples[0].instructions / warmMedianMs / 1000,
    peakCachedBlocksMedian: samples[0].profile === null ? null : median(
      cold.map(s => s.profile.peakCachedBlocks)),
    peakCachedBytesMedian: samples[0].profile === null ? null : median(
      cold.map(s => s.profile.peakCachedBytes)),
  };
}

function sha256(value) { assert(/^[a-f0-9]{64}$/.test(value), 'invalid artifact/workload SHA256'); }
export function validateReport(report, requireProfile = false) {
  assert.equal(report.schema, 'dynarmic.js/benchmark-v2');
  validateEnvironment(report);
  assert.deepEqual(report.methodology, methodology, 'unknown methodology');
  assert.equal(report.cases.length, cases.length);
  assert.equal(new Set(report.cases.map(c => c.name)).size, cases.length);
  for (const spec of cases) {
    const item = report.cases.find(c => c.name === spec.name);
    assert(item, `missing case ${spec.name}`);
    assert.equal(item.architecture, spec.architecture);
    assert.equal(item.memoryMode, spec.memoryMode);
    sha256(item.workloadSha256); sha256(item.artifact?.jsSha256); sha256(item.artifact?.wasmSha256);
    assert(Number.isInteger(item.runs) && item.runs > 0 && item.runs <= 100);
    assert.equal(item.samples.length, item.runs * 9);
    item.samples.forEach(validateSample);
    assert(item.samples.every(s => Number.isInteger(s.process) && s.process >= 0 && s.process < item.runs), 'invalid process index');
    for (let p = 0; p < item.runs; ++p) validateProcess(item.samples.filter(s => s.process === p));
    assert(item.samples.every(s => s.instructions === item.samples[0].instructions), 'instruction count changed between processes');
    assert(item.samples.every(s => (s.profile !== null) === report.build.profile), 'build/profile mismatch');
    if (requireProfile) assert(report.build.profile, 'profile build required');
    assert.deepEqual(item.summary, summarize(item.samples), 'incorrect summary');
  }
}
