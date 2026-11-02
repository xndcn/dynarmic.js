// Recompute saved CPU summaries and validate retained diagnostic evidence.
// Run from the repository root: node <this-file> <extracted-evidence-dir>
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const root = process.argv[2];
assert(root && process.argv.length === 3, 'usage: verify.mjs <extracted-evidence-dir>');
const {summarizeCpuProfile} = await import(pathToFileURL(path.resolve('tests/bench/cpu_profile.mjs')));
const {validateReport} = await import(pathToFileURL(path.resolve('tests/bench/protocol.mjs')));
const {validateCoremarkReport} = await import(pathToFileURL(path.resolve('tests/bench/coremark_protocol.mjs')));
const readJson = name => JSON.parse(fs.readFileSync(path.join(root, name), 'utf8'));
const sha256 = data => createHash('sha256').update(data).digest('hex');
const names = ['a32-thunk', 'a32-page-table', 'a32-flat', 'a64-thunk', 'a64-page-table', 'a64-flat'];

for (const name of names) {
  const raw = fs.readFileSync(path.join(root, `${name}.cpuprofile`));
  const profile = JSON.parse(raw);
  for (const [suffix, pattern] of [['all', null], ['guest', `^${name.slice(0, 3)}_`]]) {
    const saved = readJson(`${name}-${suffix}.json`);
    assert.equal(saved.input.sha256, sha256(raw), `${name}: input hash`);
    const {input, ...expected} = saved;
    assert.deepEqual(summarizeCpuProfile(profile, pattern), expected, `${name}: ${suffix} summary`);
  }
  const text = fs.readFileSync(path.join(root, `${name}-sample.txt`), 'utf8');
  assert.match(text, /^COREMARK_SAMPLE \{[^\n]+\}\n$/);
  const sample = JSON.parse(text.slice('COREMARK_SAMPLE '.length));
  assert.equal(sample.haltReason, 0x01000000, `${name}: halt`);
  assert(Number.isFinite(sample.runMs) && sample.runMs > 0, `${name}: Run time`);
  assert.match(sample.stdout, /Iterations       : 120000\n/);
  assert.match(sample.stdout, /\[0\]crcfinal\s+: 0x988c\n/);
  assert.match(sample.stdout, /Correct operation validated\./);
}

const observations = readJson('observations.json');
validateReport(readJson('baseline-fixed.json'));
validateCoremarkReport(readJson('baseline-coremark.json'), true);
const trace = fs.readFileSync(path.join(root, 'a64-flat-tiering.log'), 'utf8');
for (const region of observations.hotRegions) {
  assert(region.turboFanObserved && Number.isSafeInteger(region.bodyBytes));
  const copies = observations.hotRegions.filter(other => other.bodyBytes === region.bodyBytes).length;
  const records = trace.match(new RegExp(`using TurboFan,.*bodysize ${region.bodyBytes} `, 'g')) || [];
  assert(records.length >= copies, `${region.functionName}: missing TurboFan record`);
  if (region.dumpBlock !== 33 && region.dumpBlock !== 38) {
    const wat = fs.readFileSync(path.join(root, `a64-block-${region.dumpBlock}.wat`), 'utf8');
    assert(wat.includes(`(func $${region.functionName} `), `${region.functionName}: WAT identity`);
  }
}
const counters = JSON.parse(fs.readFileSync(path.join(root, 'a64-flat-profile-counters.txt'), 'utf8')
  .slice('COREMARK_SAMPLE '.length));
for (const [key, value] of Object.entries(observations.a64FlatProfileCounters)) {
  assert.equal(counters.profile[key], value, `profile counter ${key}`);
}

const inventoryName = 'SHA256SUMS';
const inventory = fs.readFileSync(path.join(root, inventoryName), 'utf8').trim().split('\n');
for (const line of inventory) {
  const match = line.match(/^([0-9a-f]{64})  (.+)$/);
  assert(match, `invalid inventory line: ${line}`);
  const data = fs.readFileSync(path.join(root, match[2]));
  assert.equal(sha256(data), match[1], `inventory hash: ${match[2]}`);
}
console.log(`runtime CPU evidence PASS profiles=${names.length} files=${inventory.length}`);
