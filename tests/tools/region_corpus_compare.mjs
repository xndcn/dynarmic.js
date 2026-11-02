import {createHash} from 'node:crypto';
import {readFileSync} from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const schema = 'dynarmic.js/region-corpus-v1';
const baselineRevision = '2bd45b378e205baeffe797a3d5bc320769d53167';
const descriptorPattern = /^0x[0-9a-f]{16}$/;

function requireCondition(condition, message) {
  if (!condition) throw new Error(message);
}

function loadManifest(directory, baseline = false) {
  const value = JSON.parse(readFileSync(path.join(directory, 'manifest.json'), 'utf8'));
  requireCondition(value.schema === schema, `${directory}: unsupported corpus schema`);
  requireCondition(value.source_revision === null ||
    (typeof value.source_revision === 'string' && /^[0-9a-f]{40}$/.test(value.source_revision)),
    `${directory}: invalid source revision`);
  requireCondition(!baseline || value.source_revision === baselineRevision,
    `${directory}: baseline source revision must be pinned to ${baselineRevision}`);
  requireCondition(value.emission && typeof value.emission === 'object', `${directory}: missing emission settings`);
  requireCondition(Array.isArray(value.cases) && value.cases.length > 0, `${directory}: cases must be a nonempty array`);
  const names = new Set();
  for (const item of value.cases) {
    requireCondition(item && typeof item.name === 'string' && item.name.length > 0, `${directory}: invalid case name`);
    requireCondition(!names.has(item.name), `${directory}: duplicate case ${item.name}`);
    names.add(item.name);
    requireCondition(typeof item.entry_descriptor === 'string' && descriptorPattern.test(item.entry_descriptor),
      `${item.name}: invalid entry descriptor`);
    requireCondition(Array.isArray(item.descriptors) && item.descriptors.length > 0 &&
      item.descriptors.every(descriptor => typeof descriptor === 'string' && descriptorPattern.test(descriptor)),
    `${item.name}: descriptors must be full 64-bit hexadecimal strings`);
    requireCondition(Number.isInteger(item.entry_index) && item.entry_index >= 0 && item.entry_index < item.descriptors.length,
      `${item.name}: invalid entry_index`);
    requireCondition(typeof item.wasm === 'string' && path.basename(item.wasm) === item.wasm && item.wasm.endsWith('.wasm'),
      `${item.name}: Wasm path must be a local file name`);
    requireCondition(Number.isInteger(item.wasm_bytes) && item.wasm_bytes >= 8, `${item.name}: invalid Wasm size`);
  }
  return value;
}

function compareMetadata(baseline, candidate) {
  requireCondition(JSON.stringify(candidate.emission) === JSON.stringify(baseline.emission), 'emission settings changed');
  requireCondition(candidate.cases.length === baseline.cases.length,
    `case set mismatch: missing case or extra case (baseline ${baseline.cases.length}, candidate ${candidate.cases.length})`);
  for (let index = 0; index < baseline.cases.length; ++index) {
    const before = baseline.cases[index], after = candidate.cases[index];
    requireCondition(after.name === before.name,
      `case order or case set mismatch at ${index}: expected ${before.name}, got ${after.name}`);
    requireCondition(JSON.stringify(after.descriptors) === JSON.stringify(before.descriptors),
      `${before.name}: reordered descriptors or changed descriptor selection`);
    requireCondition(after.entry_index === before.entry_index,
      `${before.name}: wrong entry_index (expected ${before.entry_index}, got ${after.entry_index})`);
    requireCondition(after.entry_descriptor === before.entry_descriptor,
      `${before.name}: wrong entry descriptor`);
    requireCondition(after.wasm === before.wasm, `${before.name}: Wasm file name changed`);
  }
}

function readModule(directory, item) {
  const bytes = readFileSync(path.join(directory, item.wasm));
  requireCondition(bytes.length === item.wasm_bytes,
    `${item.name}: Wasm size mismatch in ${directory} (manifest ${item.wasm_bytes}, actual ${bytes.length})`);
  requireCondition(bytes.length >= 8 && bytes.subarray(0, 8).equals(Buffer.from([0, 97, 115, 109, 1, 0, 0, 0])),
    `${item.name}: invalid Wasm module header in ${directory}`);
  return bytes;
}

export function compareRegionCorpora(baselineDirectory, candidateDirectory) {
  const baseline = loadManifest(baselineDirectory, true);
  const candidate = loadManifest(candidateDirectory);
  compareMetadata(baseline, candidate);
  const cases = [];
  for (let index = 0; index < baseline.cases.length; ++index) {
    const before = baseline.cases[index], after = candidate.cases[index];
    const expected = readModule(baselineDirectory, before);
    const actual = readModule(candidateDirectory, after);
    requireCondition(actual.equals(expected), `${before.name}: raw Wasm bytes differ (Buffer.equals failed)`);
    cases.push({name: before.name, bytes: actual.length,
      sha256: createHash('sha256').update(actual).digest('hex')});
  }
  return {pass: true, cases};
}

function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2) {
    console.error(`usage: ${path.basename(process.argv[1])} BASELINE_DIR CANDIDATE_DIR`);
    process.exitCode = 2;
    return;
  }
  try {
    const result = compareRegionCorpora(args[0], args[1]);
    for (const item of result.cases) console.log(`PASS ${item.name} ${item.bytes} bytes sha256=${item.sha256}`);
  } catch (error) {
    console.error(`FAIL region corpus: ${error.message}`);
    process.exitCode = 1;
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
