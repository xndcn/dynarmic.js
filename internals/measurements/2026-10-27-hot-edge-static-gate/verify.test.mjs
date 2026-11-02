import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {verifyEvidence, verifyEvidenceContent} from './verify.mjs';

const evidence = path.join(path.dirname(fileURLToPath(import.meta.url)), 'evidence');
const FILES = ['analysis.json', 'baseline.wasm', 'candidate.wasm', 'commands.md',
  'emission-original.json', 'emit_probe_original.cpp', 'v8-compile-original.txt'];
const sha256 = data => createHash('sha256').update(data).digest('hex');

function copyEvidence(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'hot-edge-verify-test-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  fs.cpSync(evidence, root, {recursive: true});
  return root;
}

function refreshInventory(directory) {
  const inventory = FILES.map(name =>
    `${sha256(fs.readFileSync(path.join(directory, name)))}  ${name}`).join('\n');
  fs.writeFileSync(path.join(directory, 'SHA256SUMS'), `${inventory}\n`);
}

function rewriteJson(directory, name, update) {
  const file = path.join(directory, name);
  const value = JSON.parse(fs.readFileSync(file, 'utf8'));
  update(value);
  fs.writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
  refreshInventory(directory);
}

test('retained evidence passes frozen integrity and semantic verification', () => {
  assert.deepEqual(verifyEvidence(evidence), {
    scope: 'static-evidence-integrity-only', files: 7, fullBlocks: 88,
    finalBlocks: 64, finalInstructions: 524, internalizedTotal: 49599998,
    returnSites: 7, baselineModuleBytes: 17551, candidateModuleBytes: 19408,
  });
});

test('semantic tampering is rejected even with a refreshed inventory', t => {
  const root = copyEvidence(t);
  rewriteJson(root, 'analysis.json', analysis => {
    analysis.filled_selection.blocks = 65;
  });
  assert.throws(() => verifyEvidenceContent(root), /filled selection/);
});

test('hot block continuations and exact direct-branch terminal are independently verified', t => {
  const mixed = copyEvidence(t);
  rewriteJson(mixed, 'analysis.json', analysis => {
    const hot = analysis.full_reachable_within_distance.block_info.find(block => block.descriptor === 0x12370);
    hot.push_rsb_continuations = [0x1202c];
  });
  assert.throws(() => verifyEvidenceContent(mixed), /hot block PushRSB continuations/);

  const wrongTerminal = copyEvidence(t);
  rewriteJson(wrongTerminal, 'analysis.json', analysis => {
    const hot = analysis.full_reachable_within_distance.block_info.find(block => block.descriptor === 0x12370);
    hot.terminal = 'terminal = Return{}';
  });
  assert.throws(() => verifyEvidenceContent(wrongTerminal), /hot block terminal/);
});

test('unknown former-internal dynamic counts cannot be changed to zero', t => {
  const root = copyEvidence(t);
  rewriteJson(root, 'analysis.json', analysis => {
    analysis.filled_selection.externalized_baseline_internal_edges[0].previous_dynamic_count = 0;
  });
  assert.throws(() => verifyEvidenceContent(root), /externalized baseline-internal edges/);
});

test('dropping a retained return site is rejected', t => {
  const root = copyEvidence(t);
  rewriteJson(root, 'analysis.json', analysis => {
    analysis.filled_selection.return_sites_present.pop();
  });
  assert.throws(() => verifyEvidenceContent(root), /return sites/);
});

test('missing inventory entries are rejected', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'SHA256SUMS');
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  fs.writeFileSync(file, `${lines.slice(1).join('\n')}\n`);
  assert.throws(() => verifyEvidence(root), /inventory files/);
});

test('duplicate inventory entries are rejected', t => {
  const root = copyEvidence(t);
  const file = path.join(root, 'SHA256SUMS');
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  fs.writeFileSync(file, `${[...lines, lines[0]].join('\n')}\n`);
  assert.throws(() => verifyEvidence(root), /inventory files/);
});

test('coordinated changes invisible to semantics are caught by frozen hashes', t => {
  const root = copyEvidence(t);
  rewriteJson(root, 'emission-original.json', emission => {
    emission.baseline.native_emit_ms_median_of_20 = 999;
  });
  assert.equal(verifyEvidenceContent(root).scope, 'static-evidence-integrity-only');
  assert.throws(() => verifyEvidence(root), /pinned evidence bytes: emission-original.json/);
});

test('modified Wasm bytes are independently rejected despite refreshed inventory', t => {
  const root = copyEvidence(t);
  for (const name of ['baseline.wasm', 'candidate.wasm']) {
    const file = path.join(root, name);
    const bytes = fs.readFileSync(file);
    bytes[bytes.length - 1] ^= 1;
    fs.writeFileSync(file, bytes);
    refreshInventory(root);
    assert.throws(() => verifyEvidence(root), new RegExp(`pinned evidence bytes: ${name}`));
    fs.copyFileSync(path.join(evidence, name), file);
    refreshInventory(root);
  }
});
