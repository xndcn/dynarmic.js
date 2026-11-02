import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {summarizeCpuProfile} from './cpu_profile.mjs';

function fixture() {
  const node = (id, functionName, children = [], url = 'compiler.wasm') => ({id, children,
    callFrame: {functionName, scriptId: '1', url, lineNumber: 0, columnNumber: functionName.length}});
  return {startTime: 100, endTime: 220,
    nodes: [node(1, '(root)', [2, 6]), node(2, 'Compile', [3, 5]), node(3, 'Decode', [4]),
      node(4, 'Decode'), node(5, 'Decode', [], 'different.wasm'), node(6, '(garbage collector)')],
    samples: [2, 3, 4, 5, 6], timeDeltas: [10, 10, 70, 10, 10]};
}

test('unweighted counts preserve frame identity and deduplicate recursive inclusive samples', () => {
  const r = summarizeCpuProfile(fixture());
  assert.equal(r.totalSamples, 5);
  assert.equal(r.selectedSamples, 5);
  assert.equal(r.frames.reduce((n, f) => n + f.selfSamples, 0), 5);
  const compile = r.frames.find(f => f.functionName === 'Compile');
  assert.deepEqual([compile.selfSamples, compile.inclusiveSamples], [1, 4]);
  const decodes = r.frames.filter(f => f.functionName === 'Decode');
  assert.equal(decodes.length, 2);
  assert.deepEqual(decodes.map(f => [f.selfSamples, f.inclusiveSamples]), [[2, 2], [1, 1]]);
  assert.deepEqual(r.sampling, {totalDeltaUs: 110, trailingUs: 10, minDeltaUs: 10,
    medianDeltaUs: 10, p95DeltaUs: 70, maxDeltaUs: 70});
});

test('subtree shares exclude callers and detached GC; overlapping roots do not duplicate samples', () => {
  const r = summarizeCpuProfile(fixture(), 'Compile|Decode');
  assert.equal(r.selectedSamples, 4);
  assert.equal(r.matchedRoots.length, 4);
  assert.equal(r.frames.some(f => f.functionName.startsWith('(')), false);
  assert.equal(r.frames.reduce((n, f) => n + f.selfShare, 0), 1);
  const decode = summarizeCpuProfile(fixture(), '^Decode$');
  assert.equal(decode.selectedSamples, 3);
  assert.equal(decode.frames.reduce((n, f) => n + f.selfSamples, 0), 3);
});

test('hitCount is not substituted for samples; tree order and node IDs are irrelevant', () => {
  const p = fixture(), expected = summarizeCpuProfile(p);
  p.nodes.reverse();
  p.nodes.forEach(n => { n.hitCount = 100000; });
  assert.deepEqual(summarizeCpuProfile(p), expected);
});

for (const [name, mutate] of [
  ['duplicate IDs', p => p.nodes.push(p.nodes[0])],
  ['unknown child', p => p.nodes[0].children.push(20)],
  ['multiple parents', p => p.nodes[0].children.push(4)],
  ['disconnected cycle', p => { p.nodes[1].children = []; p.nodes[3].children = [3]; }],
  ['root cycle', p => p.nodes[5].children.push(1)],
  ['multiple roots', p => p.nodes[0].children.pop()],
  ['invalid frame', p => delete p.nodes[1].callFrame.url],
  ['unknown sample', p => p.samples[0] = 30],
  ['missing samples', p => p.samples = []],
  ['missing deltas', p => p.timeDeltas.pop()],
  ['negative delta', p => p.timeDeltas[1] = -1],
  ['nonfinite delta', p => p.timeDeltas[0] = Infinity],
  ['time overflow', p => p.timeDeltas[0] = 1000],
  ['invalid range', p => p.endTime = 0],
]) test(`reject ${name}`, () => {
  const p = fixture(); mutate(p);
  assert.throws(() => summarizeCpuProfile(p));
});

test('root filter fails on invalid regex, missing frame and unsampled subtree', () => {
  assert.throws(() => summarizeCpuProfile(fixture(), '['));
  assert.throws(() => summarizeCpuProfile(fixture(), 'absent'), /no call frames/);
  const p = fixture(); p.samples.fill(6);
  assert.throws(() => summarizeCpuProfile(p, 'Compile'), /no samples/);
});

test('CLI binds raw input hash and reports malformed input with exit 2', t => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'cpu-profile-test-'));
  t.after(() => fs.rmSync(dir, {recursive: true, force: true}));
  const file = path.join(dir, 'profile.json');
  fs.writeFileSync(file, JSON.stringify(fixture()));
  const run = (...args) => spawnSync(process.execPath, [new URL('./cpu_profile.mjs', import.meta.url).pathname,
    ...args], {encoding: 'utf8'});
  const good = run(file, '--root', 'Compile');
  assert.equal(good.status, 0, good.stderr);
  assert.equal(JSON.parse(good.stdout).selectedSamples, 4);
  assert.match(JSON.parse(good.stdout).input.sha256, /^[a-f0-9]{64}$/);
  assert.equal(run(file, '--root').status, 2);
  assert.equal(run(file, '--wat').status, 2);
  assert.equal(run(file, '--root', 'absent').status, 2);
  fs.writeFileSync(file, '{');
  assert.equal(run(file).status, 2);
});
