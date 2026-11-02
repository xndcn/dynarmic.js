import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtempSync, writeFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {compareReports} from './compare.mjs';
import {compareOverhead} from './overhead.mjs';
import {report, refresh} from './fixtures.mjs';

test('identical measurements pass even when executable hashes change', () => {
  const after = report(); after.cases[0].artifact.wasmSha256 = 'f'.repeat(64);
  after.git.revision = 'f'.repeat(40);
  assert(compareReports(report(), after).pass);
});
for (const [name, factor, pass] of [['boundary', 1 / 0.95, true], ['regression', 1.06, false], ['improvement', 0.5, true]]) {
  test(`warm throughput ${name}`, () => {
    const after = report();
    for (const s of after.cases[0].samples.filter(s => s.phase === 'warm')) { s.ms *= factor; s.minstrPerSec /= factor; }
    assert.equal(compareReports(report(), refresh(after)).pass, pass);
  });
}
for (const [key, value, pass] of [['bytes', 110, true], ['bytes', 111, false], ['translateMs', 0.115, true], ['translateMs', 0.116, false]]) {
  test(`cold ${key}=${value} budget`, () => {
    const after = report();
    for (const s of after.cases[0].samples.filter(s => s.phase === 'cold')) s.compile[key] = value;
    assert.equal(compareReports(report(), refresh(after)).pass, pass);
  });
}
test('cold execution does not masquerade as compilation', () => {
  const after = report();
  for (const s of after.cases[0].samples.filter(s => s.phase === 'cold')) { s.ms *= 2; s.minstrPerSec /= 2; }
  assert(compareReports(report(), refresh(after)).pass);
});
for (const [name, change] of [
  ['engine', r => { r.v8 = 'other'; }], ['CPU', r => { r.host.cpu = 'other'; }],
  ['CPU affinity', r => { r.runtime.cpuAffinity = '0'; }],
  ['engine flags', r => { r.runtime.nodeOptions = '--jitless'; }],
  ['compiler', r => { r.build.compilerVersion = 'other'; }], ['flags', r => { r.build.cxxFlags = '-O0'; }],
  ['upstream', r => { r.git.upstream = 'f'.repeat(40); }],
  ['workload', r => { r.cases[0].workloadSha256 = 'f'.repeat(64); }],
  ['instructions', r => { for (const s of r.cases[0].samples) { s.instructions *= 2; s.minstrPerSec *= 2; } refresh(r); }],
  ['insufficient processes', r => { r.cases[0].runs = 1; r.cases[0].samples = r.cases[0].samples.slice(0, 9); refresh(r); }],
]) test(`incompatible ${name} rejected`, () => { const after = report(); change(after); assert.throws(() => compareReports(report(), after)); });
test('profile cannot be compared against production', () => assert.throws(() => compareReports(report(), report(true))));
for (const field of ['regionDiagnostics', 'testExceptions']) test(`${field} cannot be hidden in a speed or overhead comparison`, () => {
  const before = report(), after = report(), profile = report(true);
  before.build[field] = false;
  after.build[field] = true;
  profile.build[field] = true;
  assert.throws(() => compareReports(before, after), /incompatible build/);
  assert.throws(() => compareOverhead(before, profile), /incompatible build/);
  delete after.build[field];
  assert.throws(() => compareReports(before, after), /incompatible build/);
});
test('profile overhead accepts otherwise identical builds', () => {
  const result = compareOverhead(report(), report(true));
  assert.equal(result.schema, 'dynarmic.js/profile-overhead-v1');
  assert.equal(result.cases.length, report().cases.length);
  assert(result.cases.every(item => item.warmThroughputRatio === 1));
});
for (const [name, change] of [
  ['same profile mode', r => { r.build.profile = false; }],
  ['different CPU', r => { r.host.cpu = 'other'; }],
  ['different workload', r => { r.cases[0].workloadSha256 = 'f'.repeat(64); }],
  ['different upstream', r => { r.git.upstream = 'f'.repeat(40); }],
  ['different implementation', r => { r.git.revision = 'f'.repeat(40); }],
  ['different focus policy', r => { r.build.a64RegionFocus = '0x0000000000001000,0x0000000000001004,0x0000000000002000'; }],
  ['too few processes', r => { r.cases[0].runs = 1; r.cases[0].samples = r.cases[0].samples.slice(0, 9); }],
]) test(`profile overhead rejects ${name}`, () => {
  const production = report(), profile = report(true);
  change(profile);
  assert.throws(() => compareOverhead(production, profile));
});
test('profile overhead represents undefined compile ratio without Infinity', () => {
  const production = report(), profile = report(true);
  for (const s of production.cases[0].samples) s.compile.translateMs = 0;
  assert.equal(compareOverhead(refresh(production), profile).cases[0].coldCompileRatio, null);
  for (const s of profile.cases[0].samples) s.compile.translateMs = 0;
  assert.equal(compareOverhead(production, refresh(profile)).cases[0].coldCompileRatio, 1);
});
test('zero compile baseline never silently divides by zero', () => {
  const before = report(), after = report();
  for (const s of before.cases[0].samples) s.compile.translateMs = 0;
  assert.equal(compareReports(refresh(before), after).pass, false);
});
test('CLI distinguishes pass, regression and incompatible experiment', () => {
  const dir = mkdtempSync(path.join(tmpdir(), 'dynarmic-compare-'));
  try {
    const before = path.join(dir, 'before.json'), after = path.join(dir, 'after.json');
    writeFileSync(before, JSON.stringify(report()));
    for (const [value, expected] of [[report(), 0], [report(true), 2]]) {
      writeFileSync(after, JSON.stringify(value));
      assert.equal(spawnSync(process.execPath, [new URL('./compare.mjs', import.meta.url).pathname, before, after]).status, expected);
    }
    const regression = report();
    for (const s of regression.cases[0].samples.filter(s => s.phase === 'cold')) s.compile.bytes = 200;
    writeFileSync(after, JSON.stringify(refresh(regression)));
    assert.equal(spawnSync(process.execPath, [new URL('./compare.mjs', import.meta.url).pathname, before, after]).status, 1);
  } finally { rmSync(dir, {recursive: true}); }
});
