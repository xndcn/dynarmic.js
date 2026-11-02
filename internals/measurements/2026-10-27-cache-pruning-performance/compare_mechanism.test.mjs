import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import {projectRegionDiagnostics} from '../../../tests/bench/region_diagnostics.mjs';

const script = fileURLToPath(new URL('./compare_mechanism.mjs', import.meta.url));
const names = [
  'before-a32-flat', 'before-a32-flat-repeat', 'after-a32-flat', 'after-a32-flat-repeat',
  'before-a64-flat', 'after-a64-flat',
];

function descriptor(value) {
  return `0x${value.toString(16).padStart(16, '0')}`;
}

function region(entry, {layout = 'structured', omitted = true, bytes = 100} = {}) {
  return {
    regionId: 1,
    frontend: 'a64',
    entryDescriptor: descriptor(entry),
    entryPc: descriptor(entry),
    entryIndex: 0,
    layout,
    moduleBytes: bytes,
    entryBodyBytes: bytes,
    liveRegionIdsBeforeCompile: [],
    blocks: [
      {descriptor: descriptor(entry), emitted: true, entryReachable: true, returnSite: false},
      {descriptor: descriptor(entry + 4), emitted: true, entryReachable: true, returnSite: false},
      ...(omitted ? [{descriptor: descriptor(entry + 8), emitted: false, entryReachable: false, returnSite: false}] : []),
    ],
  };
}

function report(regions) {
  const regionDiagnostics = {
    schema: 'dynarmic.js/region-diagnostics-v2',
    regions: regions.map((value, index) => ({...value, regionId: index + 1})),
  };
  projectRegionDiagnostics(regionDiagnostics);
  return {
    sample: {haltReason: 0x01000000, metrics: {crc: {}}},
    native: {metrics: {crc: {}}},
    build: {profile: false},
    regionDiagnostics,
  };
}

function fixture(t, mutate = () => {}) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'cache-pruning-mechanism-test-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const reports = Object.fromEntries(names.map(name => [name, report([region(0x1000, {bytes: name.startsWith('before') ? 100 : 90})])]));
  mutate(reports);
  for (const [name, value] of Object.entries(reports)) {
    value.regionDiagnostics.regions = value.regionDiagnostics.regions.map((item, index) =>
      ({...item, regionId: index + 1}));
    projectRegionDiagnostics(value.regionDiagnostics);
    fs.writeFileSync(path.join(root, `${name}.json`), JSON.stringify(value));
  }
  return root;
}

function run(t, mutate) {
  const root = fixture(t, mutate);
  const output = path.join(root, 'comparison.json');
  const result = spawnSync(process.execPath, [script, root, output], {encoding: 'utf8'});
  return {result, output};
}

test('accepts shrinking only an omitted-block structured region', t => {
  const {result, output} = run(t);
  assert.equal(result.status, 0, result.stderr);
  const comparison = JSON.parse(fs.readFileSync(output, 'utf8'));
  assert.equal(comparison.originalGate.pass, false);
  assert.equal(comparison.followupGate.pass, true);
  assert.equal(comparison.a32.length, 4);
});

test('rejects duplicate entry descriptors before map construction', t => {
  const {result} = run(t, reports => {
    reports['after-a32-flat'].regionDiagnostics.regions.push(region(0x1000, {layout: 'fallback', omitted: false}));
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /duplicate entry descriptor/);
});

test('rejects shrinking a structured region without omitted blocks', t => {
  const {result} = run(t, reports => {
    for (const name of ['before-a64-flat', 'after-a64-flat']) {
      reports[name].regionDiagnostics.regions.push(region(0x2000, {
        omitted: false, bytes: name.startsWith('before') ? 100 : 90,
      }));
    }
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /size changed outside an omitted-block structured region/);
});

test('retains the per-region growth rejection', t => {
  const {result} = run(t, reports => {
    reports['after-a64-flat'].regionDiagnostics.regions[0].moduleBytes = 101;
  });
  assert.notEqual(result.status, 0);
  assert.match(result.stderr, /module growth/);
});
