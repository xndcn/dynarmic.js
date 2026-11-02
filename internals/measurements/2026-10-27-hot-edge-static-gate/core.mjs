import assert from 'node:assert/strict';

export const ENTRY_PC = 0x1202c;
export const HOT_EXIT = 0x12370;
export const HOT_TARGET = 0x1237c;
export const LIMITS = Object.freeze({
  maxBlocks: 64, maxInstructions: 2000, maxGuestDistance: 4096,
});
export const FULL_GRAPH_SAFETY_LIMIT = 1024;
export const UNKNOWN_INTERNAL_COUNT = 'not-measured-internal-edge';

const descriptor = value => Number.parseInt(value, 16);
const safeCount = value => {
  const parsed = BigInt(value);
  assert(parsed >= 0n && parsed <= BigInt(Number.MAX_SAFE_INTEGER), 'dynamic count is not a safe nonnegative integer');
  return Number(parsed);
};
const region = (report, key, value) => {
  const matches = report.regionTransferDiagnostics.regions.filter(item => item[key] === value);
  assert.equal(matches.length, 1, `missing or ambiguous transfer region ${value}`);
  return matches[0];
};
const sorted = values => [...values].sort((a, b) => a - b);
const totalInstructions = (blocks, selected) =>
  [...selected].reduce((sum, descriptor) => sum + blocks.get(descriptor).instructions, 0);

export function parseOptimizedIr(text, requestedPc) {
  assert.equal(typeof text, 'string', 'optimized IR must be text');
  const lines = text.split(/\r?\n/);
  const locations = lines.filter(line => line.startsWith('Block: location='));
  assert.equal(locations.length, 1, 'optimized IR must contain exactly one block location');
  const match = locations[0].match(/^Block: location=\{([0-9a-f]{16})\}$/);
  assert(match, 'malformed optimized IR block location');
  const location = descriptor(match[1]);
  assert.equal(location, requestedPc, `IR block location mismatch: requested ${requestedPc.toString(16)}, got ${location.toString(16)}`);
  const terminals = lines.filter(line => line.startsWith('terminal = '));
  assert.equal(terminals.length, 1, 'optimized IR must contain exactly one terminal');
  const conditionFailures = [...text.matchAll(/\bcond_fail=\{([0-9a-f]{16})\}/g)];
  assert(conditionFailures.length <= 1, 'optimized IR must contain at most one cond_fail location');
  const conditionFailedSuccessor = conditionFailures.length ? descriptor(conditionFailures[0][1]) : null;
  const directSuccessors = [...terminals[0].matchAll(/LinkBlock(?:Fast)?\{\{([0-9a-f]{16})\}\}/g)]
    .map(item => descriptor(item[1]));
  const pushRsbContinuations = lines.flatMap(line =>
    [...line.matchAll(/\bPushRSB #0x([0-9a-f]+)/g)].map(item => descriptor(item[1])));
  return {
    pc: requestedPc,
    instructions: lines.filter(line => line.startsWith('[')).length,
    conditionFailedSuccessor,
    directSuccessors,
    pushRsbContinuations,
    successors: [
      ...(conditionFailedSuccessor === null ? [] : [conditionFailedSuccessor]),
      ...directSuccessors, ...pushRsbContinuations,
    ],
    terminal: terminals[0],
  };
}

export function discoverGraph(entry, translate, options = {}) {
  const {
    blockLimit = null, instructionLimit = null,
    maxGuestDistance = LIMITS.maxGuestDistance,
    safetyLimit = FULL_GRAPH_SAFETY_LIMIT,
  } = options;
  const first = translate(entry);
  const blocks = new Map([[entry, first]]);
  let instructions = first.instructions;
  const work = [...first.successors];
  const rejected = {};
  while (work.length) {
    const pc = work.shift();
    if (blocks.has(pc)) continue;
    if (Math.abs(pc - entry) > maxGuestDistance) {
      rejected.distance = (rejected.distance || 0) + 1;
      continue;
    }
    if (blockLimit !== null && blocks.size >= blockLimit) {
      rejected.blocks = (rejected.blocks || 0) + 1;
      continue;
    }
    assert(blocks.size < safetyLimit, `full graph safety bound of ${safetyLimit} blocks exceeded`);
    const current = translate(pc);
    if (instructionLimit !== null && instructions + current.instructions > instructionLimit) {
      rejected.instructions = (rejected.instructions || 0) + 1;
      continue;
    }
    blocks.set(pc, current);
    instructions += current.instructions;
    work.push(...current.successors);
  }
  return {blocks, instructions, rejected};
}

export function shortestPath(blocks, start, goal) {
  assert(blocks.has(start) && blocks.has(goal), `no path ${start.toString(16)} -> ${goal.toString(16)}`);
  const predecessor = new Map([[start, null]]);
  const work = [start];
  for (let index = 0; index < work.length; index++) {
    const pc = work[index];
    if (pc === goal) {
      const path = [];
      for (let at = goal; at !== null; at = predecessor.get(at)) path.push(at);
      return path.reverse();
    }
    for (const next of blocks.get(pc).successors) {
      if (blocks.has(next) && !predecessor.has(next)) {
        predecessor.set(next, pc);
        work.push(next);
      }
    }
  }
  assert.fail(`no path ${start.toString(16)} -> ${goal.toString(16)}`);
}

export function deriveReturnBlocks(report, hotTarget = HOT_TARGET) {
  const target = region(report, 'callerEntryPc', `0x${hotTarget.toString(16).padStart(16, '0')}`);
  return target.exits.filter(exit => exit.kind === 'rsb-return' && BigInt(exit.count) > 0n)
    .map(exit => ({block: descriptor(exit.exitBlockDescriptor), count: safeCount(exit.count)}))
    .sort((a, b) => a.block - b.block);
}

export function selectRequiredClosure(blocks, entry, hotExit, hotTarget, returnBlocks) {
  const paths = {entry_to_hot_exit: shortestPath(blocks, entry, hotExit)};
  for (const returnBlock of returnBlocks) {
    paths[`target_to_${returnBlock.toString(16)}`] = shortestPath(blocks, hotTarget, returnBlock);
  }
  const required = new Set([hotExit, hotTarget]);
  for (const path of Object.values(paths)) for (const pc of path) required.add(pc);
  return {paths, required};
}

export function assertSelectionBudgets(blocks, required, limits = LIMITS) {
  const instructions = totalInstructions(blocks, required);
  assert(required.size <= limits.maxBlocks, `required closure exceeds block budget: ${required.size} > ${limits.maxBlocks}`);
  assert(instructions <= limits.maxInstructions, `required closure exceeds IR budget: ${instructions} > ${limits.maxInstructions}`);
}

export function reachableBlocks(blocks, selected, entry) {
  if (!selected.has(entry)) return new Set();
  const reachable = new Set([entry]);
  const work = [entry];
  for (let index = 0; index < work.length; index++) {
    for (const next of blocks.get(work[index]).successors) {
      if (selected.has(next) && !reachable.has(next)) {
        reachable.add(next);
        work.push(next);
      }
    }
  }
  return reachable;
}

export function fillSelection(blocks, required, maxBlocks = LIMITS.maxBlocks, entry = ENTRY_PC,
                              maxInstructions = LIMITS.maxInstructions) {
  assert(required.size <= maxBlocks, 'required closure exceeds block budget');
  let instructions = totalInstructions(blocks, required);
  assert(instructions <= maxInstructions, 'required closure exceeds IR budget');
  const selected = new Set(required);
  for (const [pc, block] of blocks) {
    if (selected.has(pc)) continue;
    if (selected.size >= maxBlocks) break;
    if (instructions + block.instructions > maxInstructions) continue;
    selected.add(pc);
    instructions += block.instructions;
  }
  const reachable = reachableBlocks(blocks, selected, entry);
  assert.equal(reachable.size, selected.size, 'selected blocks are not entry-reachable');
  return selected;
}

function directCounts(report, entry) {
  const caller = region(report, 'callerEntryPc', `0x${entry.toString(16).padStart(16, '0')}`);
  const counts = new Map();
  for (const exit of caller.exits) {
    if (exit.kind !== 'external-direct-call') continue;
    const key = `${descriptor(exit.exitBlockDescriptor)}:${descriptor(exit.staticTargetDescriptor)}`;
    assert(!counts.has(key), `duplicate measured direct edge ${key}`);
    counts.set(key, safeCount(exit.count));
  }
  return counts;
}

function assertBaselineMatchesReport(report, baseline, entry) {
  const regions = report.regionDiagnostics.regions.filter(item =>
    item.entryPc === `0x${entry.toString(16).padStart(16, '0')}`);
  assert.equal(regions.length, 1, 'missing or ambiguous retained baseline region');
  const expected = new Set(regions[0].blocks.map(block => descriptor(block.descriptor)));
  const actual = new Set(baseline.blocks.keys());
  const difference = sorted([...new Set([...actual, ...expected])].filter(pc => actual.has(pc) !== expected.has(pc)));
  assert.equal(difference.length, 0, `baseline descriptor mismatch: ${difference.map(pc => pc.toString(16)).join(',')}`);
  return regions[0];
}

export function analyzeDiscoveredGraph({
  entry = ENTRY_PC, hotExit = HOT_EXIT, hotTarget = HOT_TARGET,
  full, baseline, report, limits = LIMITS, moduleEmissionResults,
}) {
  const retainedRegion = assertBaselineMatchesReport(report, baseline, entry);
  const hotBlock = full.blocks.get(hotExit);
  const hotDirectEdge = hotBlock?.directSuccessors.includes(hotTarget) ?? false;
  assert(hotDirectEdge, 'hot direct edge absent from hotExit direct successors');
  const hotContinuations = hotBlock.pushRsbContinuations;
  assert.equal(hotContinuations.length, 0, 'hot block must not have PushRSB continuations');
  assert.equal(hotBlock.directSuccessors.length, 2, 'hot block must have exactly two direct successors');
  const [thenTarget, elseTarget] = hotBlock.directSuccessors;
  assert.equal(thenTarget, 0x12418, 'hot block taken successor must be 0x12418');
  assert.equal(elseTarget, hotTarget, 'hot target must be the direct conditional else successor');
  const descriptorText = value => value.toString(16).padStart(16, '0');
  const expectedHotTerminal = `terminal = If{ls, LinkBlock{{${descriptorText(thenTarget)}}}, ` +
    `LinkBlock{{${descriptorText(elseTarget)}}}}`;
  assert.equal(hotBlock.terminal, expectedHotTerminal, 'unexpected hot block terminal');
  const continuationRequired = hotContinuations.length > 0;
  const isArchitecturalCall = continuationRequired;
  const returns = deriveReturnBlocks(report, hotTarget);
  const {paths, required} = selectRequiredClosure(full.blocks, entry, hotExit, hotTarget,
    returns.map(item => item.block));
  assertSelectionBudgets(full.blocks, required, limits);
  const selected = fillSelection(full.blocks, required, limits.maxBlocks, entry, limits.maxInstructions);
  const hotEdgeInternal = hotDirectEdge && selected.has(hotExit) && selected.has(hotTarget);
  assert(hotEdgeInternal, 'hot edge is not internal');
  const baselineSet = new Set(baseline.blocks.keys());
  const counts = directCounts(report, entry);
  const newlyExternal = [];
  for (const pc of sorted(required)) for (const target of full.blocks.get(pc).successors) {
    if (!required.has(target)) newlyExternal.push({block: pc, target,
      was_baseline_internal: baselineSet.has(pc) && baselineSet.has(target)});
  }
  const internalized = [...counts].map(([key, count]) => {
    const [block, target] = key.split(':').map(Number);
    return {block, target, count};
  }).filter(edge => selected.has(edge.block) && selected.has(edge.target) && !baselineSet.has(edge.target))
    .sort((a, b) => a.block - b.block || a.target - b.target);
  const externalized = [];
  for (const pc of sorted([...selected].filter(pc => baselineSet.has(pc)))) {
    for (const target of full.blocks.get(pc).successors) {
      if (baselineSet.has(target) && !selected.has(target)) {
        externalized.push({block: pc, target, previous_dynamic_count: UNKNOWN_INTERNAL_COUNT});
      }
    }
  }
  const external = [];
  for (const pc of sorted(selected)) for (const target of full.blocks.get(pc).successors) {
    if (!selected.has(target)) external.push({
      block: pc, target, previous_external_count: counts.get(`${pc}:${target}`) ?? null,
      was_baseline_internal: baselineSet.has(pc) && baselineSet.has(target),
    });
  }
  const baselineReturnSites = sorted(new Set(retainedRegion.blocks.filter(block => block.returnSite === true)
    .map(block => descriptor(block.descriptor))));
  const returnSites = baselineReturnSites.filter(pc => selected.has(pc));
  const missingReturnSites = baselineReturnSites.filter(pc => !selected.has(pc));
  assert.equal(missingReturnSites.length, 0,
    `missing baseline return sites: ${missingReturnSites.map(pc => pc.toString(16)).join(',')}`);
  const result = {
    schema: 'offline-hot-edge-static-probe-v1',
    semantics: 'direct-branch-only; 0x12370 is not BL; RSB counts are not attributed to this edge',
    limits: {max_blocks: limits.maxBlocks, max_instructions: limits.maxInstructions,
      max_guest_distance: limits.maxGuestDistance, full_graph_safety_blocks: FULL_GRAPH_SAFETY_LIMIT},
    baseline: {blocks: baseline.blocks.size, instructions: baseline.instructions,
      descriptors: sorted(baselineSet), rejected: baseline.rejected},
    full_reachable_within_distance: {blocks: full.blocks.size, instructions: full.instructions,
      rejected: full.rejected, descriptors: sorted(full.blocks.keys()),
      block_info: sorted(full.blocks.keys()).map(pc => {
        const block = full.blocks.get(pc);
        return {descriptor: pc, instructions: block.instructions, successors: block.successors,
          condition_failed_successor: block.conditionFailedSuccessor ?? null,
          direct_successors: block.directSuccessors, push_rsb_continuations: block.pushRsbContinuations,
          terminal: block.terminal};
      })},
    target_region_rsb_exits: returns,
    paths,
    required: {blocks: required.size, instructions: totalInstructions(full.blocks, required),
      descriptors: sorted(required), baseline_blocks_displaced: sorted([...baselineSet].filter(pc => !required.has(pc))),
      new_blocks_vs_baseline: sorted([...required].filter(pc => !baselineSet.has(pc))),
      newly_external_edges_before_optional_fill: newlyExternal},
    filled_selection: {blocks: selected.size, instructions: totalInstructions(full.blocks, selected),
      descriptors: sorted(selected), new_blocks_vs_baseline: sorted([...selected].filter(pc => !baselineSet.has(pc))),
      displaced_baseline_blocks: sorted([...baselineSet].filter(pc => !selected.has(pc))),
      hot_edge_internal: hotEdgeInternal, all_return_blocks_present: returns.every(item => selected.has(item.block)),
      baseline_return_sites: baselineReturnSites, return_sites_present: returnSites,
      missing_return_sites: missingReturnSites,
      internalized_measured_external_edges: internalized,
      internalized_measured_external_total: internalized.reduce((sum, edge) => sum + edge.count, 0),
      externalized_baseline_internal_edges: externalized, external_edges: external},
    static_eligibility: {entry_reachable: full.blocks.has(hotExit), is_architectural_call: isArchitecturalCall,
      continuation_required: continuationRequired, fits_block_cap: required.size <= limits.maxBlocks,
      fits_ir_cap: totalInstructions(full.blocks, required) <= limits.maxInstructions,
      all_baseline_return_sites_present: missingReturnSites.length === 0},
  };
  if (moduleEmissionResults !== undefined) result.module_emission_results = moduleEmissionResults;
  return result;
}

export function validateReportIdentity(report, expected) {
  assert.equal(report.git.revision, expected.source, 'source revision mismatch');
  assert.equal(report.git.upstream, expected.upstream, 'upstream revision mismatch');
  assert.equal(report.git.dirty, false, 'report git tree must not be dirty');
  assert.equal(report.architecture, 'a64', 'architecture mismatch');
  assert.equal(report.memoryMode, '0', 'memory mode mismatch');
  assert.equal(report.workloadSha256, expected.workloadSha256, 'workload SHA-256 mismatch');
  const a64Sha256 = report.workload?.artifacts?.a64?.sha256;
  if (a64Sha256 !== undefined) assert.equal(a64Sha256, expected.binarySha256, 'report a64 binary SHA-256 mismatch');
}
