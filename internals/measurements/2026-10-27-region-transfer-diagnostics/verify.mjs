#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';
import {gunzipSync} from 'node:zlib';
import {analyzeEvidence} from './analyze.mjs';

const EXPECTED_FILES = ['a64-flat-window.json.gz', 'a64-page-table.json.gz', 'analysis.json', 'commands.md'];
const FROZEN_SHA256 = {
  'a64-flat-window.json.gz': 'd8aa6a432c4fc56ec2aad2c83b99a38f7c6f9c00272490b3415014ab6d3c08f4',
  'a64-page-table.json.gz': '849f0db7842af11f97c74971594aab101f77834a8fa1660ab88c5c3082e4e84f',
};
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readReport = file => JSON.parse(gunzipSync(fs.readFileSync(file)));

function verifyInventory(directory) {
  const lines = fs.readFileSync(path.join(directory, 'SHA256SUMS'), 'utf8').split('\n').filter(Boolean);
  const entries = lines.map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match && !path.isAbsolute(match[2]) && !match[2].split('/').includes('..'), 'invalid inventory row');
    return {hash: match[1], name: match[2]};
  });
  assert.equal(new Set(entries.map(entry => entry.name)).size, entries.length, 'duplicate inventory path');
  assert.deepEqual(entries.map(entry => entry.name).sort(), [...EXPECTED_FILES].sort(), 'inventory files');
  for (const entry of entries) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, entry.name))), entry.hash,
      `inventory hash: ${entry.name}`);
  }
}

function result(analysis) {
  return {
    cases: analysis.cases.length,
    transferCount: BigInt(analysis.cases[0].totals.transferCount).toString(),
    hotEdgeCounts: analysis.cases.map(item => BigInt(item.hotEdge.count).toString()),
  };
}

export function verifyEvidenceContent(directory) {
  verifyInventory(directory);
  const reports = ['a64-flat-window.json.gz', 'a64-page-table.json.gz']
    .map(name => readReport(path.join(directory, name)));
  const fresh = analyzeEvidence(reports);
  const retained = JSON.parse(fs.readFileSync(path.join(directory, 'analysis.json'), 'utf8'));
  assert.deepEqual(retained, fresh, 'fresh analysis differs from retained analysis');
  return result(fresh);
}

export function verifyEvidence(directory) {
  for (const [name, expected] of Object.entries(FROZEN_SHA256)) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, name))), expected, `pinned evidence bytes: ${name}`);
  }
  return verifyEvidenceContent(directory);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  assert(process.argv.length === 2, 'usage: verify.mjs');
  const directory = path.join(path.dirname(fileURLToPath(import.meta.url)), 'evidence');
  const checked = verifyEvidence(directory);
  console.log(`region transfer evidence PASS cases=${checked.cases} transfers=${checked.transferCount} ` +
    `hotEdge=${checked.hotEdgeCounts.join(',')}`);
}
