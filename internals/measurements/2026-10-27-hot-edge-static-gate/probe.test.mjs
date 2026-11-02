import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {
  LIMITS, UNKNOWN_INTERNAL_COUNT, analyzeDiscoveredGraph, assertSelectionBudgets,
  deriveReturnBlocks, discoverGraph, fillSelection, parseOptimizedIr,
  reachableBlocks, selectRequiredClosure, shortestPath, validateReportIdentity,
} from './core.mjs';
import {runCli, validateBinarySha256} from './probe.mjs';

const pc = value => `0x${value.toString(16).padStart(16, '0')}`;
const count = value => `0x${BigInt(value).toString(16).padStart(16, '0')}`;
const ir = (location, terminal, instructions = 1, push = []) => [
  `Block: location={${location.toString(16).padStart(16, '0')}}`,
  ...Array.from({length: instructions}, (_, i) => `[${i}] SetX r0, #0x${i}`),
  ...push.map(target => `[p] PushRSB #0x${target.toString(16)}`),
  `terminal = ${terminal}`,
].join('\n');
const block = (descriptor, successors, instructions = 1) => ({
  pc: descriptor, instructions, successors, directSuccessors: successors,
  pushRsbContinuations: [], terminal: 'synthetic',
});
const directBranchTerminal = (thenTarget, elseTarget) =>
  `terminal = If{ls, LinkBlock{{${thenTarget.toString(16).padStart(16, '0')}}}, ` +
  `LinkBlock{{${elseTarget.toString(16).padStart(16, '0')}}}}`;
const graphFrom = specs => new Map(specs.map(([descriptor, successors, instructions = 1]) =>
  [descriptor, block(descriptor, successors, instructions)]));
const asDiscovery = blocks => ({blocks, rejected: {},
  instructions: [...blocks.values()].reduce((sum, item) => sum + item.instructions, 0)});
const analyzeFixture = (fixture, overrides = {}) => analyzeDiscoveredGraph({
  ...fixture, full: asDiscovery(fixture.full), baseline: asDiscovery(fixture.baseline), ...overrides,
});
const translatorFrom = blocks => descriptor => {
  assert(blocks.has(descriptor), `unexpected translation ${pc(descriptor)}`);
  return blocks.get(descriptor);
};

function fixture() {
  const entry = 0x1000, hotExit = 0x1010, hotTarget = 0x1030;
  const old = 0x12418, displaced = 0x1060, middle = 0x1040, returned = 0x1050;
  const full = graphFrom([
    [entry, [hotExit, old]], [hotExit, [old, hotTarget]], [old, [displaced]],
    [hotTarget, [middle]], [displaced, []], [middle, [returned]], [returned, []],
  ]);
  full.set(hotExit, {...full.get(hotExit), terminal: directBranchTerminal(old, hotTarget)});
  const baseline = new Map([...full].filter(([descriptor]) =>
    ![hotTarget, middle, returned].includes(descriptor)));
  const report = {
    git: {revision: 'source', upstream: 'upstream', dirty: false},
    architecture: 'a64', memoryMode: '0', workloadSha256: 'workload',
    workload: {artifacts: {a64: {sha256: 'a'.repeat(64)}}},
    regionDiagnostics: {regions: [{entryPc: pc(entry), blocks: [...baseline.keys()].map(descriptor => ({
      descriptor: pc(descriptor), returnSite: [entry, old].includes(descriptor),
    }))}]},
    regionTransferDiagnostics: {regions: [
      {callerEntryPc: pc(entry), exits: [
        {kind: 'external-direct-call', exitBlockDescriptor: pc(hotExit),
          staticTargetDescriptor: pc(hotTarget), count: count(7)},
      ]},
      {callerEntryPc: pc(hotTarget), exits: [
        {kind: 'rsb-return', exitBlockDescriptor: pc(returned), count: count(999)},
        {kind: 'rsb-return', exitBlockDescriptor: pc(displaced), count: count(0)},
      ]},
    ]},
  };
  return {entry, hotExit, hotTarget, old, displaced, middle, returned, full, baseline, report};
}

test('parser preserves terminal order then PushRSB continuation order', () => {
  const parsed = parseOptimizedIr(ir(0x12370,
    'If{ls, LinkBlock{{0000000000012418}}, LinkBlock{{000000000001237c}}}', 3,
    [0x1202c, 0x1209c]), 0x12370);
  assert.equal(parsed.instructions, 5);
  assert.deepEqual(parsed.directSuccessors, [0x12418, 0x1237c]);
  assert.deepEqual(parsed.pushRsbContinuations, [0x1202c, 0x1209c]);
  assert.deepEqual(parsed.successors, [0x12418, 0x1237c, 0x1202c, 0x1209c]);
});

test('parser matches selector order with cond_fail and LinkBlockFast', () => {
  const text = [
    'Block: location={0000000000001000}',
    'cycles=1, entry_cond=ne, cond_fail={0000000000001010}',
    'terminal = If{eq, LinkBlockFast{{0000000000001020}}, LinkBlock{{0000000000001030}}}',
    '[p] PushRSB #0x1040',
  ].join('\n');
  const parsed = parseOptimizedIr(text, 0x1000);
  assert.equal(parsed.conditionFailedSuccessor, 0x1010);
  assert.deepEqual(parsed.directSuccessors, [0x1020, 0x1030]);
  assert.deepEqual(parsed.successors, [0x1010, 0x1020, 0x1030, 0x1040]);
});

test('parser rejects malformed IR and requested-location mismatches', () => {
  assert.throws(() => parseOptimizedIr('terminal = Return{}', 0x1000), /location/);
  assert.throws(() => parseOptimizedIr(ir(0x1000, 'Return{}'), 0x1004), /location mismatch/);
  assert.throws(() => parseOptimizedIr(`Block: location={${'1000'.padStart(16, '0')}}`, 0x1000), /terminal/);
  assert.throws(() => parseOptimizedIr(ir(0x1000, 'Return{}') + '\nterminal = Return{}', 0x1000), /terminal/);
  assert.throws(() => parseOptimizedIr(ir(0x1000, 'Return{}') + '\nBlock: location={0000000000001000}', 0x1000), /location/);
});

test('discovery obeys caps, distance, IR budget, and finite safety bound', () => {
  const blocks = graphFrom([[0x1000, [0x1004, 0x3000]], [0x1004, [0x1008], 3],
    [0x1008, []], [0x3000, []]]);
  const baseline = discoverGraph(0x1000, translatorFrom(blocks),
    {blockLimit: 2, instructionLimit: 4, maxGuestDistance: 4096});
  assert.deepEqual([...baseline.blocks.keys()], [0x1000, 0x1004]);
  assert.equal(baseline.instructions, 4);
  assert.deepEqual(baseline.rejected, {distance: 1, blocks: 1});
  const uncapped = discoverGraph(0x1000, translatorFrom(blocks));
  assert.equal(uncapped.blocks.size, 3);
  assert.throws(() => discoverGraph(0x1000, translatorFrom(blocks), {safetyLimit: 1}), /safety bound/);
});

test('shortest paths and required closure include every path node', () => {
  const f = fixture();
  assert.deepEqual(shortestPath(f.full, f.entry, f.returned),
    [f.entry, f.hotExit, f.hotTarget, f.middle, f.returned]);
  assert.throws(() => shortestPath(f.full, f.displaced, f.returned), /no path/);
  const selected = selectRequiredClosure(f.full, f.entry, f.hotExit, f.hotTarget, [f.returned]);
  assert.deepEqual([...selected.required].sort((a, b) => a - b),
    [f.entry, f.hotExit, f.hotTarget, f.middle, f.returned]);
  assert.throws(() => selectRequiredClosure(f.full, f.entry, f.hotExit, f.hotTarget, [0xdead]), /no path/);
});

test('RSB returns derive only from target region and positive counts', () => {
  const f = fixture();
  f.report.regionTransferDiagnostics.regions[1].exits[1].count = count(3);
  f.report.regionTransferDiagnostics.regions.push({callerEntryPc: pc(f.entry), exits: [
    {kind: 'rsb-return', exitBlockDescriptor: pc(0xdead), count: count(100)}]});
  assert.deepEqual(deriveReturnBlocks(f.report, f.hotTarget), [
    {block: f.returned, count: 999}, {block: f.displaced, count: 3}]);
});

test('required budgets fail closed on blocks and IR', () => {
  const f = fixture();
  const {required} = selectRequiredClosure(f.full, f.entry, f.hotExit, f.hotTarget, [f.returned]);
  assert.throws(() => assertSelectionBudgets(f.full, required, {...LIMITS, maxBlocks: 4}), /block budget/);
  assert.throws(() => assertSelectionBudgets(f.full, required, {...LIMITS, maxInstructions: 4}), /IR budget/);
  assertSelectionBudgets(f.full, required, {...LIMITS, maxBlocks: 5, maxInstructions: 5});
});

test('fill is deterministic, capped, reachable, and keeps hot edge internal', () => {
  const f = fixture();
  const {required} = selectRequiredClosure(f.full, f.entry, f.hotExit, f.hotTarget, [f.returned]);
  const selected = fillSelection(f.full, required, 6, f.entry);
  assert.deepEqual([...selected], [f.hotExit, f.hotTarget, f.entry, f.middle, f.returned, f.old]);
  assert.deepEqual([...reachableBlocks(f.full, selected, f.entry)].sort((a, b) => a - b),
    [...selected].sort((a, b) => a - b));
  assert.equal(selected.has(f.hotExit) && selected.has(f.hotTarget), true);
  assert.throws(() => fillSelection(f.full, new Set([f.displaced]), 1, f.entry), /not entry-reachable/);
});

test('return sites come from retained baseline diagnostics, not PushRSB inference', () => {
  const f = fixture();
  const old = f.full.get(f.old);
  const pushRsbContinuations = [f.returned, f.returned, 0x2000];
  const full = new Map(f.full);
  full.set(f.old, {...old, pushRsbContinuations,
    successors: [...old.directSuccessors, ...pushRsbContinuations]});
  const result = analyzeFixture(f, {full: asDiscovery(full), limits: {...LIMITS, maxBlocks: 6}});
  assert.deepEqual(result.filled_selection.baseline_return_sites, [f.entry, f.old]);
  assert.deepEqual(result.filled_selection.return_sites_present, [f.entry, f.old]);
  assert.deepEqual(result.filled_selection.missing_return_sites, []);
});

test('selection fails closed when a retained baseline return site is displaced', () => {
  const f = fixture();
  assert.throws(() => analyzeFixture(f, {limits: {...LIMITS, maxBlocks: 5}}),
    /missing baseline return sites/);
});

test('optional FIFO fill skips IR-expensive blocks and admits later cheap blocks', () => {
  const blocks = graphFrom([[0x1000, [0x1010, 0x1020]], [0x1010, [], 10], [0x1020, []]]);
  const selected = fillSelection(blocks, new Set([0x1000]), 3, 0x1000, 2);
  assert.deepEqual([...selected], [0x1000, 0x1020]);
  assert.equal([...selected].reduce((sum, pc) => sum + blocks.get(pc).instructions, 0), 2);
});

test('hot target must be a direct successor, not only a selected PushRSB continuation', () => {
  const f = fixture();
  const hotExit = f.full.get(f.hotExit);
  const full = new Map(f.full);
  full.set(f.hotExit, {...hotExit, directSuccessors: [], pushRsbContinuations: [f.hotTarget]});
  assert.throws(() => analyzeFixture(f, {full: asDiscovery(full)}), /hot direct edge absent/);
});

test('direct hot successor does not hide a PushRSB call continuation', () => {
  const f = fixture();
  const hotExit = f.full.get(f.hotExit);
  const full = new Map(f.full);
  full.set(f.hotExit, {...hotExit, pushRsbContinuations: [f.returned],
    successors: [...hotExit.successors, f.returned]});
  assert.throws(() => analyzeFixture(f, {full: asDiscovery(full)}),
    /hot block must not have PushRSB continuations/);
});

test('hot block terminal must be the exact direct conditional branch', () => {
  const f = fixture();
  const hotExit = f.full.get(f.hotExit);
  const full = new Map(f.full);
  full.set(f.hotExit, {...hotExit, terminal: `terminal = LinkBlock{{${pc(f.hotTarget).slice(2)}}}`});
  assert.throws(() => analyzeFixture(f, {full: asDiscovery(full)}), /unexpected hot block terminal/);
});

test('hot block taken successor remains pinned to the reviewed direct branch', () => {
  const f = fixture();
  const hotExit = f.full.get(f.hotExit);
  const full = new Map(f.full);
  full.set(f.hotExit, {...hotExit, directSuccessors: [f.middle, f.hotTarget],
    successors: [f.middle, f.hotTarget], terminal: directBranchTerminal(f.middle, f.hotTarget)});
  assert.throws(() => analyzeFixture(f, {full: asDiscovery(full)}), /hot block taken successor/);
});

test('required newly-external baseline flag requires both endpoints in baseline', () => {
  const f = fixture();
  const hotTarget = f.full.get(f.hotTarget);
  const full = new Map(f.full);
  full.set(f.hotTarget, {...hotTarget, directSuccessors: [f.middle, f.old],
    successors: [f.middle, f.old]});
  const result = analyzeFixture(f, {full: asDiscovery(full), limits: {...LIMITS, maxBlocks: 6}});
  const edge = result.required.newly_external_edges_before_optional_fill.find(item =>
    item.block === f.hotTarget && item.target === f.old);
  assert.equal(edge.was_baseline_internal, false);
});

test('accounting separates measured internalization from unknown former-internal counts', () => {
  const f = fixture();
  const result = analyzeFixture(f, {limits: {...LIMITS, maxBlocks: 6}});
  assert.equal(result.required.blocks, 5);
  assert.equal(result.filled_selection.blocks, 6);
  assert.deepEqual(result.filled_selection.new_blocks_vs_baseline, [f.hotTarget, f.middle, f.returned]);
  assert.deepEqual(result.filled_selection.displaced_baseline_blocks, [f.displaced]);
  assert.deepEqual(result.filled_selection.internalized_measured_external_edges,
    [{block: f.hotExit, target: f.hotTarget, count: 7}]);
  assert.equal(result.filled_selection.internalized_measured_external_total, 7);
  assert.deepEqual(result.filled_selection.externalized_baseline_internal_edges,
    [{block: f.old, target: f.displaced, previous_dynamic_count: UNKNOWN_INTERNAL_COUNT}]);
  assert.equal(result.filled_selection.external_edges[0].previous_external_count, null);
  assert.equal(result.filled_selection.external_edges[0].was_baseline_internal, true);
  assert.equal(result.static_eligibility.is_architectural_call, false);
  assert.equal(result.static_eligibility.continuation_required, false);
  const text = JSON.stringify(result);
  assert.doesNotMatch(text, /inlin|call\/return/i);
  assert.equal(result.semantics,
    'direct-branch-only; 0x12370 is not BL; RSB counts are not attributed to this edge');
  assert.equal(result.filled_selection.internalized_measured_external_total, 7, 'RSB count leaked into hot edge');
});

test('baseline descriptor mismatch fails closed', () => {
  const f = fixture();
  const expected = new Set(f.baseline.keys());
  f.report.regionDiagnostics = {regions: [{entryPc: pc(f.entry),
    blocks: [...expected].map(descriptor => ({descriptor: pc(descriptor)}))}]};
  const wrongBaseline = new Map([...f.baseline].filter(([descriptor]) => descriptor !== f.old));
  assert.throws(() => analyzeFixture(f, {baseline: asDiscovery(wrongBaseline)}), /baseline descriptor mismatch/);
});

test('CLI rejects non-10000 load bases before reading inputs', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hot-edge-base-test-'));
  try {
    assert.throws(() => runCli(['missing-tool', 'missing-binary', '10001', 'missing-report',
      path.join(dir, 'new.json')]), /load base must be 10000/);
    assert.throws(() => runCli(['missing-tool', 'missing-binary', '0x10001', 'missing-report',
      path.join(dir, 'new.json')]), /load base must be 10000/);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});

test('report and binary identity validation fail closed', () => {
  const f = fixture();
  validateReportIdentity(f.report, {
    source: 'source', upstream: 'upstream', workloadSha256: 'workload', binarySha256: 'a'.repeat(64),
  });
  assert.throws(() => validateReportIdentity({...f.report, git: {...f.report.git, dirty: true}}, {
    source: 'source', upstream: 'upstream', workloadSha256: 'workload', binarySha256: 'a'.repeat(64),
  }), /dirty/);
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hot-edge-test-'));
  try {
    const binary = path.join(dir, 'a64.bin');
    fs.writeFileSync(binary, 'not the workload');
    assert.throws(() => validateBinarySha256(binary, '0'.repeat(64)), /SHA-256 mismatch/);
    const output = path.join(dir, 'exists.json');
    fs.writeFileSync(output, '{}');
    assert.throws(() => runCli(['native', binary, '10000', 'report.json', output]), /refusing to overwrite/);
  } finally {
    fs.rmSync(dir, {recursive: true, force: true});
  }
});
