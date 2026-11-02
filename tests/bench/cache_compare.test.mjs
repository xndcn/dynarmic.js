import {test} from 'node:test';
import assert from 'node:assert/strict';
import {cacheCases, cacheMethodology, summarizeCache} from './cache_protocol.mjs';
import {compareCacheReports} from './cache_compare.mjs';
import {report as fixedReport} from './fixtures.mjs';

const sample = (cycle, process = 0) => ({cycle, process, value: cycle + 1, ms: 10,
  compile: {blocks: 1, bytes: 100, translateMs: 1, optMs: 1, emitMs: 1, compileMs: 1, instMs: 1, tableMs: 1},
  profile: null});
const report = () => ({schema: 'dynarmic.js/cache-benchmark-v1', timestamp: '2026-10-22T00:00:00.000Z',
  node: 'v24.test', v8: 'test', host: {platform: 'linux', release: 'test', arch: 'x64', cpu: 'test', cpus: 4},
  runtime: {nodeOptions: '', cpuAffinity: '0-3'},
  git: {revision: 'a'.repeat(40), upstream: 'b'.repeat(40), dirty: false},
  build: fixedReport().build,
  methodology: cacheMethodology,
  cases: cacheCases.map(spec => { const samples = Array.from({length: 5}, (_, p) =>
    [...Array(cacheMethodology.cycles).keys()].map(cycle => sample(cycle, p))).flat();
    return {...spec, architecture: 'a32', runs: 5, workloadSha256: 'c'.repeat(64),
      artifact: {jsSha256: 'd'.repeat(64), wasmSha256: 'e'.repeat(64)}, samples,
      summary: summarizeCache(samples, false)};
  }),
});

test('cache comparator accepts identical compatible reports', () => {
  assert.equal(compareCacheReports(report(), report()).pass, true);
});
test('cache comparator rejects total latency regression', () => {
  const baseline = report(), candidate = report();
  for (const item of candidate.cases) {
    item.samples.forEach(sample => { sample.ms *= 1.16; });
    item.summary = summarizeCache(item.samples, false);
  }
  assert.equal(compareCacheReports(baseline, candidate).pass, false);
});
test('cache comparator rejects a single cycle regression', () => {
  const baseline = report(), candidate = report();
  for (const item of candidate.cases) {
    item.samples.filter(sample => sample.cycle === 6).forEach(sample => { sample.ms *= 1.16; });
    item.summary = summarizeCache(item.samples, false);
  }
  assert.equal(compareCacheReports(baseline, candidate).pass, false);
});
test('cache comparator rejects incompatible workload and process count', () => {
  const baseline = report(), candidate = report();
  candidate.cases[0].workloadSha256 = 'f'.repeat(64);
  assert.throws(() => compareCacheReports(baseline, candidate));
  const tooFew = report();
  tooFew.cases[0].runs = 1;
  tooFew.cases[0].samples = tooFew.cases[0].samples.filter(sample => sample.process === 0);
  tooFew.cases[0].summary = summarizeCache(tooFew.cases[0].samples, false);
  assert.throws(() => compareCacheReports(baseline, tooFew));
});
test('cache report rejects malformed provenance', () => {
  const broken = report();
  broken.cases[0].artifact.wasmSha256 = 'bad';
  assert.throws(() => compareCacheReports(report(), broken));
});
for (const [name, mutate] of [
  ['affinity', r => { r.runtime.cpuAffinity = '0'; }],
  ['engine flags', r => { r.runtime.nodeOptions = '--jitless'; }],
  ['compiler flags', r => { r.build.configCxxFlags = '-O0'; }],
  ['focus option', r => { r.build.a64RegionFocus = '0x0000000000001000,0x0000000000001004,0x0000000000002000'; }],
  ['upstream', r => { r.git.upstream = 'f'.repeat(40); }],
  ['missing build flags', r => { delete r.build.linkFlags; }],
  ['missing affinity', r => { delete r.runtime.cpuAffinity; }],
]) test(`cache comparison rejects incompatible ${name}`, () => {
  const after = report(); mutate(after);
  assert.throws(() => compareCacheReports(report(), after));
});
