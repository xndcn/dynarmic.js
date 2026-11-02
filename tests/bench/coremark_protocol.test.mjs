import {test} from 'node:test';
import assert from 'node:assert/strict';
import {output, workload, fixture, refresh} from './coremark_fixtures.mjs';
import {coremarkCases, coremarkMethodology, parseCoremark, validateCoremarkBuild,
  coremarkIdentity, summarizeCoremark, validateCoremarkReport} from './coremark_protocol.mjs';
import {compareCoremark} from './coremark/compare.mjs';

test('CoreMark short correctness and duration-qualified reports in both profile modes', () => {
  for (const profile of [false, true]) for (const ticks of [500000, 10000000]) validateCoremarkReport(fixture(profile, ticks));
  assert.throws(() => validateCoremarkReport(fixture(), true), /below 10 seconds/);
  validateCoremarkReport(fixture(false, 10000000), true);
});
for (const [name, change] of [
  ['wrong stage CRC', s => s.replace('e714', 'e715')],
  ['unrecognized diagnostic', s => s + 'unexpected failure\n'],
  ['missing long summary', () => output(10000000).replace(/CoreMark 1.0[^\n]+\n/, '')],
  ['wrong long summary', () => output(10000000).replace('CoreMark 1.0 : 400.000000', 'CoreMark 1.0 : 999.000000')],
  ['unknown error', s => s + 'ERROR! corrupted\n'],
  ['unknown error with valid marker', () => output(10000000) + 'ERROR! corrupted\n'],
  ['missing short error', s => s.replace('Errors detected', '')],
  ['duplicate ticks', s => s + 'Total ticks : 500000\n'],
  ['missing final CRC', s => s.replace('[0]crcfinal', '[1]crcfinal')],
  ['inconsistent seconds', s => s.replace('0.500000', '1.500000')],
  ['inconsistent rate', s => s.replace('8000.000000', '9000.000000')],
  ['wrong iterations', s => s.replace('Iterations       : 4000', 'Iterations       : 4001')],
  ['wrap timer', s => s.replace('500000', '4294967296')],
]) test(`CoreMark rejects ${name}`, () => assert.throws(() => parseCoremark(change(output()), 4000)));
for (const [name, change] of [
  ['fault after valid output', r => { r.cases[0].samples[0].haltReason = 0x02000000; }],
  ['missing case', r => r.cases.pop()],
  ['duplicate case', r => { r.cases[0] = r.cases[1]; }],
  ['missing process', r => r.cases[0].samples.pop()],
  ['duplicate process', r => { r.cases[0].samples[0].process = 1; }],
  ['tampered summary', r => { r.cases[0].summary.kernelMedianMs++; }],
  ['partial profile', r => { delete r.cases[0].samples[0].profile.dispatchCount; }],
  ['wrong profile mode', r => { r.cases[0].samples[0].profile = null; }],
  ['wrong cache peak', r => { r.cases[0].samples[0].profile.peakCachedBytes++; }],
  ['wrong run count', r => { r.cases[0].samples[0].profile.runCount = 2; }],
  ['invalidation', r => { r.cases[0].samples[0].profile.invalidationCount = 1; }],
  ['missing affinity', r => { delete r.runtime.cpuAffinity; }],
  ['manifest mutation', r => { r.workload.sources['port/build.py'] = '0'.repeat(64); }],
  ['compiler flags', r => { r.cases[0].samples[0].stdout = output().replace('-O2', '-O3'); }],
  ['final CRC differs from native', r => {
    const s = r.cases[0].samples[0]; s.stdout = output(500000, '1234'); s.metrics = parseCoremark(s.stdout, 4000);
  }],
  ['kernel exceeds Run', r => { r.cases[0].samples[0].runMs = 100; }],
  ['phases exceed Run', r => { r.cases[0].samples[0].compile.translateMs = 1000; }],
]) test(`CoreMark report rejects ${name}`, () => { const r = fixture(true); change(r); assert.throws(() => validateCoremarkReport(r)); });
test('CoreMark workload identity includes nested inputs and ignores command paths', () => {
  for (const change of [w => { w.sources['port/build.py'] = '0'.repeat(64); },
    w => { w.compilers.a32 = 'new gcc'; }, w => { w.artifacts.a64.sha256 = '0'.repeat(64); }]) {
    const w = workload(), before = coremarkIdentity(w); change(w); assert.notEqual(coremarkIdentity(w), before);
  }
  const w = workload(), before = coremarkIdentity(w); w.commands.a32.push('/other/path'); assert.equal(coremarkIdentity(w), before);
  delete w.sources['port/build.py']; assert.throws(() => validateCoremarkBuild(w));
});
test('CoreMark comparator enforces experiment identity, five processes and duration', () => {
  assert.equal(compareCoremark(fixture(), fixture()).pass, true);
  assert.throws(() => compareCoremark(fixture(), fixture(), true));
  for (const change of [r => { r.runtime.cpuAffinity = '4-7'; }, r => { r.git.upstream = '0'.repeat(40); },
    r => { r.cases.forEach(c => { c.runs = 1; c.samples = c.samples.slice(0, 1); }); refresh(r); }]) {
    const r = fixture(); change(r); assert.throws(() => compareCoremark(fixture(), r));
  }
});
test('CoreMark comparator catches kernel, Run, compile and size regressions independently', () => {
  for (const change of [s => { s.stdout = output(600000); s.metrics = parseCoremark(s.stdout, 4000); s.runMs = 650; },
    s => { s.runMs *= 2; }, s => { s.compile.translateMs *= 2; },
    s => { s.compile.bytes *= 2; s.profile.emittedBytes *= 2; s.profile.peakCachedBytes *= 2; }]) {
    const r = fixture(true); r.cases[0].samples.forEach(change); refresh(r);
    assert.equal(compareCoremark(fixture(true), r).pass, false);
  }
});
