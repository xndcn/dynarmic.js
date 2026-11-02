#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {validateNativeReport} from '../../../tests/bench/native_protocol.mjs';

const digest = data => createHash('sha256').update(data).digest('hex');
const read = name => fs.readFileSync(new URL(name, import.meta.url));
const bytes = read('report.json.gz');
assert.equal(digest(bytes), '1c497db12e9debd88159823c0adcc23ac6ae4375abafaa2855bd543ee3a91e9c');
const json = gunzipSync(bytes);
assert.equal(digest(json), '8fb31ec23b5d5adacd59105e930678a1d6a430566ac05eab08b3d907c3890eba');
const report = validateNativeReport(JSON.parse(json), true);
// Independently audited by Unicorn's per-instruction hook, including SVC.
// Pin the archived workload counts as well as validating its report protocol.
const counts = {
  'memory-thunk': 3082002, 'memory-page-table': 3082002, 'memory-flat-window': 3082002,
  'call-return': 5242883,
  'a64-memory-thunk': 2568003, 'a64-memory-page-table': 2568003, 'a64-memory-flat-window': 2568003,
  'a64-neon': 4194310, 'a64-scalar': 4194308,
  'thumb-memory-thunk': 32803, 'thumb-memory-page-table': 32803, 'thumb-memory-flat-window': 32803,
};
for (const item of report.cases) for (const pair of item.pairs) for (const side of ['native', 'wasm']) {
  for (const sample of pair[side].samples) assert.equal(sample.instructions, counts[item.name]);
}

// Keep the rejected initial observation immutable and explicitly outside the
// accepted cohort. Its incorrect A32 instruction denominator is not a score.
const rejectedBytes = read('invalid-instruction-count.json.gz');
assert.equal(digest(rejectedBytes), 'c92d4ba61f02af2a1f35fa0c8d9f043706eb3ba5c5b53dff08db46170399f60f');
const rejected = JSON.parse(gunzipSync(rejectedBytes));
assert.equal(rejected.cases.find(c => c.name === 'memory-thunk').pairs[0].native.samples[0].instructions, 3080003);
console.log(`native comparison evidence PASS: ${report.cases.length} cases, ${report.runs} process pairs, all 1080 samples retained; initial invalid cohort preserved`);
