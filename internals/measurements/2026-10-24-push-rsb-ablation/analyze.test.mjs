import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {analyzeRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';
import {compareReports} from './analyze.mjs';

const TARGETS = [
  '0x0000000000010cb8', '0x0000000000010e50', '0x0000000000010edc', '0x0000000000010f10',
  '0x0000000000010f4c', '0x0000000000010f8c', '0x0000000000010f9c', '0x0000000000010fa4',
  '0x0000000000010fb0', '0x0000000000010fb8', '0x0000000000010fd8', '0x0000000000010fe8',
  '0x0000000000010ff0', '0x0000000000011008', '0x0000000000011024', '0x0000000000011038',
  '0x000000000001104c', '0x0000000000011060', '0x0000000000011070', '0x00000000000110ac',
  '0x00000000000110f8', '0x0000000000011144', '0x0000000000011188', '0x00000000000111d0',
  '0x00000000000111ec', '0x000000000001120c', '0x0000000000011220', '0x000000000001122c',
];
const HOT = [
  '0x0000000000010b00', '0x0000000000010b18', '0x0000000000011700',
  '0x0000000000011fd0', '0x000000000001202c', '0x000000000001209c',
];
const descriptor = value => `0x${value.toString(16).padStart(16, '0')}`;
const repeat = (value, count) => Array.from({length: count}, () => value);
const CRC = Object.freeze({seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '4983'});
const COREMARK_STDOUT = `2K performance run parameters for coremark.
CoreMark Size    : 666
Total ticks      : 1000000
Total time (secs): 1.000000
Iterations/Sec   : 200000.000000
ERROR! Must execute for at least 10 secs for a valid result!
Iterations       : 200000
Compiler version : test
Compiler flags   : -O2
Memory location  : STACK
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
[0]crcfinal      : 0x4983
Errors detected`;
const metrics = () => ({
  ticks: 1000000, seconds: 1, iterationsPerSecond: 200000, crc: {...CRC}, durationValid: false,
  compilerVersion: 'test', compilerFlags: '-O2',
});

function region(regionId, selectedDescriptors) {
  const moduleBytes = regionId === 1 ? 6457 : 6386;
  return {
    regionId, frontend: 'a64', entryDescriptor: selectedDescriptors[0], entryPc: selectedDescriptors[0],
    entryIndex: 0, moduleBytes, entryBodyBytes: regionId === 1 ? 6337 : 6254, selectedDescriptors,
  };
}

function baselineRegions() {
  const filler = [...HOT, ...Array.from({length: 828}, (_, i) => descriptor(0x20000 + i * 4))];
  const queue = [
    ...TARGETS.flatMap((value, i) => repeat(value, i < 3 ? 16 : 15)),
    ...filler.slice(6).flatMap((value, i) => repeat(value, i + 6 < 97 ? 6 : i + 6 < 559 ? 5 : 0)),
    ...HOT.flatMap(value => repeat(value, 5)),
  ];
  const regions = [region(1, [...TARGETS, ...filler]), ...HOT.map((value, i) => region(i + 2, [value]))];
  const distributed = Array.from({length: 104}, () => []);
  queue.forEach((value, i) => distributed[i % distributed.length].push(value));
  return [...regions, ...distributed.map((selected, i) => region(regions.length + i + 1, selected))];
}

function report(regions, dirty = false) {
  const regionDiagnostics = {schema: 'dynarmic.js/region-diagnostics-v1', regions};
  const duplication = analyzeRegionDiagnostics(regionDiagnostics);
  return {
    schema: 'dynarmic.js/coremark-region-diagnostics-v1', architecture: 'a64', memoryMode: '2',
    workloadSha256: 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa',
    artifact: {jsSha256: 'a'.repeat(64), wasmSha256: 'b'.repeat(64)},
    native: {stdout: COREMARK_STDOUT, metrics: metrics()},
    sample: {
      haltReason: 0x01000000,
      compile: {blocks: duplication.totals.regionCount, bytes: duplication.totals.moduleBytes},
      profile: null, stdout: COREMARK_STDOUT, metrics: metrics(),
    },
    regionDiagnostics, duplication,
    git: {
      revision: '9731e9a779755edb3e61ca17ed8ea3798119f3ec',
      upstream: 'a41c380246d3d9f9874f0f792d234dc0cc17c180', dirty,
    },
    build: {profile: false},
    workload: {iterations: 200000, artifacts: {a64: {sha256: '8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1'}}},
  };
}

function recalculate(value) {
  const duplication = analyzeRegionDiagnostics(value.regionDiagnostics);
  return {...value, duplication, sample: {...value.sample, compile: {
    ...value.sample.compile, blocks: duplication.totals.regionCount, bytes: duplication.totals.moduleBytes,
  }}};
}

function candidate(moduleDelta = -1, bodyDelta = -1) {
  const value = report(baselineRegions(), true);
  value.artifact = {jsSha256: 'c'.repeat(64), wasmSha256: 'd'.repeat(64)};
  value.regionDiagnostics.regions = value.regionDiagnostics.regions.map(item => ({
    ...item, moduleBytes: item.moduleBytes + moduleDelta, entryBodyBytes: item.entryBodyBytes + bodyDelta,
  }));
  return recalculate(value);
}

function withoutTarget(value, target = TARGETS[0]) {
  value.regionDiagnostics.regions = value.regionDiagnostics.regions.map(item => {
    const selectedDescriptors = item.selectedDescriptors.filter(descriptor => descriptor !== target);
    return {...item, selectedDescriptors, entryDescriptor: selectedDescriptors[0], entryPc: selectedDescriptors[0], entryIndex: 0};
  });
  return recalculate(value);
}

function freeze(value) {
  if (value && typeof value === 'object') {
    Object.values(value).forEach(freeze);
    Object.freeze(value);
  }
  return value;
}

test('pins archived totals and reports target and total deltas without mutating inputs', () => {
  const before = freeze(report(baselineRegions()));
  const after = freeze(candidate());
  const result = compareReports(before, after);
  assert.equal(result.verdict, 'PASS');
  assert.deepEqual(result.totals.before, {
    regionCount: 111, selectedBlockCount: 4177, uniqueDescriptorCount: 862,
    duplicatedDescriptorCount: 587, duplicateSelectionCount: 3315, moduleBytes: 708917, entryBodyBytes: 694277,
  });
  assert.equal(result.totals.delta.moduleBytes, -111);
  assert.equal(result.totals.delta.entryBodyBytes, -111);
  assert.equal(result.targets.descriptors.length, 28);
  assert.deepEqual(result.targets.before, {selectionCount: 451, duplicateSelectionCount: 423});
  assert.deepEqual(result.targets.descriptors[0].before.selectedRegionIds,
    before.duplication.descriptors.find(row => row.descriptor === TARGETS[0]).selectedRegionIds);
});

for (const [moduleDelta, bodyDelta, verdict] of [
  [-1, -1, 'PASS'], [-1, 0, 'REJECT'], [0, -1, 'REJECT'], [0, 0, 'REJECT'],
]) test(`strict byte thresholds: module ${moduleDelta}, entry ${bodyDelta} => ${verdict}`, () => {
  assert.equal(compareReports(report(baselineRegions()), candidate(moduleDelta, bodyDelta)).verdict, verdict);
});

test('accepts identical JS for a C++-only candidate and records artifact identity', () => {
  const before = report(baselineRegions()), after = candidate();
  after.artifact.jsSha256 = before.artifact.jsSha256;
  const result = compareReports(before, after);
  assert.equal(result.verdict, 'PASS');
  assert.deepEqual(result.artifacts, {
    before: before.artifact, after: after.artifact, jsIdentical: true, wasmIdentical: false,
  });
});

test('allows an ablated candidate target to disappear with zero after-history', () => {
  const result = compareReports(report(baselineRegions()), withoutTarget(candidate()));
  assert.equal(result.verdict, 'PASS');
  assert.deepEqual(result.targets.descriptors[0].after, {
    selectionCount: 0, selectedRegionIds: [], entryRegionIds: [], duplicateSelectionCount: 0,
  });
  assert.equal(result.targets.after.selectionCount, 434);
  assert.equal(result.targets.after.duplicateSelectionCount, 407);
});

test('requires every pinned target in baseline, not merely the aggregate', () => {
  const after = candidate();
  for (const target of TARGETS) {
    assert.throws(() => compareReports(withoutTarget(report(baselineRegions()), target), after),
      new RegExp(`baseline missing pinned target ${target}`));
  }
});

test('pins the CoreMark iteration count instead of trusting each report', () => {
  const before = report(baselineRegions()), after = candidate();
  for (const value of [before, after]) {
    value.workload.iterations = 120000;
    for (const side of [value.native, value.sample]) {
      side.stdout = side.stdout.replace('Iterations       : 200000', 'Iterations       : 120000')
        .replace('Total ticks      : 1000000', 'Total ticks      : 600000')
        .replace('Total time (secs): 1.000000', 'Total time (secs): 0.600000');
      side.metrics.ticks = 600000;
      side.metrics.seconds = 0.6;
    }
  }
  assert.throws(() => compareReports(before, after), /iterations must be 200000/);
});

test('ignores regionId renumbering but detects other entry history changes', () => {
  const before = report(baselineRegions());
  const after = candidate(0, 0);
  after.regionDiagnostics.regions = after.regionDiagnostics.regions.reverse().map((item, i) => ({...item, regionId: i + 1}));
  const renumbered = recalculate(after);
  const unchanged = compareReports(before, renumbered);
  assert(!unchanged.changedEntryHistories.some(row => row.entryDescriptor === HOT[0]));
  const retained = unchanged.retainedHotHistories.find(row => row.entryDescriptor === HOT[0]);
  assert.equal(retained.after[0].regionId, 110);

  const changedCandidate = candidate(0, 0);
  changedCandidate.regionDiagnostics.regions = changedCandidate.regionDiagnostics.regions.map(item =>
    item.entryDescriptor === HOT[0] ? {...item, entryBodyBytes: item.entryBodyBytes - 1} : item);
  const validAfter = recalculate(changedCandidate);
  const result = compareReports(before, validAfter);
  assert.equal(result.changedEntryHistories.length, 1);
  const changed = result.changedEntryHistories[0];
  assert.equal(changed.entryDescriptor, HOT[0]);
  assert.equal(changed.before[0].regionId, 2);
  assert.equal(changed.after[0].regionId, 2);
  const hot = result.retainedHotHistories.find(row => row.entryDescriptor === HOT[0]);
  assert.deepEqual(hot.before, before.regionDiagnostics.regions.filter(row => row.entryDescriptor === HOT[0]));
  assert.deepEqual(hot.after, validAfter.regionDiagnostics.regions.filter(row => row.entryDescriptor === HOT[0]));
  assert.equal(result.retainedHotHistories.length, HOT.length);
});

const rejections = [
  ['report schema', (a, b) => { b.schema = 'wrong'; }, /schema/],
  ['abnormal halt', (a, b) => { b.sample.haltReason = 0; }, /halt/],
  ['CRC contamination', (a, b) => { b.sample.metrics.crc.crcfinal = 'ffff'; }, /CRC/],
  ['retained stdout CRC contamination', (a, b) => {
    b.sample.stdout = b.sample.stdout.replace('crcfinal      : 0x4983', 'crcfinal      : 0xffff');
  }, /CRC/],
  ['compile block mismatch', (a, b) => { b.sample.compile.blocks--; }, /block/],
  ['compile byte mismatch', (a, b) => { b.sample.compile.bytes--; }, /byte/],
  ['stale embedded duplication', (a, b) => { b.duplication.totals.moduleBytes--; }, /duplication/],
  ['architecture', (a, b) => { b.architecture = 'a32'; }, /architecture/],
  ['memory mode', (a, b) => { b.memoryMode = '1'; }, /memoryMode/],
  ['workload identity', (a, b) => { b.workloadSha256 = '0'.repeat(64); }, /workload/],
  ['A64 binary identity', (a, b) => { b.workload.artifacts.a64.sha256 = '0'.repeat(64); }, /A64/],
  ['base revision', (a, b) => { b.git.revision = '0'.repeat(40); }, /revision/],
  ['upstream revision', (a, b) => { b.git.upstream = '0'.repeat(40); }, /upstream/],
  ['profiled build', (a, b) => { b.build.profile = true; }, /profile/],
  ['sample profile counters', (a, b) => { b.sample.profile = {runCount: 1}; }, /profile/],
  ['dirty baseline', a => { a.git.dirty = true; }, /clean/],
  ['clean candidate', (a, b) => { b.git.dirty = false; }, /dirty/],
  ['stale Wasm artifact', (a, b) => { b.artifact.wasmSha256 = a.artifact.wasmSha256; }, /Wasm/],
  ['changed archived baseline totals', a => {
    a.regionDiagnostics.regions[0].moduleBytes++;
    Object.assign(a, recalculate(a));
  }, /baseline.*totals/],
];
for (const [name, mutate, pattern] of rejections) test(`fail closed: ${name}`, () => {
  const before = report(baselineRegions()), after = candidate();
  mutate(before, after);
  assert.throws(() => compareReports(before, after), pattern);
});

function cliFixture(t) {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'push-rsb-comparison-test-'));
  t.after(() => fs.rmSync(directory, {recursive: true, force: true}));
  const before = path.join(directory, 'baseline.json'), after = path.join(directory, 'candidate.json');
  const output = path.join(directory, 'comparison.json');
  fs.writeFileSync(before, JSON.stringify(report(baselineRegions())));
  fs.writeFileSync(after, JSON.stringify(candidate()));
  return {before, after, output};
}
const analyzer = fileURLToPath(new URL('./analyze.mjs', import.meta.url));
const runCli = args => spawnSync(process.execPath, [analyzer, ...args], {encoding: 'utf8'});

test('CLI writes PASS and valid REJECT comparisons with distinct exit codes', t => {
  const paths = cliFixture(t);
  const pass = runCli([paths.before, paths.after, paths.output]);
  assert.equal(pass.status, 0, pass.stderr);
  assert.equal(JSON.parse(fs.readFileSync(paths.output)).verdict, 'PASS');
  fs.rmSync(paths.output);
  fs.writeFileSync(paths.after, JSON.stringify(candidate(0, -1)));
  const reject = runCli([paths.before, paths.after, paths.output]);
  assert.equal(reject.status, 1, reject.stderr);
  assert.equal(JSON.parse(fs.readFileSync(paths.output)).verdict, 'REJECT');
});

test('CLI rejects malformed input without output and never overwrites', t => {
  const paths = cliFixture(t);
  fs.writeFileSync(paths.after, '{}');
  assert.equal(runCli([paths.before, paths.after, paths.output]).status, 2);
  assert(!fs.existsSync(paths.output));
  fs.writeFileSync(paths.output, 'keep');
  const result = runCli([paths.before, paths.after, paths.output]);
  assert.equal(result.status, 2);
  assert.match(result.stderr, /overwrite/);
  assert.equal(fs.readFileSync(paths.output, 'utf8'), 'keep');
});
