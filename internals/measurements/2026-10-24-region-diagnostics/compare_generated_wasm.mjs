#!/usr/bin/env node
// Compare emitted modules while masking only proven embedded runtime addresses.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const [productionArg, diagnosticsArg, wasm2watArg, outputArg, ...extra] = process.argv.slice(2);
assert(productionArg && diagnosticsArg && wasm2watArg && outputArg && !extra.length,
  'usage: compare_generated_wasm.mjs <production-dir> <diagnostics-dir> <wasm2wat> <new-output.json>');
const productionDir = path.resolve(productionArg);
const diagnosticsDir = path.resolve(diagnosticsArg);
const wasm2wat = path.resolve(wasm2watArg);
assert(!fs.existsSync(outputArg), `refusing to overwrite ${outputArg}`);
const names = fs.readdirSync(productionDir).filter(name => name.endsWith('.wasm')).sort();
assert(names.length > 0, 'no production Wasm modules');
assert.deepEqual(fs.readdirSync(diagnosticsDir).filter(name => name.endsWith('.wasm')).sort(), names,
  'module inventories differ');
const sha256 = data => createHash('sha256').update(data).digest('hex');
const opcode = line => line.trim().split(/\s+/, 1)[0];
const numericShape = line => line.replace(/-?\d+/g, '<n>');
const constant = line => {
  const match = /^\s*i32\.const (-?\d+)\s*$/.exec(line);
  assert(match, `expected i32.const: ${line}`);
  return Number(match[1]);
};
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-wat-compare-'));
const totals = {
  flatBaseOffset: 0,
  linkSlotIndex: 0,
  linkSlotMissAddress: 0,
  rsbFastDispatchEntry: 0,
  fastDispatchBase: 0,
  fastDispatchMissAddress: 0,
};
let flatBasePair = null;

function wat(modulePath, target) {
  const result = spawnSync(wasm2wat, ['--enable-all', modulePath, '-o', target], {encoding: 'utf8'});
  assert.equal(result.status, 0, `wasm2wat failed for ${modulePath}: ${result.stderr}`);
  return fs.readFileSync(target, 'utf8').split('\n');
}

function classifyConstant(lines, index) {
  const ops = lines.map(opcode);
  const before = ops.slice(Math.max(0, index - 3), index);
  const after = ops.slice(index + 1, index + 6);
  if (after[0] === 'i32.load' && after[1] === 'local.tee' &&
      after[2] === 'i32.eqz' && after[3] === 'i32.eqz') {
    return 'linkSlotIndex';
  }
  if (after[0] === 'i32.store' && /^\s*i32\.store offset=8(?:\s|$)/.test(lines[index + 1]) &&
      before.at(-1) === 'local.get') {
    return 'linkSlotMissAddress';
  }
  if (after[0] === 'i32.store' && /^\s*i32\.store offset=12(?:\s|$)/.test(lines[index + 1]) &&
      before.at(-1) === 'local.get') {
    return 'fastDispatchMissAddress';
  }
  if (after[0] === 'i32.load' && /^\s*i32\.load offset=8(?:\s|$)/.test(lines[index + 1]) &&
      after[1] === 'i32.const' && after[2] === 'i32.const' && after[3] === 'i64.load' &&
      constant(lines[index]) === constant(lines[index + 3])) {
    return 'rsbFastDispatchEntry';
  }
  if (after[0] === 'i64.load' && before.at(-3) === 'i32.const' &&
      before.at(-2) === 'i32.load' && before.at(-1) === 'i32.const' &&
      constant(lines[index]) === constant(lines[index - 3])) {
    return 'rsbFastDispatchEntry';
  }
  if (before.at(-1) === 'i32.shl' && after[0] === 'i32.add' && after[1] === 'local.tee' &&
      after[2] === 'i64.load') {
    return 'fastDispatchBase';
  }
  return null;
}

function validateLinkSlotAddresses(lines, name) {
  const loads = [];
  const stores = [];
  for (let index = 0; index < lines.length; ++index) {
    if (opcode(lines[index]) !== 'i32.const') continue;
    const category = classifyConstant(lines, index);
    if (category === 'linkSlotIndex') loads.push(index);
    if (category === 'linkSlotMissAddress') stores.push(index);
  }
  assert.equal(loads.length, stores.length, `${name}: unpaired link-slot addresses`);
  for (let index = 0; index < loads.length; ++index) {
    assert.equal(constant(lines[loads[index]]), constant(lines[stores[index]]),
      `${name}: link-slot address mismatch at lines ${loads[index] + 1} and ${stores[index] + 1}`);
  }
}

try {
  const modules = [];
  for (const name of names) {
    const productionBytes = fs.readFileSync(path.join(productionDir, name));
    const diagnosticBytes = fs.readFileSync(path.join(diagnosticsDir, name));
    assert.equal(productionBytes.length, diagnosticBytes.length, `${name}: module sizes differ`);
    const production = wat(path.join(productionDir, name), path.join(temporary, 'production.wat'));
    const diagnostics = wat(path.join(diagnosticsDir, name), path.join(temporary, 'diagnostics.wat'));
    validateLinkSlotAddresses(production, `${name}: production`);
    validateLinkSlotAddresses(diagnostics, `${name}: diagnostics`);
    assert.equal(production.length, diagnostics.length, `${name}: WAT line counts differ`);
    const normalizedProduction = [...production];
    const normalizedDiagnostics = [...diagnostics];
    const differences = {};
    for (let index = 0; index < production.length; ++index) {
      if (production[index] === diagnostics[index]) continue;
      assert.equal(numericShape(production[index]), numericShape(diagnostics[index]),
        `${name}:${index + 1}: non-numeric WAT difference\n${production[index]}\n${diagnostics[index]}`);
      const opcodeName = opcode(production[index]);
      if (opcodeName === 'i32.const') {
        const category = classifyConstant(production, index);
        assert(category, `${name}:${index + 1}: unclassified i32.const difference: ${production[index]}`);
        assert.equal(classifyConstant(diagnostics, index), category, `${name}:${index + 1}: address contexts differ`);
        normalizedProduction[index] = `i32.const <runtime:${category}>`;
        normalizedDiagnostics[index] = normalizedProduction[index];
        totals[category]++;
        differences[category] = (differences[category] || 0) + 1;
        continue;
      }
      const left = /^\s*(?:i32|i64|v128)\.(?:load|store)(?:8|16|32|64)?(?:_[su])? offset=(\d+)(?:\s|$)/.exec(production[index]);
      const right = /^\s*(?:i32|i64|v128)\.(?:load|store)(?:8|16|32|64)?(?:_[su])? offset=(\d+)(?:\s|$)/.exec(diagnostics[index]);
      assert(left && right, `${name}:${index + 1}: unexpected difference\n${production[index]}\n${diagnostics[index]}`);
      const pair = [Number(left[1]), Number(right[1])];
      if (flatBasePair === null) flatBasePair = pair;
      assert.deepEqual(pair, flatBasePair, `${name}:${index + 1}: inconsistent flat-window base`);
      normalizedProduction[index] = production[index].replace(/offset=\d+/, 'offset=<runtime:flatBase>');
      normalizedDiagnostics[index] = diagnostics[index].replace(/offset=\d+/, 'offset=<runtime:flatBase>');
      totals.flatBaseOffset++;
      differences.flatBaseOffset = (differences.flatBaseOffset || 0) + 1;
    }
    assert.deepEqual(normalizedDiagnostics, normalizedProduction, `${name}: normalized WAT differs`);
    assert.equal(differences.linkSlotIndex || 0, differences.linkSlotMissAddress || 0,
      `${name}: unpaired link-slot addresses`);
    assert.equal((differences.rsbFastDispatchEntry || 0) % 2, 0,
      `${name}: unpaired RSB fast-dispatch addresses`);
    modules.push({
      name,
      bytes: productionBytes.length,
      rawEqual: productionBytes.equals(diagnosticBytes),
      productionSha256: sha256(productionBytes),
      diagnosticsSha256: sha256(diagnosticBytes),
      normalizedWatSha256: sha256(normalizedProduction.join('\n')),
      runtimeAddressDifferences: differences,
    });
  }
  assert(flatBasePair, 'no flat-window base differences found');
  assert.equal(totals.linkSlotIndex, totals.linkSlotMissAddress, 'unpaired link-slot addresses');
  assert.equal(totals.rsbFastDispatchEntry % 2, 0, 'unpaired RSB fast-dispatch addresses');
  const report = {
    schema: 'dynarmic.js/generated-wasm-comparison-v1',
    wasm2watVersion: spawnSync(wasm2wat, ['--version'], {encoding: 'utf8'}).stdout.trim(),
    normalization: {
      flatBasePair,
      allowed: ['flatBaseOffset', 'linkSlotIndex', 'linkSlotMissAddress',
        'rsbFastDispatchEntry', 'fastDispatchBase', 'fastDispatchMissAddress'],
      semantics: 'Only embedded flat-window, link-slot and fast-dispatch addresses are masked; all other WAT text must match.',
    },
    moduleCount: modules.length,
    rawIdenticalModuleCount: modules.filter(module => module.rawEqual).length,
    runtimeAddressDifferences: totals,
    modules,
  };
  fs.writeFileSync(outputArg, JSON.stringify(report, null, 2) + '\n', {flag: 'wx'});
  console.log(`generated-Wasm comparison PASS modules=${modules.length} rawIdentical=${report.rawIdenticalModuleCount} ` +
    `runtimeAddressDifferences=${Object.values(totals).reduce((sum, value) => sum + value, 0)}`);
} finally {
  fs.rmSync(temporary, {recursive: true, force: true});
}
