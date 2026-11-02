#!/usr/bin/env node
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const evidence = path.join(path.dirname(fileURLToPath(import.meta.url)), 'evidence');
const FILES = ['analysis.json', 'baseline.wasm', 'candidate.wasm', 'commands.md',
  'emission-original.json', 'emit_probe_original.cpp', 'v8-compile-original.txt'];
const FROZEN_SHA256 = Object.freeze({
  'analysis.json': 'abdb7d273fe0cba7cd70943ee79a3c73c503588191e1f541aec073e30c4eae04',
  'baseline.wasm': '3e5a89aa815d658b85988cebbea49475f7ec4bde907c916c98098f877b29a306',
  'candidate.wasm': '9effb79067b9a658cc6c94a872f8cb093fa134e94d7b4c8baab4e492b7405102',
  'commands.md': 'aa33639a53ba75e66568b194bd5d2151f5798ffa9364694c19684f272ac1b6fd',
  'emission-original.json': 'f15791bfcf1b497ece9b2023c60e22eb42ac3b0528f964ac60eacbbc06b9d511',
  'emit_probe_original.cpp': '23628ab9988006a17e9d4f2c6d2fdaa2d750bcb441d2d2720a7e436a1e716ff3',
  'v8-compile-original.txt': '8bc7795348a84814b9d92153b7bf00533804466b72f2d39655b6c14b66ef4785',
});
const RETURN_SITES = [0x1202c, 0x1209c, 0x120f4, 0x12104, 0x12114, 0x12124, 0x12134];
const UNKNOWN = 'not-measured-internal-edge';
const sha256 = data => createHash('sha256').update(data).digest('hex');
const readJson = (directory, name) => JSON.parse(fs.readFileSync(path.join(directory, name), 'utf8'));

function verifyInventory(directory) {
  const inventory = fs.readFileSync(path.join(directory, 'SHA256SUMS'), 'utf8');
  const entries = inventory.trim().split('\n').map(line => {
    const match = /^([0-9a-f]{64})  (.+)$/.exec(line);
    assert(match, `invalid inventory line: ${line}`);
    return {hash: match[1], name: match[2]};
  });
  assert.deepEqual(entries.map(entry => entry.name).sort(), [...FILES].sort(), 'inventory files');
  for (const entry of entries) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, entry.name))), entry.hash,
      `inventory hash: ${entry.name}`);
  }
}

function verifyFrozenHashes(directory) {
  for (const [name, expected] of Object.entries(FROZEN_SHA256)) {
    assert.equal(sha256(fs.readFileSync(path.join(directory, name))), expected,
      `pinned evidence bytes: ${name}`);
  }
}

function verifyAnalysis(analysis) {
  assert.equal(analysis.source, 'ec4701f20d2e53a133367e4048f3f0fc2b981c82', 'source identity');
  assert.equal(analysis.upstream, 'a41c380246d3d9f9874f0f792d234dc0cc17c180', 'upstream identity');
  assert.equal(analysis.git.dirty, false, 'source cleanliness');
  assert.equal(analysis.architecture, 'a64', 'architecture');
  assert.equal(analysis.memoryMode, '0', 'memory mode');
  assert.equal(analysis.workloadSha256, 'ad43250ce2139ab0d0fdbf1eac0bb1284e00fde62419d387accfe6348cca17aa', 'workload hash');
  const binaryHash = '8f96e0f0f3beb9a5fb8765405926655c182bfcdfe93a8f99dc6ea32dec227ab1';
  assert.equal(analysis.a64BinarySha256, binaryHash, 'A64 binary hash');
  assert.equal(analysis.reportA64BinarySha256, binaryHash, 'reported A64 binary hash');
  assert.equal(analysis.blockDumpSha256, 'd59bc78bbf7058ef103428ac3f8b1e5d12dd95944d4e4218e89afa18b010c5e7', 'tool hash');
  assert.equal(analysis.loadBase, 65536, 'load base');
  assert.deepEqual(analysis.limits, {max_blocks: 64, max_instructions: 2000,
    max_guest_distance: 4096, full_graph_safety_blocks: 1024}, 'selection limits');
  assert.equal(analysis.full_reachable_within_distance.blocks, 88, 'full graph blocks');
  assert.equal(analysis.full_reachable_within_distance.instructions, 701, 'full graph IR');
  assert.equal(analysis.baseline.blocks, 64, 'baseline blocks');
  assert.equal(analysis.baseline.instructions, 511, 'baseline IR');
  assert.equal(analysis.required.blocks, 17, 'required blocks');
  assert.equal(analysis.required.instructions, 127, 'required IR');
  assert.equal(analysis.filled_selection.blocks, 64, 'filled selection blocks');
  assert.equal(analysis.filled_selection.instructions, 524, 'filled selection IR');
  const hotBlocks = analysis.full_reachable_within_distance.block_info.filter(block => block.descriptor === 0x12370);
  assert.equal(hotBlocks.length, 1, 'hot exit block_info');
  assert(hotBlocks[0].direct_successors.includes(0x1237c), 'actual direct hot edge missing');
  assert.deepEqual(hotBlocks[0].direct_successors, [0x12418, 0x1237c], 'hot block direct successors');
  assert.deepEqual(hotBlocks[0].push_rsb_continuations, [], 'hot block PushRSB continuations');
  assert.equal(hotBlocks[0].terminal,
    'terminal = If{ls, LinkBlock{{0000000000012418}}, LinkBlock{{000000000001237c}}}',
    'hot block terminal');
  assert.equal(analysis.static_eligibility.is_architectural_call, false, 'hot block architectural call');
  assert.equal(analysis.static_eligibility.continuation_required, false, 'hot block continuation requirement');
  assert.equal(analysis.filled_selection.hot_edge_internal, true, 'hot edge not internal');
  assert.deepEqual(analysis.filled_selection.new_blocks_vs_baseline, [0x1237c, 0x12418], 'added blocks');
  assert.deepEqual(analysis.filled_selection.displaced_baseline_blocks, [0x12350, 0x12398], 'displaced blocks');
  assert.deepEqual(analysis.filled_selection.baseline_return_sites, RETURN_SITES, 'baseline return sites');
  assert.deepEqual(analysis.filled_selection.return_sites_present, RETURN_SITES, 'return sites present');
  assert.deepEqual(analysis.filled_selection.missing_return_sites, [], 'missing return sites');
  assert.deepEqual(analysis.filled_selection.internalized_measured_external_edges, [
    {block: 0x12370, target: 0x1237c, count: 47199999},
    {block: 0x12370, target: 0x12418, count: 2399999},
  ], 'internalized measured edges');
  assert.equal(analysis.filled_selection.internalized_measured_external_total, 49599998, 'internalized total');
  assert.deepEqual(analysis.filled_selection.externalized_baseline_internal_edges, [
    {block: 0x122f8, target: 0x12398, previous_dynamic_count: UNKNOWN},
    {block: 0x1233c, target: 0x12398, previous_dynamic_count: UNKNOWN},
    {block: 0x1233c, target: 0x12350, previous_dynamic_count: UNKNOWN},
  ], 'externalized baseline-internal edges');
}

function verifyEmission(directory, emission) {
  assert.equal(emission.baseline.blocks, 64, 'emission baseline blocks');
  assert.equal(emission.candidate.blocks, 64, 'emission candidate blocks');
  assert.equal(emission.baseline.layout, 'fallback', 'emission baseline layout');
  assert.equal(emission.candidate.layout, 'fallback', 'emission candidate layout');
  assert.equal(emission.baseline.module_bytes, 17551, 'emission baseline module bytes');
  assert.equal(emission.candidate.module_bytes, 19408, 'emission candidate module bytes');
  assert.equal(emission.baseline.entry_body_bytes, 17342, 'emission baseline entry body');
  assert.equal(emission.candidate.entry_body_bytes, 19199, 'emission candidate entry body');
  assert.equal(fs.statSync(path.join(directory, 'baseline.wasm')).size, 17551, 'baseline Wasm size');
  assert.equal(fs.statSync(path.join(directory, 'candidate.wasm')).size, 19408, 'candidate Wasm size');
}

function verifyContent(directory) {
  const analysis = readJson(directory, 'analysis.json');
  verifyAnalysis(analysis);
  verifyEmission(directory, readJson(directory, 'emission-original.json'));
  return {scope: 'static-evidence-integrity-only', files: FILES.length,
    fullBlocks: analysis.full_reachable_within_distance.blocks,
    finalBlocks: analysis.filled_selection.blocks,
    finalInstructions: analysis.filled_selection.instructions,
    internalizedTotal: analysis.filled_selection.internalized_measured_external_total,
    returnSites: analysis.filled_selection.return_sites_present.length,
    baselineModuleBytes: 17551, candidateModuleBytes: 19408};
}

export function verifyEvidenceContent(directory) {
  verifyInventory(directory);
  return verifyContent(directory);
}

export function verifyEvidence(directory) {
  verifyInventory(directory);
  verifyFrozenHashes(directory);
  return verifyContent(directory);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  assert.equal(process.argv.length, 2, 'usage: verify.mjs');
  const result = verifyEvidence(evidence);
  console.log(`hot-edge static evidence integrity PASS files=${result.files} full=${result.fullBlocks}/701 ` +
    `final=${result.finalBlocks}/${result.finalInstructions} internalized=${result.internalizedTotal} ` +
    `returnSites=${result.returnSites}; runtime/performance not assessed`);
}
