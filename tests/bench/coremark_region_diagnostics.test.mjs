import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {createCoremarkRegionDiagnosticReport} from './coremark/region_diagnostics.mjs';
import {collectCoremarkSample} from './coremark/sample.mjs';
import {profileCounters} from './protocol.mjs';

const descriptor = pc => '0x' + pc.toString(16).padStart(16, '0');
const region = (id, entry, other) => ({
  regionId: id,
  frontend: 'a64',
  entryDescriptor: descriptor(entry),
  entryPc: descriptor(entry),
  entryIndex: 1,
  moduleBytes: 1000 + id,
  entryBodyBytes: 800 + id,
  selectedDescriptors: [descriptor(0x800), descriptor(entry), descriptor(other)],
});
const diagnostics = () => ({
  schema: 'dynarmic.js/region-diagnostics-v1',
  regions: [region(1, 0x1000, 0x1004), region(2, 0x1100, 0x1104)],
});
const crc = () => ({seedcrc: 'e9f5', crclist: 'e714', crcmatrix: '1fd7', crcstate: '8e3a', crcfinal: '1234'});
const transfers = () => ({
  schema: 'dynarmic.js/region-transfer-diagnostics-v1',
  regions: [{
    regionId: 1,
    frontend: 'a64',
    callerEntryDescriptor: descriptor(0x1000),
    callerEntryPc: descriptor(0x1000),
    entryCount: '0x0020000000000001',
    exits: [{
      exitSite: 0,
      exitBlockDescriptor: descriptor(0x1004),
      kind: 'external-direct-call',
      staticTargetDescriptor: descriptor(0x2000),
      count: '0x0010000000000001',
      lastDynamicTargetDescriptor: descriptor(0x2000),
      lastDynamicTargetPc: descriptor(0x2000),
    }],
  }, {
    regionId: 2,
    frontend: 'a64',
    callerEntryDescriptor: descriptor(0x1100),
    callerEntryPc: descriptor(0x1100),
    entryCount: '0x0000000000000000',
    exits: [],
  }],
});
const input = () => ({
  architecture: 'a64',
  memoryMode: '2',
  workloadSha256: 'a'.repeat(64),
  artifact: {jsSha256: 'b'.repeat(64), wasmSha256: 'c'.repeat(64)},
  native: {stdout: 'native', metrics: {crc: crc()}},
  sample: {
    haltReason: 0x01000000,
    runMs: 123,
    stdout: 'guest',
    compile: {blocks: 2, bytes: 2003},
    profile: null,
    metrics: {crc: crc()},
    regionDiagnostics: diagnostics(),
  },
});
const profile = (dispatchCount = 0x0010000000000000) => ({
  ...Object.fromEntries(profileCounters.map(key => [key, 0])),
  runCount: 1,
  stepCount: 0,
  dispatchCount,
  compiledBlocks: 2,
  emittedBytes: 2003,
});

test('CoreMark diagnostic report retains raw history and derives exact duplication', () => {
  const original = input();
  const report = createCoremarkRegionDiagnosticReport(original);
  assert.equal(report.schema, 'dynarmic.js/coremark-region-diagnostics-v1');
  assert.equal(report.regionDiagnostics, original.sample.regionDiagnostics);
  assert.equal(report.sample.regionDiagnostics, undefined);
  assert.equal(report.duplication.totals.regionCount, 2);
  assert.equal(report.duplication.totals.selectedBlockCount, 6);
  assert.equal(report.duplication.totals.uniqueDescriptorCount, 5);
  assert.equal(report.duplication.totals.duplicateSelectionCount, 1);
  assert.equal(report.duplication.totals.moduleBytes, 2003);
  assert.equal(report.duplication.totals.entryBodyBytes, 1603);
  assert.deepEqual(report.duplication.descriptors.find(d => d.descriptor === descriptor(0x800)).selectedRegionIds, [1, 2]);
  assert.equal(report.regionTransferDiagnostics, undefined);
  assert.equal(report.transferAnalysis, undefined);
  assert(!Object.hasOwn(report, 'transferConservation'));
  assert.deepEqual(original, input());
});

test('CoreMark report retains optional raw transfer history and exact analysis', () => {
  const original = input();
  original.sample.regionTransferDiagnostics = transfers();
  const before = input();
  before.sample.regionTransferDiagnostics = transfers();
  const report = createCoremarkRegionDiagnosticReport(original);
  assert.equal(report.regionTransferDiagnostics, original.sample.regionTransferDiagnostics);
  assert.equal(report.sample.regionTransferDiagnostics, undefined);
  assert.equal(report.transferAnalysis.schema, 'dynarmic.js/region-transfer-analysis-v1');
  assert.equal(report.transferAnalysis.totals.entryCount, '0x0020000000000001');
  assert.equal(report.transferAnalysis.totals.externalDirectCallCount, '0x0010000000000001');
  assert.equal(report.transferAnalysis.regions[0].regionId, 1);
  assert(!Object.hasOwn(report, 'transferConservation'));
  assert.deepEqual(original, before);
  const absentProfile = input();
  absentProfile.sample.regionTransferDiagnostics = transfers();
  delete absentProfile.sample.profile;
  assert(!Object.hasOwn(createCoremarkRegionDiagnosticReport(absentProfile), 'transferConservation'));
});

test('CoreMark transfer report retains exact Run conservation and input immutability', () => {
  const original = input();
  original.sample.regionTransferDiagnostics = transfers();
  original.sample.profile = profile();
  const before = input();
  before.sample.regionTransferDiagnostics = transfers();
  before.sample.profile = profile();
  const report = createCoremarkRegionDiagnosticReport(original);
  assert.deepEqual(report.transferConservation, {
    scope: 'Run-only',
    conserved: true,
    entryCount: '0x0020000000000001',
    dispatchCount: '0x0010000000000000',
    transferCount: '0x0010000000000001',
    expectedEntryCount: '0x0020000000000001',
  });
  assert.deepEqual(original, before);
});

test('CoreMark transfer conservation rejects mismatches and non-Run profiles', () => {
  for (const mutate of [
    value => { value.sample.profile = profile(2); },
    value => { value.sample.profile = profile(); value.sample.profile.runCount = 0; },
    value => { value.sample.profile = profile(); value.sample.profile.runCount = 2; },
    value => { value.sample.profile = profile(); value.sample.profile.stepCount = 1; },
  ]) {
    const original = input();
    original.sample.regionTransferDiagnostics = transfers();
    mutate(original);
    assert.throws(() => createCoremarkRegionDiagnosticReport(original), /conserv|Run|Step/);
  }
});

test('CoreMark transfer conservation validates every profile counter', () => {
  const mutations = [
    (profile, key) => { profile[key] = -1; },
    (profile, key) => { profile[key] = Number.MAX_SAFE_INTEGER + 1; },
    (profile, key) => { delete profile[key]; },
    (profile, key) => { profile[key] = '1'; },
  ];
  for (const counter of profileCounters) {
    for (const mutation of mutations) {
      const original = input();
      original.sample.regionTransferDiagnostics = transfers();
      original.sample.profile = profile();
      mutation(original.sample.profile, counter);
      assert.throws(() => createCoremarkRegionDiagnosticReport(original), /profile/,
        `${counter} mutation must fail`);
    }
  }
});

test('CoreMark diagnostic report checks profile totals when present', () => {
  const original = input();
  original.sample.profile = {compiledBlocks: 2, emittedBytes: 2003};
  const report = createCoremarkRegionDiagnosticReport(original);
  assert(!Object.hasOwn(report, 'transferConservation'));
  original.sample.profile.emittedBytes = 2004;
  assert.throws(() => createCoremarkRegionDiagnosticReport(original), /profile byte totals/);
});

test('CoreMark diagnostic report fails closed on missing history and inconsistent totals', () => {
  for (const mutate of [
    value => { delete value.sample.regionDiagnostics; },
    value => { value.sample.compile.blocks = 1; },
    value => { value.sample.compile.bytes = 2004; },
    value => { value.sample.metrics.crc.crcfinal = 'ffff'; },
    value => { value.sample.haltReason = 0; },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionTransferDiagnostics.regions[0].exits[0].count = 1;
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionTransferDiagnostics.regions.pop();
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionTransferDiagnostics.regions[1].frontend = 'a32';
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionDiagnostics.regions[1].frontend = 'a32';
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionTransferDiagnostics.regions[0].callerEntryDescriptor = descriptor(0x1100);
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionTransferDiagnostics.regions[0].callerEntryPc = descriptor(0x1100);
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.sample.regionTransferDiagnostics.regions[0].exits[0].exitBlockDescriptor = descriptor(0x3000);
    },
    value => {
      value.sample.regionTransferDiagnostics = transfers();
      value.architecture = 'a32';
    },
  ]) {
    const original = input();
    mutate(original);
    assert.throws(() => createCoremarkRegionDiagnosticReport(original), /region|transfer|CRC|halt/);
  }
});

test('CoreMark sample adds the transfer leaf only when published', async t => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'coremark-transfer-sample-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const binary = path.join(root, 'coremark.bin');
  fs.writeFileSync(binary, 'synthetic guest');
  const makeJit = (name, transferDiagnostics) => {
    const jit = path.join(root, name);
    fs.writeFileSync(jit, `module.exports = async () => {
      const compile = {blocks: 0, bytes: 0};
      const snapshot = {compile};
      const transfer = ${JSON.stringify(transferDiagnostics ?? null)};
      if (transfer !== null) snapshot.regionTransferDiagnostics = transfer;
      return {
        HEAPU8: new Uint8Array(200000),
        _arm_ram: () => 10000,
        _arm_ram_size: () => 100000,
        _arm_console_clear() {},
        _arm_init() {},
        _arm_run() { return 0x01000000; },
        _arm_console: () => 0,
        _arm_console_len: () => 0,
        dynarmicWasmStats: {activeInstance: 1, getInstance: () => snapshot},
      };
    };`);
    return jit;
  };

  const ordinary = await collectCoremarkSample(makeJit('ordinary.cjs', null), binary, 'a64', '2');
  assert.deepEqual(Object.keys(ordinary), ['haltReason', 'runMs', 'stdout', 'compile', 'profile']);
  assert.equal(ordinary.regionTransferDiagnostics, undefined);

  const published = transfers();
  const diagnostic = await collectCoremarkSample(makeJit('diagnostic.cjs', published), binary, 'a64', '2');
  assert.deepEqual(Object.keys(diagnostic), ['haltReason', 'runMs', 'stdout', 'compile', 'profile', 'regionTransferDiagnostics']);
  assert.deepEqual(diagnostic.regionTransferDiagnostics, published);
  assert.deepEqual({...diagnostic, regionTransferDiagnostics: undefined, runMs: ordinary.runMs},
    {...ordinary, regionTransferDiagnostics: undefined});
});
