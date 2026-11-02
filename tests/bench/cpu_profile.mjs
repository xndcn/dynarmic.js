// Diagnostic V8 CPU profile analysis. Sample shares are observations, not
// compile-stage timings or uninstrumented benchmark measurements.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import {pathToFileURL} from 'node:url';

export function summarizeCpuProfile(profile, rootPattern = null) {
  assert(Array.isArray(profile.nodes) && profile.nodes.length > 0, 'missing profile nodes');
  assert(Array.isArray(profile.samples) && profile.samples.length > 0, 'missing CPU samples');
  assert(Array.isArray(profile.timeDeltas) && profile.timeDeltas.length === profile.samples.length,
    'sample/timeDelta length mismatch');
  assert(Number.isFinite(profile.startTime) && Number.isFinite(profile.endTime)
    && profile.endTime >= profile.startTime, 'invalid profile time range');
  const nodes = new Map(), parents = new Map(), frames = new Map(), keys = new Map();
  for (const node of profile.nodes) {
    assert(Number.isSafeInteger(node.id) && node.id > 0 && !nodes.has(node.id), 'invalid/duplicate node ID');
    const f = node.callFrame;
    assert(f && typeof f.functionName === 'string' && typeof f.scriptId === 'string'
      && typeof f.url === 'string' && Number.isInteger(f.lineNumber) && Number.isInteger(f.columnNumber),
    'invalid call frame');
    assert(node.children === undefined || Array.isArray(node.children), 'invalid children');
    const frame = {functionName: f.functionName, scriptId: f.scriptId, url: f.url,
      lineNumber: f.lineNumber, columnNumber: f.columnNumber};
    const key = JSON.stringify(frame);
    keys.set(node.id, key);
    if (!frames.has(key)) frames.set(key, {...frame, selfSamples: 0, inclusiveSamples: 0});
    nodes.set(node.id, node);
  }
  for (const node of nodes.values()) for (const child of node.children || []) {
    assert(nodes.has(child), 'unknown child ID');
    assert(!parents.has(child), 'node has multiple parents');
    parents.set(child, node.id);
  }
  const roots = [...nodes.keys()].filter(id => !parents.has(id));
  assert.equal(roots.length, 1, 'profile must have one tree root');
  const visited = new Set(), pending = [roots[0]];
  while (pending.length) {
    const id = pending.pop();
    assert(!visited.has(id), 'cyclic profile tree');
    visited.add(id);
    for (const child of nodes.get(id).children || []) pending.push(child);
  }
  assert.equal(visited.size, nodes.size, 'disconnected/cyclic profile tree');

  const rootRegex = rootPattern === null ? null : new RegExp(rootPattern);
  const selectedRoots = new Set([...nodes.values()]
    .filter(n => rootRegex === null ? n.id === roots[0] : rootRegex.test(n.callFrame.functionName))
    .map(n => n.id));
  assert(selectedRoots.size > 0, 'no call frames match root pattern');
  let selectedSamples = 0;
  for (const id of profile.samples) {
    assert(nodes.has(id), 'sample references unknown node');
    const stack = [];
    for (let current = id; current !== undefined; current = parents.get(current)) stack.push(current);
    // The outermost matching ancestor defines the scope. Overlapping matching
    // roots and recursion must not multiply sample counts.
    const rootIndex = stack.findLastIndex(current => selectedRoots.has(current));
    if (rootIndex < 0) continue;
    selectedSamples++;
    frames.get(keys.get(id)).selfSamples++;
    const seenFrames = new Set(stack.slice(0, rootIndex + 1).map(current => keys.get(current)));
    for (const key of seenFrames) frames.get(key).inclusiveSamples++;
  }
  assert(selectedSamples > 0, 'matched subtree has no samples');
  const deltas = profile.timeDeltas;
  assert(deltas.every(d => Number.isFinite(d) && d >= 0), 'invalid/negative sampling delta');
  const sorted = [...deltas].sort((a, b) => a - b);
  const percentile = p => sorted[Math.ceil(sorted.length * p) - 1];
  const totalDeltaUs = deltas.reduce((a, b) => a + b, 0);
  const durationUs = profile.endTime - profile.startTime;
  assert(Number.isFinite(totalDeltaUs) && totalDeltaUs <= durationUs, 'sampling deltas exceed duration');
  return {
    schema: 'dynarmic.js/cpu-samples-v1',
    methodology: 'Unweighted sample counts; inclusive counts deduplicate recursive frames per sample. '
      + 'Shares use selected samples; inclusive shares overlap and must not be summed. '
      + 'Inlining, engine/native work and GC can hide or detach caller attribution. '
      + 'This diagnostic is not a performance comparison or exact stage timing.',
    rootPattern, matchedRoots: [...selectedRoots], totalSamples: profile.samples.length, selectedSamples,
    durationUs, sampling: {totalDeltaUs, trailingUs: durationUs - totalDeltaUs,
      minDeltaUs: sorted[0], medianDeltaUs: percentile(0.5), p95DeltaUs: percentile(0.95),
      maxDeltaUs: sorted.at(-1)},
    frames: [...frames.values()].filter(f => f.inclusiveSamples > 0)
      .map(f => ({...f, selfShare: f.selfSamples / selectedSamples,
        inclusiveShare: f.inclusiveSamples / selectedSamples}))
      .sort((a, b) => b.selfSamples - a.selfSamples || b.inclusiveSamples - a.inclusiveSamples
        || JSON.stringify(a).localeCompare(JSON.stringify(b))),
  };
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    const [file, option, pattern, ...extra] = process.argv.slice(2);
    assert(file && !extra.length && (option === undefined || (option === '--root' && pattern !== undefined)),
      'usage: cpu_profile.mjs <raw.cpuprofile> [--root <function-name-regex>]');
    const raw = fs.readFileSync(file);
    const result = summarizeCpuProfile(JSON.parse(raw), pattern ?? null);
    result.input = {path: file, sha256: createHash('sha256').update(raw).digest('hex')};
    console.log(JSON.stringify(result, null, 2));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 2;
  }
}
