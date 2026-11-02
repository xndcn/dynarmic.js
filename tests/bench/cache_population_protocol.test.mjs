import {test} from 'node:test';
import assert from 'node:assert/strict';
import {comparePopulationReports} from './cache_population_compare.mjs';
import {compileTimers, profileCounters} from './protocol.mjs';
import {populationCases, populationMethodology, summarizePopulation, validatePopulationReport} from './cache_population_protocol.mjs';

function report(profile = false, runs = 3) {
  const cases = populationCases.map(spec => {
    const samples = Array.from({length: runs}, (_, process) => ['cold', 'warm'].map(phase => {
      const cold = phase === 'cold';
      const blocks = cold ? spec.population : 0;
      const bytes = blocks * 100;
      return {population: spec.population, phase, ms: (cold ? 4 : 1) + process,
        compile: {blocks, bytes, ...Object.fromEntries(compileTimers.map(key => [key, cold ? 0.1 : 0]))},
        profile: profile ? {
          schema: 'dynarmic.js/profile-v1', enabled: true,
          ...Object.fromEntries(profileCounters.map(key => [key, 0])),
          runCount: spec.population, compiledBlocks: blocks, emittedBytes: bytes,
          peakCachedBlocks: blocks, peakCachedBytes: bytes,
        } : null, process};
    })).flat();
    return {...spec, runs, workloadSha256: 'a'.repeat(64),
      artifact: {jsSha256: 'b'.repeat(64), wasmSha256: 'c'.repeat(64)}, samples,
      summary: summarizePopulation(samples, profile)};
  });
  return {schema: 'dynarmic.js/cache-population-v1', timestamp: '2026-10-22T00:00:00.000Z',
    node: 'v24', v8: 'test', host: {platform: 'linux', release: 'test', arch: 'x64', cpu: 'test', cpus: 4},
    runtime: {nodeOptions: '', cpuAffinity: '0-3'},
    git: {revision: 'd'.repeat(40), upstream: 'e'.repeat(40), dirty: false},
    build: {type: 'Release', profile, compilerId: 'Clang', compilerVersion: '18', emscriptenVersion: '6.0.13',
      cxxFlags: '', configCxxFlags: '-O3', linkFlags: '', configLinkFlags: ''},
    methodology: populationMethodology, cases};
}

test('cache population production report validates', () => validatePopulationReport(report()));
test('cache population profile report validates', () => validatePopulationReport(report(true), true));
test('cache population summary uses each pass across fresh processes', () => {
  assert.deepEqual(report(true).cases[0].summary, {
    coldMedianMs: 5, warmMedianMs: 2, coldMadMs: 1, warmMadMs: 1,
    compileMedianMs: 0.6, compileBytesMedian: 100,
    peakCachedBlocksMedian: 1, peakCachedBytesMedian: 100,
  });
});
for (const [name, change] of [
  ['wrong schema', r => { r.schema = 'old'; }],
  ['missing population case', r => { r.cases.pop(); }],
  ['duplicate population case', r => { r.cases[1] = structuredClone(r.cases[0]); }],
  ['wrong module count', r => { r.cases[0].samples[0].compile.blocks = 2; }],
  ['wrong cache peak', r => { r.cases[0].samples[0].profile.peakCachedBlocks = 0; }],
  ['inflated cache peak', r => { r.cases[0].samples[0].profile.peakCachedBytes++; }],
  ['evicted module', r => { r.cases[0].samples[0].profile.invalidatedBlocks++; }],
  ['warm compilation', r => { r.cases[0].samples[1].compile.blocks = 1; }],
  ['warm compile timing', r => { r.cases[0].samples[1].compile.emitMs = 1; }],
  ['missing warm pass', r => { r.cases[0].samples.pop(); }],
  ['reordered passes', r => { r.cases[0].samples.reverse(); }],
  ['duplicate process', r => { r.cases[0].samples[2].process = 0; }],
  ['invalid process', r => { r.cases[0].samples[0].process = -1; }],
  ['bad summary', r => { r.cases[0].summary.coldMedianMs = 42; }],
  ['missing build flags', r => { delete r.build.cxxFlags; }],
  ['missing affinity', r => { delete r.runtime.cpuAffinity; }],
  ['missing artifact hash', r => { delete r.cases[0].artifact.jsSha256; }],
  ['missing upstream revision', r => { delete r.git.upstream; }],
  ['missing profile counter', r => { delete r.cases[0].samples[0].profile.regionCandidates; }],
  ['coerced artifact hash', r => { r.cases[0].artifact.jsSha256 = [r.cases[0].artifact.jsSha256]; }],
  ['coerced revision', r => { r.git.revision = [r.git.revision]; }],
  ['coerced timestamp', r => { r.timestamp = [r.timestamp]; }],
]) test(`cache population rejects ${name}`, () => {
  const r = report(true); change(r);
  assert.throws(() => validatePopulationReport(r, true));
});
test('cache population requires profile when requested', () => {
  assert.throws(() => validatePopulationReport(report(), true));
});

test('population comparator accepts compatible reports and changed implementation hashes', () => {
  const before = report(true, 5), after = structuredClone(before);
  after.git.revision = 'f'.repeat(40);
  after.cases[0].artifact.jsSha256 = 'f'.repeat(64);
  assert.equal(comparePopulationReports(before, after).pass, true);
});
for (const phase of ['cold', 'warm']) test(`population comparator catches ${phase} latency regression`, () => {
  const before = report(false, 5), after = structuredClone(before);
  const item = after.cases[11];
  item.samples.filter(s => s.phase === phase).forEach(s => { s.ms *= 1.16; });
  item.summary = summarizePopulation(item.samples, false);
  const result = comparePopulationReports(before, after);
  assert.equal(result.pass, false);
  assert.equal(result.cases[11].metrics.find(m => m.metric === `${phase}MedianMs`).pass, false);
});
test('population comparator catches emitted-byte and peak regressions', () => {
  const before = report(true, 5), after = structuredClone(before);
  const item = after.cases[11];
  item.samples.filter(s => s.phase === 'cold').forEach(s => {
    s.compile.bytes *= 2;
    s.profile.emittedBytes = s.profile.peakCachedBytes = s.compile.bytes;
  });
  item.summary = summarizePopulation(item.samples, true);
  const result = comparePopulationReports(before, after);
  assert.equal(result.pass, false);
  assert.equal(result.cases[11].metrics.find(m => m.metric === 'peakCachedBytesMedian').pass, false);
});
for (const [name, change] of [
  ['affinity', r => { r.runtime.cpuAffinity = '4-7'; }],
  ['flags', r => { r.build.cxxFlags = '-O0'; }],
  ['workload', r => { r.cases[0].workloadSha256 = 'f'.repeat(64); }],
  ['upstream', r => { r.git.upstream = 'f'.repeat(40); }],
]) test(`population comparator rejects incompatible ${name}`, () => {
  const before = report(false, 5), after = structuredClone(before); change(after);
  assert.throws(() => comparePopulationReports(before, after));
});
test('population comparator requires five fresh processes', () => {
  assert.throws(() => comparePopulationReports(report(), report()));
});
