#!/usr/bin/env node
// Recompute retained compile-boundary evidence; GO is not a performance verdict.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {analyzeReports} from './analyze.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const {validateEnvironment} = await import(pathToFileURL(path.join(root, 'tests/bench/environment.mjs')));
const EXPECTED_FILES = ['a64-flat-v2.json.gz', 'analysis.json', 'commands.md',
  'generated-wasm-comparison.json', 'generated-wasm.tar.gz'];
const COVERED_REGION_IDS = [22, 23, 26, 27, 28, 29, 33, 38, 42, 43, 44, 52, 53, 72, 80, 81];
const ADDRESS_CATEGORIES = ['flatBaseOffset', 'linkSlotIndex', 'linkSlotMissAddress',
  'rsbFastDispatchEntry', 'fastDispatchBase', 'fastDispatchMissAddress'];
const EXPECTED_ADDRESS_DIFFERENCES = {
  flatBaseOffset: 2273,
  linkSlotIndex: 278,
  linkSlotMissAddress: 278,
  rsbFastDispatchEntry: 460,
  fastDispatchBase: 97,
  fastDispatchMissAddress: 0,
};
const FROZEN_SHA256 = {
  'a64-flat-v2.json.gz': '56473e4e42f9a4495a9c6990119a7b5c7248f55068e3aa40a3c4780913816a55',
  'analysis.json': '66cdf7480b82798c9ca277c733fe4f2edc4bd7e42f0ea382eb566318be9105d1',
  'generated-wasm-comparison.json': '1cd0fb80ae6b51e8dccbf7f2482fac7d3eb22b0d7daaf1ddb6b687096a6382e5',
  'generated-wasm.tar.gz': '4058fc64a2e49797a678e7b3a762f69a4e6915a549e4296f025e6325406caf36',
};
const BASELINE_SHA256 = '8c29c74c7b7ba4788ba11847a9ffd9d86f1ec39e0217c1d9251dee595e805874';
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readGzip = file => JSON.parse(gunzipSync(fs.readFileSync(file)));

function verifyInventory(directory) {
  const lines = fs.readFileSync(path.join(directory, 'SHA256SUMS'), 'utf8').trim().split('\n');
  const entries = lines.map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match, `invalid inventory line: ${line}`);
    return {hash: match[1], name: match[2]};
  });
  assert.deepEqual(entries.map(entry => entry.name).sort(), [...EXPECTED_FILES].sort(), 'inventory files');
  for (const entry of entries) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, entry.name))), entry.hash,
      `inventory hash: ${entry.name}`);
  }
}

function verifyGeneratedComparison(directory, report) {
  const comparison = JSON.parse(fs.readFileSync(
    path.join(directory, 'generated-wasm-comparison.json'), 'utf8'));
  assert.equal(comparison.schema, 'dynarmic.js/generated-wasm-comparison-v1');
  assert.equal(comparison.wasm2watVersion, '1.0.39');
  assert.deepEqual(comparison.normalization.allowed, ADDRESS_CATEGORIES);
  assert.deepEqual(comparison.normalization.flatBasePair, [223236, 227076]);
  assert.equal(comparison.moduleCount, report.regionDiagnostics.regions.length);
  assert.equal(comparison.modules.length, comparison.moduleCount);
  assert.equal(comparison.rawIdenticalModuleCount, 0);
  const modules = new Map(comparison.modules.map(module => [module.name, module]));
  assert.equal(modules.size, comparison.moduleCount, 'duplicate generated module names');
  for (const region of report.regionDiagnostics.regions) {
    const module = modules.get(`block_${region.regionId}.wasm`);
    assert(module, `missing generated module for region ${region.regionId}`);
    assert.equal(module.bytes, region.moduleBytes, `module bytes for region ${region.regionId}`);
  }
  const totals = Object.fromEntries(ADDRESS_CATEGORIES.map(name => [name, 0]));
  for (const module of comparison.modules) {
    assert(/^block_[1-9][0-9]*\.wasm$/.test(module.name), 'generated module name');
    assert(Number.isSafeInteger(module.bytes) && module.bytes > 0, 'generated module size');
    for (const hash of [module.productionSha256, module.diagnosticsSha256, module.normalizedWatSha256]) {
      assert(/^[0-9a-f]{64}$/.test(hash), `generated module hash: ${module.name}`);
    }
    for (const [name, count] of Object.entries(module.runtimeAddressDifferences)) {
      assert(ADDRESS_CATEGORIES.includes(name), `generated address category: ${name}`);
      assert(Number.isSafeInteger(count) && count >= 0, `generated address count: ${name}`);
      totals[name] += count;
    }
    const differences = module.runtimeAddressDifferences;
    assert.equal(differences.linkSlotIndex || 0, differences.linkSlotMissAddress || 0,
      `generated link-slot counts: ${module.name}`);
    assert.equal((differences.rsbFastDispatchEntry || 0) % 2, 0,
      `generated RSB counts: ${module.name}`);
    assert.equal(module.rawEqual, module.productionSha256 === module.diagnosticsSha256);
  }
  assert.deepEqual(comparison.runtimeAddressDifferences, totals);
  assert.deepEqual(totals, EXPECTED_ADDRESS_DIFFERENCES, 'generated address totals');
  assert.equal(comparison.modules.reduce((sum, module) => sum + module.bytes, 0),
    report.sample.compile.bytes, 'generated module byte total');
  return comparison;
}

function verifyGeneratedArchive(directory, comparison) {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-alias-wasm-'));
  try {
    const archive = path.join(directory, 'generated-wasm.tar.gz');
    const listing = spawnSync('tar', ['-tvzf', archive], {encoding: 'utf8'});
    assert.equal(listing.status, 0, `generated archive listing failed: ${listing.stderr}`);
    const expected = ['production/', 'diagnostics/', ...comparison.modules.flatMap(module =>
      [`production/${module.name}`, `diagnostics/${module.name}`])];
    assert.deepEqual(listing.stdout.trim().split('\n').map(line => {
      assert(line[0] === '-' || line[0] === 'd', `non-regular generated archive entry: ${line}`);
      return line.slice(line.lastIndexOf(' ') + 1);
    }).sort(), expected.sort(), 'generated archive inventory');
    const extracted = spawnSync('tar', ['--no-same-owner', '--no-same-permissions',
      '-xzf', archive, '-C', temporary], {encoding: 'utf8'});
    assert.equal(extracted.status, 0, `generated archive extraction failed: ${extracted.stderr}`);
    for (const module of comparison.modules) {
      const production = fs.readFileSync(path.join(temporary, 'production', module.name));
      const diagnostics = fs.readFileSync(path.join(temporary, 'diagnostics', module.name));
      assert.equal(production.length, module.bytes, `production module size: ${module.name}`);
      assert.equal(diagnostics.length, module.bytes, `diagnostic module size: ${module.name}`);
      assert.equal(sha256(production), module.productionSha256, `production module hash: ${module.name}`);
      assert.equal(sha256(diagnostics), module.diagnosticsSha256, `diagnostic module hash: ${module.name}`);
      assert.equal(production.equals(diagnostics), module.rawEqual, `raw equality: ${module.name}`);
    }
  } finally {
    fs.rmSync(temporary, {recursive: true, force: true});
  }
}

function verifyContent(directory, baselineFile) {
  assert.equal(sha256(fs.readFileSync(baselineFile)), BASELINE_SHA256, 'pinned retained v1 evidence');
  const baseline = readGzip(baselineFile);
  const current = readGzip(path.join(directory, 'a64-flat-v2.json.gz'));
  validateEnvironment(current);
  const fresh = analyzeReports(baseline, current);
  const retained = JSON.parse(fs.readFileSync(path.join(directory, 'analysis.json'), 'utf8'));
  assert.deepEqual(retained, fresh, 'fresh analysis differs from retained analysis');
  assert.deepEqual(fresh.coverage.totals, {
    regionCount: 111,
    laterEntryCount: 82,
    coveredLaterEntryCount: 16,
  });
  assert.equal(fresh.coverage.decision, 'GO');
  assert.deepEqual(fresh.coverage.laterEntries.filter(row => row.covered).map(row => row.regionId),
    COVERED_REGION_IDS, 'covered region cohort');
  const comparison = verifyGeneratedComparison(directory, current);
  verifyGeneratedArchive(directory, comparison);
  return {regions: 111, laterEntries: 82, covered: 16, files: EXPECTED_FILES.length};
}

const defaultBaseline = directory =>
  path.join(directory, '../2026-10-24-region-diagnostics/a64-flat.json.gz');

// Exercise deep validators independently; the CLI always uses the frozen gate.
export function verifyEvidenceContent(directory, baselineFile = defaultBaseline(directory)) {
  verifyInventory(directory);
  return verifyContent(directory, baselineFile);
}

export function verifyEvidence(directory, baselineFile = defaultBaseline(directory)) {
  verifyInventory(directory);
  for (const [name, expected] of Object.entries(FROZEN_SHA256)) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, name))), expected,
      `pinned evidence bytes: ${name}`);
  }
  return verifyContent(directory, baselineFile);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  assert(process.argv.length === 2, 'usage: verify.mjs');
  const result = verifyEvidence(here);
  console.log(`region-alias evidence GO regions=${result.regions} laterEntries=${result.laterEntries} ` +
    `covered=${result.covered} files=${result.files}`);
}
