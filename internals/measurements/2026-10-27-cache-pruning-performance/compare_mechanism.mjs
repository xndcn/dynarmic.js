#!/usr/bin/env node
// Audit the preregistered cache-pruning mechanism follow-up without timing data.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';

const [inputArg, outputArg, ...extra] = process.argv.slice(2);
assert(inputArg && outputArg && !extra.length,
  'usage: compare_mechanism.mjs <mechanism-dir> <new-comparison.json>');
const input = path.resolve(inputArg);
assert(!fs.existsSync(outputArg), `refusing to overwrite ${outputArg}`);
const load = name => JSON.parse(fs.readFileSync(path.join(input, `${name}.json`), 'utf8'));
const names = arch => arch === 'a64'
  ? [`before-${arch}-flat`, `after-${arch}-flat`]
  : [`before-${arch}-flat`, `before-${arch}-flat-repeat`,
    `after-${arch}-flat`, `after-${arch}-flat-repeat`];
const reports = Object.fromEntries(['a32', 'a64'].map(arch => [arch, names(arch).map(name => ({name, ...load(name)}))]));
for (const list of Object.values(reports)) for (const report of list) {
  assert.equal(report.sample.haltReason, 0x01000000, `${report.name}: abnormal halt`);
  assert.deepEqual(report.sample.metrics.crc, report.native.metrics.crc, `${report.name}: CRC mismatch`);
  assert.equal(report.build.profile, false, `${report.name}: profiling enabled`);
  assert.equal(report.regionDiagnostics.schema, 'dynarmic.js/region-diagnostics-v2',
    `${report.name}: missing v2 diagnostics`);
  const descriptors = report.regionDiagnostics.regions.map(region => region.entryDescriptor);
  assert.equal(new Set(descriptors).size, descriptors.length, `${report.name}: duplicate entry descriptor`);
}
const shape = region => ({
  entryDescriptor: region.entryDescriptor,
  entryPc: region.entryPc,
  entryIndex: region.entryIndex,
  layout: region.layout,
  blocks: region.blocks,
});
function compareRegions(before, after, ordered) {
  const a = before.regionDiagnostics.regions, b = after.regionDiagnostics.regions;
  if (ordered) assert.deepEqual(a.map(shape), b.map(shape), 'ordered A64 history changed');
  const afterByEntry = new Map(b.map(region => [region.entryDescriptor, region]));
  const differences = [];
  let shared = 0, shrinkingOmitted = 0;
  for (const left of a) {
    const right = afterByEntry.get(left.entryDescriptor);
    if (!right) continue;
    shared++;
    assert.deepEqual(shape(left), shape(right), `${before.name}/${after.name}: region shape changed`);
    assert(right.moduleBytes <= left.moduleBytes, `${before.name}/${after.name}: module growth`);
    assert(right.entryBodyBytes <= left.entryBodyBytes, `${before.name}/${after.name}: body growth`);
    const omitted = left.blocks.some(block => !block.emitted);
    const sizeChanged = left.moduleBytes !== right.moduleBytes || left.entryBodyBytes !== right.entryBodyBytes;
    if (sizeChanged) {
      assert(left.layout === 'structured' && omitted,
        `${before.name}/${after.name}: size changed outside an omitted-block structured region`);
    }
    if (left.layout === 'structured' && omitted && right.moduleBytes < left.moduleBytes) shrinkingOmitted++;
    if (sizeChanged) differences.push({
      entryDescriptor: left.entryDescriptor,
      layout: left.layout,
      omittedBlocks: left.blocks.filter(block => !block.emitted).length,
      beforeModuleBytes: left.moduleBytes,
      afterModuleBytes: right.moduleBytes,
      beforeEntryBodyBytes: left.entryBodyBytes,
      afterEntryBodyBytes: right.entryBodyBytes,
    });
  }
  assert(shrinkingOmitted > 0, `${before.name}/${after.name}: mechanism not exercised`);
  return {before: before.name, after: after.name, shared, shrinkingOmitted, differences};
}

const [a64Before, a64After] = reports.a64;
const a64 = compareRegions(a64Before, a64After, true);
const a32Reports = reports.a32;
const baseline = a32Reports.slice(0, 2), candidate = a32Reports.slice(2);
const entries = list => new Set(list.flatMap(report => report.regionDiagnostics.regions.map(region => region.entryDescriptor)));
const baselineEntries = entries(baseline), candidateEntries = entries(candidate);
assert.deepEqual(baselineEntries, candidateEntries, 'A32 descriptor unions differ');
const occurrences = new Map();
for (const report of a32Reports) for (const region of report.regionDiagnostics.regions) {
  if (!occurrences.has(region.entryDescriptor)) occurrences.set(region.entryDescriptor, []);
  occurrences.get(region.entryDescriptor).push({report: report.name, layout: region.layout});
}
const varying = [...occurrences].filter(([, found]) => found.length !== a32Reports.length)
  .map(([entryDescriptor, found]) => ({entryDescriptor, found}));
for (const entry of varying) for (const found of entry.found) {
  assert.equal(found.layout, 'fallback', `varying non-fallback entry: ${entry.entryDescriptor}`);
}
const a32 = [];
for (const before of baseline) for (const after of candidate) a32.push(compareRegions(before, after, false));
const comparison = {
  schema: 'dynarmic.js/cache-pruning-mechanism-v1',
  originalGate: {pass: false, reason: 'initial A32 region counts were 117 baseline and 118 candidate'},
  followupGate: {pass: true, a32VaryingEntries: varying},
  a32,
  a64,
};
fs.writeFileSync(outputArg, JSON.stringify(comparison, null, 2) + '\n', {flag: 'wx'});
console.log(`cache-pruning mechanism PASS a32Pairs=${a32.length} a32Varying=${varying.length} ` +
  `a64Shrinking=${a64.shrinkingOmitted} originalGate=FAILED`);
