import assert from 'node:assert/strict';
import {compileTimers, median, profileCounters} from './protocol.mjs';

export const populationSizes = [1, 64, 512, 4096];
export const populationModes = [
  ['thunk', 0], ['page-table', 1], ['flat-window', 2],
];
export const populationCases = populationModes.flatMap(([memoryMode, mode]) =>
  populationSizes.map(population => ({
    name: `cache-population-${memoryMode}-${population}`,
    architecture: 'a32', memoryMode, population, argv: [String(mode), String(population)],
  })));

export const populationMethodology = {
  populations: populationSizes,
  workload: 'Compile independent A32 entry blocks and keep every module live in one JIT.',
  timing: 'Each pass includes Run, profile publication, reset and result checking; JIT construction is excluded.',
  warm: 'One subsequent pass over all entries in the same JIT; must compile zero modules; tiering may continue.',
  ordering: 'Rotate cases each round; each case uses a fresh process.',
  memory: 'Cache peaks count emitted Wasm bytes only, not engine code, JS heap or total resident memory.',
  comparison: 'Compare equal memory mode, population, build identity and workload source.',
};

const compileKeys = ['blocks', 'bytes', ...compileTimers];
const sha256 = value => assert(typeof value === 'string' && /^[a-f0-9]{64}$/.test(value), 'invalid SHA256');

export function validatePopulationSample(sample, population, profileRequired) {
  assert(Number.isFinite(sample.ms) && sample.ms > 0, 'invalid population timing');
  assert.equal(sample.population, population, 'population mismatch');
  assert(['cold', 'warm'].includes(sample.phase), 'invalid population phase');
  const cold = sample.phase === 'cold';
  for (const key of compileKeys) {
    assert(Number.isFinite(sample.compile?.[key]) && sample.compile[key] >= 0, `invalid compile.${key}`);
    if (!cold) assert.equal(sample.compile[key], 0, `warm pass changed compile.${key}`);
  }
  assert.equal(sample.compile.blocks, cold ? population : 0, 'population did not compile one module per entry');
  assert(Number.isSafeInteger(sample.compile.bytes) && sample.compile.bytes >= (cold ? 1 : 0), 'invalid compiled bytes');
  if (profileRequired) {
    assert(sample.profile && sample.profile.schema === 'dynarmic.js/profile-v1');
    assert.equal(sample.profile.enabled, true);
    for (const key of profileCounters) {
      assert(Number.isSafeInteger(sample.profile[key]) && sample.profile[key] >= 0, `invalid profile.${key}`);
    }
    assert.equal(sample.profile.runCount, population, 'one Run per entry required');
    assert.equal(sample.profile.stepCount, 0, 'population unexpectedly stepped');
    assert.equal(sample.profile.compiledBlocks, sample.compile.blocks, 'profile module count mismatch');
    assert.equal(sample.profile.emittedBytes, sample.compile.bytes, 'profile byte count mismatch');
    assert.equal(sample.profile.peakCachedBlocks, sample.compile.blocks, 'unexpected cache block peak');
    assert.equal(sample.profile.peakCachedBytes, sample.compile.bytes, 'unexpected cache byte peak');
    assert.equal(sample.profile.invalidationCount, 0, 'unexpected invalidation');
    assert.equal(sample.profile.invalidatedBlocks, 0, 'population evicted modules');
  } else {
    assert.equal(sample.profile, null, 'production population report unexpectedly has profile');
  }
  return sample;
}

export function summarizePopulation(samples, profileRequired) {
  const cold = samples.filter(s => s.phase === 'cold');
  const warm = samples.filter(s => s.phase === 'warm');
  const coldMedianMs = median(cold.map(s => s.ms));
  const warmMedianMs = median(warm.map(s => s.ms));
  return {
    coldMedianMs, warmMedianMs,
    coldMadMs: median(cold.map(s => Math.abs(s.ms - coldMedianMs))),
    warmMadMs: median(warm.map(s => Math.abs(s.ms - warmMedianMs))),
    compileMedianMs: median(cold.map(s => compileTimers.reduce((sum, key) => sum + s.compile[key], 0))),
    compileBytesMedian: median(cold.map(s => s.compile.bytes)),
    peakCachedBlocksMedian: profileRequired ? median(cold.map(s => s.profile.peakCachedBlocks)) : null,
    peakCachedBytesMedian: profileRequired ? median(cold.map(s => s.profile.peakCachedBytes)) : null,
  };
}

export function validatePopulationReport(report, requireProfile = false) {
  assert.equal(report.schema, 'dynarmic.js/cache-population-v1');
  assert(typeof report.timestamp === 'string' && Number.isFinite(Date.parse(report.timestamp)), 'invalid timestamp');
  for (const key of ['node', 'v8']) assert(typeof report[key] === 'string' && report[key].length > 0);
  for (const key of ['platform', 'release', 'arch', 'cpu']) {
    assert(typeof report.host?.[key] === 'string' && report.host[key].length > 0, `missing host.${key}`);
  }
  assert(Number.isSafeInteger(report.host.cpus) && report.host.cpus > 0);
  for (const key of ['revision', 'upstream']) {
    assert(typeof report.git?.[key] === 'string' && /^[a-f0-9]{40}$/.test(report.git[key]), `invalid git.${key}`);
  }
  assert.equal(typeof report.git?.dirty, 'boolean');
  for (const key of ['type', 'compilerId', 'compilerVersion', 'emscriptenVersion']) {
    assert(typeof report.build?.[key] === 'string' && report.build[key].length > 0, `missing build.${key}`);
  }
  for (const key of ['cxxFlags', 'configCxxFlags', 'linkFlags', 'configLinkFlags']) {
    assert.equal(typeof report.build[key], 'string', `missing build.${key}`);
  }
  assert.equal(typeof report.runtime?.nodeOptions, 'string');
  assert(typeof report.runtime.cpuAffinity === 'string' && report.runtime.cpuAffinity.length > 0);
  assert.equal(typeof report.build?.profile, 'boolean');
  if (requireProfile) assert.equal(report.build.profile, true, 'profile build required');
  assert.deepEqual(report.methodology, populationMethodology);
  assert.equal(report.cases.length, populationCases.length);
  for (const spec of populationCases) {
    const item = report.cases.find(candidate => candidate.name === spec.name);
    assert(item, `missing case ${spec.name}`);
    assert.equal(item.architecture, spec.architecture);
    assert.equal(item.memoryMode, spec.memoryMode);
    assert.equal(item.population, spec.population);
    sha256(item.workloadSha256); sha256(item.artifact?.jsSha256); sha256(item.artifact?.wasmSha256);
    assert(Number.isInteger(item.runs) && item.runs > 0 && item.runs <= 100);
    assert.equal(item.samples.length, item.runs * 2);
    assert(item.samples.every(s => Number.isInteger(s.process) && s.process >= 0 && s.process < item.runs));
    for (let process = 0; process < item.runs; ++process) {
      const sample = item.samples.filter(s => s.process === process);
      assert.deepEqual(sample.map(s => s.phase), ['cold', 'warm'], 'one cold/warm pair per process');
      sample.forEach(s => validatePopulationSample(s, spec.population, report.build.profile));
    }
    assert.deepEqual(item.summary, summarizePopulation(item.samples, report.build.profile));
  }
  return report;
}
