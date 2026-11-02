// Recompute the retained compile-history evidence; run from the repository root.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const {analyzeRegionDiagnostics} = await import(pathToFileURL(path.join(root, 'tests/bench/region_diagnostics.mjs')));
const {coremarkIdentity, parseCoremark} = await import(pathToFileURL(path.join(root, 'tests/bench/coremark_protocol.mjs')));
const {validateEnvironment} = await import(pathToFileURL(path.join(root, 'tests/bench/environment.mjs')));
const sha256 = data => createHash('sha256').update(data).digest('hex');
const args = process.argv.slice(2);
assert(args.length === 0 || (args.length === 2 && args[0] === '--wabt-bin'),
  'usage: verify.mjs [--wabt-bin DIRECTORY]');
const wabtBin = args.length ? path.resolve(args[1]) : null;

function run(tool, toolArgs) {
  const result = spawnSync(tool, toolArgs, {encoding: 'utf8'});
  assert.equal(result.status, 0, `${tool} failed: ${result.stderr || result.stdout}`);
  return result;
}

const inventoryNames = ['a64-flat.json.gz', 'generated-wasm-comparison.json', 'generated-wasm.tar.gz'];
const inventory = fs.readFileSync(path.join(here, 'SHA256SUMS'), 'utf8').trim().split('\n');
assert.deepEqual(inventory.map(line => line.slice(66)).sort(), inventoryNames);
for (const line of inventory) {
  const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
  assert(match && inventoryNames.includes(match[2]), `invalid inventory line: ${line}`);
  assert.equal(sha256(fs.readFileSync(path.join(here, match[2]))), match[1], `inventory hash: ${match[2]}`);
}
const report = JSON.parse(gunzipSync(fs.readFileSync(path.join(here, 'a64-flat.json.gz'))));
assert.equal(report.schema, 'dynarmic.js/coremark-region-diagnostics-v1');
assert.equal(report.architecture, 'a64');
assert.equal(report.memoryMode, '2');
assert.equal(report.sample.haltReason, 0x01000000);
assert.equal(report.sample.profile, null);
assert.equal(report.build.profile, false);
assert(Number.isFinite(report.sample.runMs) && report.sample.runMs > 0);
validateEnvironment(report);
assert.equal(report.workloadSha256, coremarkIdentity(report.workload));
for (const value of [report.workloadSha256, report.artifact.jsSha256, report.artifact.wasmSha256]) {
  assert(/^[0-9a-f]{64}$/.test(value), 'invalid artifact/workload hash');
}
const native = parseCoremark(report.native.stdout, report.workload.iterations);
const guest = parseCoremark(report.sample.stdout, report.workload.iterations);
assert.deepEqual(report.native.metrics, native);
assert.deepEqual(report.sample.metrics, guest);
assert.deepEqual(guest.crc, native.crc, 'native/guest CRC mismatch');
const duplication = analyzeRegionDiagnostics(report.regionDiagnostics);
assert.deepEqual(report.duplication, duplication);
assert.equal(duplication.totals.regionCount, report.sample.compile.blocks);
assert.equal(duplication.totals.moduleBytes, report.sample.compile.bytes);

const comparison = JSON.parse(fs.readFileSync(path.join(here, 'generated-wasm-comparison.json')));
assert.equal(comparison.schema, 'dynarmic.js/generated-wasm-comparison-v1');
assert.equal(comparison.moduleCount, duplication.totals.regionCount);
assert.equal(comparison.modules.length, comparison.moduleCount);
assert.equal(comparison.rawIdenticalModuleCount, 0);
const modulesByName = new Map(comparison.modules.map(module => [module.name, module]));
assert.equal(modulesByName.size, comparison.moduleCount);
for (const region of report.regionDiagnostics.regions) {
  const module = modulesByName.get(`block_${region.regionId}.wasm`);
  assert(module, `missing generated module for region ${region.regionId}`);
  assert.equal(module.bytes, region.moduleBytes, `module byte total differs for region ${region.regionId}`);
}
assert.equal(comparison.modules.reduce((sum, module) => sum + module.bytes, 0), duplication.totals.moduleBytes);
const categories = ['flatBaseOffset', 'linkSlotIndex', 'linkSlotMissAddress',
  'rsbFastDispatchEntry', 'fastDispatchBase', 'fastDispatchMissAddress'];
assert.deepEqual(comparison.normalization.allowed, categories);
const totals = Object.fromEntries(categories.map(name => [name, 0]));
for (const module of comparison.modules) {
  assert(/^block_[1-9][0-9]*\.wasm$/.test(module.name), `invalid module name: ${module.name}`);
  assert(Number.isSafeInteger(module.bytes) && module.bytes > 0);
  for (const hash of [module.productionSha256, module.diagnosticsSha256, module.normalizedWatSha256]) {
    assert(/^[0-9a-f]{64}$/.test(hash), `invalid module hash: ${module.name}`);
  }
  for (const [name, count] of Object.entries(module.runtimeAddressDifferences)) {
    assert(categories.includes(name) && Number.isSafeInteger(count) && count >= 0);
    totals[name] += count;
  }
  const differences = module.runtimeAddressDifferences;
  assert.equal(differences.linkSlotIndex || 0, differences.linkSlotMissAddress || 0);
  assert.equal((differences.rsbFastDispatchEntry || 0) % 2, 0);
  assert.equal(module.rawEqual, module.productionSha256 === module.diagnosticsSha256);
}
assert.deepEqual(comparison.runtimeAddressDifferences, totals);
assert.equal(totals.linkSlotIndex, totals.linkSlotMissAddress);
assert.equal(totals.rsbFastDispatchEntry % 2, 0);

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-region-evidence-'));
try {
  const archive = path.join(here, 'generated-wasm.tar.gz');
  const expectedEntries = ['production/', 'diagnostics/'];
  for (const module of comparison.modules) {
    expectedEntries.push(`production/${module.name}`, `diagnostics/${module.name}`);
  }
  const listing = run('tar', ['-tvzf', archive]).stdout.trim().split('\n');
  assert.deepEqual(listing.map(line => {
    assert(line[0] === '-' || line[0] === 'd', `non-regular archive entry: ${line}`);
    return line.slice(line.lastIndexOf(' ') + 1);
  }).sort(), expectedEntries.sort());
  run('tar', ['--no-same-owner', '--no-same-permissions', '-xzf', archive, '-C', temporary]);
  for (const module of comparison.modules) {
    const production = fs.readFileSync(path.join(temporary, 'production', module.name));
    const diagnostics = fs.readFileSync(path.join(temporary, 'diagnostics', module.name));
    assert.equal(production.length, module.bytes);
    assert.equal(diagnostics.length, module.bytes);
    assert.equal(sha256(production), module.productionSha256);
    assert.equal(sha256(diagnostics), module.diagnosticsSha256);
    assert.equal(production.equals(diagnostics), module.rawEqual);
  }
  if (wabtBin) {
    const wasm2wat = path.join(wabtBin, 'wasm2wat');
    const wat2wasm = path.join(wabtBin, 'wat2wasm');
    const freshPath = path.join(temporary, 'fresh-comparison.json');
    run(process.execPath, [path.join(here, 'compare_generated_wasm.mjs'),
      path.join(temporary, 'production'), path.join(temporary, 'diagnostics'), wasm2wat, freshPath]);
    assert.deepEqual(JSON.parse(fs.readFileSync(freshPath)), comparison);

    const negative = path.join(temporary, 'negative');
    fs.mkdirSync(path.join(negative, 'production'), {recursive: true});
    fs.mkdirSync(path.join(negative, 'diagnostics'), {recursive: true});
    const name = comparison.modules[0].name;
    fs.copyFileSync(path.join(temporary, 'production', name), path.join(negative, 'production', name));
    const watPath = path.join(negative, 'original.wat');
    run(wasm2wat, ['--enable-all', path.join(temporary, 'diagnostics', name), '-o', watPath]);
    const lines = fs.readFileSync(watPath, 'utf8').split('\n');
    let rejected = false;
    for (let index = 0; index < lines.length && !rejected; ++index) {
      const match = /^(\s*i64\.const )(-?\d+)(\s*)$/.exec(lines[index]);
      if (!match) continue;
      const mutated = [...lines];
      mutated[index] = `${match[1]}${Number(match[2]) + 1}${match[3]}`;
      fs.writeFileSync(watPath, mutated.join('\n'));
      const mutatedPath = path.join(negative, 'diagnostics', name);
      run(wat2wasm, ['--enable-all', watPath, '-o', mutatedPath]);
      if (fs.statSync(mutatedPath).size !== comparison.modules[0].bytes) continue;
      const outputPath = path.join(negative, 'rejected.json');
      const result = spawnSync(process.execPath, [path.join(here, 'compare_generated_wasm.mjs'),
        path.join(negative, 'production'), path.join(negative, 'diagnostics'), wasm2wat, outputPath], {encoding: 'utf8'});
      assert.notEqual(result.status, 0, 'non-address i64.const mutation was accepted');
      assert(`${result.stdout}${result.stderr}`.includes('unexpected difference'), 'mutation rejected for the wrong reason');
      assert(!fs.existsSync(outputPath));
      rejected = true;
    }
    assert(rejected, 'no same-size non-address mutation case found');

    const linkNegative = path.join(temporary, 'link-slot-negative');
    fs.mkdirSync(path.join(linkNegative, 'production'), {recursive: true});
    fs.mkdirSync(path.join(linkNegative, 'diagnostics'), {recursive: true});
    fs.copyFileSync(path.join(temporary, 'production', name), path.join(linkNegative, 'production', name));
    const linkWatPath = path.join(linkNegative, 'original.wat');
    run(wasm2wat, ['--enable-all', path.join(temporary, 'diagnostics', name), '-o', linkWatPath]);
    const linkLines = fs.readFileSync(linkWatPath, 'utf8').split('\n');
    let linkRejected = false;
    for (let index = 2; index < linkLines.length && !linkRejected; ++index) {
      if (!/^\s*i32\.store offset=8(?:\s|$)/.test(linkLines[index]) ||
          !/^\s*local\.get /.test(linkLines[index - 2])) continue;
      const match = /^(\s*i32\.const )(-?\d+)(\s*)$/.exec(linkLines[index - 1]);
      if (!match) continue;
      const mutated = [...linkLines];
      mutated[index - 1] = `${match[1]}${Number(match[2]) + 1}${match[3]}`;
      fs.writeFileSync(linkWatPath, mutated.join('\n'));
      const mutatedPath = path.join(linkNegative, 'diagnostics', name);
      run(wat2wasm, ['--enable-all', linkWatPath, '-o', mutatedPath]);
      if (fs.statSync(mutatedPath).size !== comparison.modules[0].bytes) continue;
      const outputPath = path.join(linkNegative, 'rejected.json');
      const result = spawnSync(process.execPath, [path.join(here, 'compare_generated_wasm.mjs'),
        path.join(linkNegative, 'production'), path.join(linkNegative, 'diagnostics'), wasm2wat, outputPath], {encoding: 'utf8'});
      assert.notEqual(result.status, 0, 'mismatched link-slot miss address was accepted');
      assert(`${result.stdout}${result.stderr}`.includes('link-slot address mismatch'),
        'link-slot mutation rejected for the wrong reason');
      assert(!fs.existsSync(outputPath));
      linkRejected = true;
    }
    assert(linkRejected, 'no same-size link-slot mismatch case found');
  }
} finally {
  fs.rmSync(temporary, {recursive: true, force: true});
}
console.log(`region-diagnostics evidence PASS regions=${duplication.totals.regionCount} ` +
  `descriptors=${duplication.totals.uniqueDescriptorCount} modules=${comparison.moduleCount} files=${inventory.length}`);
if (wabtBin) console.log('generated-Wasm fresh comparison and mutation rejection PASS');
