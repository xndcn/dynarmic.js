import {test} from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {compareRegionCorpora} from './region_corpus_compare.mjs';
import {verifyRegionCorpusSemantics} from './region_corpus_semantics.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const comparator = path.join(here, 'region_corpus_compare.mjs');
const baselineDir = path.join(here, 'region_corpus_baseline');

function manifest() {
  return {
    schema: 'dynarmic.js/region-corpus-v1',
    source_revision: '2bd45b378e205baeffe797a3d5bc320769d53167',
    emission: {link_slots_base: 1048576, fast_dispatch_base: 2097152, function_names: false, diagnostics: false},
    cases: [
      {name: 'first', entry_descriptor: '0x0000000000002000', descriptors: ['0x0000000000001000', '0x0000000000002000'],
        entry_index: 1, wasm: 'first.wasm', wasm_bytes: 9},
      {name: 'second', entry_descriptor: '0x0000000000003000', descriptors: ['0x0000000000003000'],
        entry_index: 0, wasm: 'second.wasm', wasm_bytes: 9},
    ],
  };
}

function makeCorpus(root, value = manifest(), modules = {}) {
  writeFileSync(path.join(root, 'manifest.json'), `${JSON.stringify(value, null, 2)}\n`);
  for (const item of value.cases) {
    writeFileSync(path.join(root, item.wasm), modules[item.name] ?? Buffer.from([0, 97, 115, 109, 1, 0, 0, 0, 0]));
  }
}

function withCorpora(run) {
  const parent = mkdtempSync(path.join(tmpdir(), 'dynarmic-region-corpus-'));
  const baseline = path.join(parent, 'baseline');
  const candidate = path.join(parent, 'candidate');
  try {
    for (const dir of [baseline, candidate]) {
      mkdirSync(dir);
      makeCorpus(dir);
    }
    run(baseline, candidate);
  } finally {
    rmSync(parent, {recursive: true, force: true});
  }
}

function updateManifest(dir, update) {
  const file = path.join(dir, 'manifest.json');
  const value = JSON.parse(readFileSync(file, 'utf8'));
  update(value);
  writeFileSync(file, `${JSON.stringify(value, null, 2)}\n`);
}

test('synthetic identical corpora compare by descriptor and raw bytes', () => {
  const parent = mkdtempSync(path.join(tmpdir(), 'dynarmic-region-unit-'));
  try {
    const baseline = path.join(parent, 'baseline'), candidate = path.join(parent, 'candidate');
    for (const dir of [baseline, candidate]) {
      const value = manifest();
      mkdirSync(dir);
      makeCorpus(dir, value);
    }
    const result = compareRegionCorpora(baseline, candidate);
    assert.equal(result.pass, true);
    assert.equal(result.cases.length, 2);
    assert.match(result.cases[0].sha256, /^[0-9a-f]{64}$/);
  } finally {
    rmSync(parent, {recursive: true, force: true});
  }
});

for (const [name, mutate] of [
  ['missing case', value => { value.cases.pop(); }],
  ['extra case', value => { value.cases.push({...value.cases[0], name: 'extra', wasm: 'extra.wasm'}); }],
  ['reordered descriptors', value => { value.cases[0].descriptors.reverse(); }],
  ['wrong entry_index', value => { value.cases[0].entry_index = 0; }],
  ['wrong entry descriptor', value => { value.cases[0].entry_descriptor = value.cases[0].descriptors[0]; }],
]) {
  test(`comparator rejects ${name}`, () => withCorpora((baseline, candidate) => {
    updateManifest(candidate, mutate);
    assert.throws(() => compareRegionCorpora(baseline, candidate), new RegExp(name.replace('_', '[_ ]')));
  }));
}

test('comparator rejects reordered cases', () => withCorpora((baseline, candidate) => {
  updateManifest(candidate, value => { value.cases.reverse(); });
  assert.throws(() => compareRegionCorpora(baseline, candidate), /case order/);
}));

test('comparator rejects truncated raw module', () => withCorpora((baseline, candidate) => {
  const first = candidateCase(candidate, 0);
  const bytes = readFileSync(path.join(candidate, first.wasm));
  writeFileSync(path.join(candidate, first.wasm), bytes.subarray(0, bytes.length - 1));
  assert.throws(() => compareRegionCorpora(baseline, candidate), /size|bytes/);
}));

test('comparator rejects one-byte raw module mutation even with unchanged metadata', () => withCorpora((baseline, candidate) => {
  const first = candidateCase(candidate, 0);
  const bytes = readFileSync(path.join(candidate, first.wasm));
  bytes[bytes.length - 1] ^= 1;
  writeFileSync(path.join(candidate, first.wasm), bytes);
  assert.throws(() => compareRegionCorpora(baseline, candidate), /raw Wasm|Buffer|bytes/);
}));

function candidateCase(dir, index) {
  return JSON.parse(readFileSync(path.join(dir, 'manifest.json'), 'utf8')).cases[index];
}

test('comparator accepts identical output with honest post-change candidate provenance', () => withCorpora((baseline, candidate) => {
  updateManifest(candidate, value => { value.source_revision = 'f'.repeat(40); });
  assert.equal(compareRegionCorpora(baseline, candidate).pass, true);
}));

test('comparator accepts source archives without inventing a Git revision', () => withCorpora((baseline, candidate) => {
  updateManifest(candidate, value => { value.source_revision = null; });
  assert.equal(compareRegionCorpora(baseline, candidate).pass, true);
}));

for (const [name, mutate, expected] of [
  ['malformed JSON', dir => { writeFileSync(path.join(dir, 'manifest.json'), '{\n'); }, /JSON|Expected|Unexpected/],
  ['wrong schema', (dir, update) => update(value => { value.schema = 'wrong'; }), /schema/],
  ['malformed revision', (dir, update) => update(value => { value.source_revision = 'HEAD'; }), /revision/],
  ['unpinned baseline revision', (dir, update) => update(value => { value.source_revision = 'f'.repeat(40); }), /baseline.*revision|pinned/],
  ['duplicate cases', (dir, update) => update(value => { value.cases.push({...value.cases[0]}); }), /duplicate/],
  ['malformed descriptor', (dir, update) => update(value => { value.cases[0].descriptors[0] = '0x1000'; }), /descriptor/],
  ['negative entry index', (dir, update) => update(value => { value.cases[0].entry_index = -1; }), /entry_index/],
  ['out-of-range entry index', (dir, update) => update(value => { value.cases[0].entry_index = 2; }), /entry_index/],
  ['absolute Wasm path', (dir, update) => update(value => { value.cases[0].wasm = '/tmp/stolen.wasm'; }), /path/],
  ['traversal Wasm path', (dir, update) => update(value => { value.cases[0].wasm = '../stolen.wasm'; }), /path/],
  ['wrong module size', (dir, update) => update(value => { value.cases[0].wasm_bytes++; }), /size/],
  ['invalid Wasm header', dir => {
    const item = candidateCase(dir, 0), bytes = readFileSync(path.join(dir, item.wasm));
    bytes[0] ^= 1;
    writeFileSync(path.join(dir, item.wasm), bytes);
  }, /header/],
]) {
  test(`comparator fails closed on ${name}`, () => withCorpora((baseline, candidate) => {
    const target = name === 'unpinned baseline revision' ? baseline : candidate;
    mutate(target, update => updateManifest(target, update));
    assert.throws(() => compareRegionCorpora(baseline, candidate), expected);
  }));
}

test('comparator CLI returns zero for equality and nonzero for mutations', () => withCorpora((baseline, candidate) => {
  assert.equal(spawnSync(process.execPath, [comparator, baseline, candidate]).status, 0);
  updateManifest(candidate, value => { value.cases[1].entry_index = 1; });
  const failed = spawnSync(process.execPath, [comparator, baseline, candidate]);
  assert.equal(failed.status, 1);
  assert.match(failed.stderr.toString(), /entry_index/);
}));

test('checked-in corpus has the five permanent cases and raw modules', () => {
  const value = JSON.parse(readFileSync(path.join(baselineDir, 'manifest.json'), 'utf8'));
  assert.deepEqual(value.cases.map(item => item.name), [
    'a32_conditional_backward',
    'a64_single_block',
    'a64_backward_conditional',
    'a64_bl_ret_push_rsb',
    'a64_max_64_blocks',
  ]);
  for (const item of value.cases) {
    assert(item.descriptors.length > 0);
    assert(item.entry_index >= 0 && item.entry_index < item.descriptors.length);
    assert.equal(item.entry_descriptor, item.descriptors[item.entry_index]);
    const bytes = readFileSync(path.join(baselineDir, item.wasm));
    assert.equal(bytes.length, item.wasm_bytes);
    assert(WebAssembly.validate(bytes), `${item.name}: baseline must contain a valid Wasm module`);
  }
  assert.deepEqual(value.cases[0].descriptors, [
    '0x0000000000001000',
    '0x0000000000002000',
    '0x0000000000002004',
    '0x0000000000002008',
  ], 'A32 branches must target valid mapped instructions using PC+8 displacement');
  assert.equal(value.cases[0].entry_index, 1);
  assert(value.cases[2].entry_index > 0, 'backward region must retain a nonzero sorted entry index');
  assert.equal(value.cases[4].descriptors.length, 64);
});

test('native recorder refuses stale output instead of overwriting it', () => {
  const recorder = process.env.DYNARMIC_REGION_CORPUS_RECORDER;
  const actual = process.env.DYNARMIC_REGION_CORPUS_ACTUAL;
  const revision = process.env.DYNARMIC_REGION_CORPUS_REVISION;
  assert(recorder && actual && (revision === 'source-archive' || /^[0-9a-f]{40}$/.test(revision)),
    'native recorder paths and revision must be supplied by CTest');
  const result = spawnSync(recorder, ['--output', actual, '--source-revision', revision]);
  assert.notEqual(result.status, 0);
  assert.match(result.stderr.toString(), /stale|already exists/i);
});

test('checked-in corpus executes with the specified guest semantics', () => {
  assert.equal(verifyRegionCorpusSemantics(baselineDir).executions, 28);
});

test('native recorder output executes with the specified guest semantics', () => {
  const actual = process.env.DYNARMIC_REGION_CORPUS_ACTUAL;
  assert(actual, 'DYNARMIC_REGION_CORPUS_ACTUAL must point at a fresh native recording');
  assert.equal(verifyRegionCorpusSemantics(actual).executions, 28);
});

test('native recorder output exactly matches the reviewed corpus', () => {
  const actual = process.env.DYNARMIC_REGION_CORPUS_ACTUAL;
  const revision = process.env.DYNARMIC_REGION_CORPUS_REVISION;
  assert(actual, 'DYNARMIC_REGION_CORPUS_ACTUAL must point at a fresh native recording');
  const actualManifest = JSON.parse(readFileSync(path.join(actual, 'manifest.json'), 'utf8'));
  assert.equal(actualManifest.source_revision, revision === 'source-archive' ? null : revision,
    'recorder must report current, not frozen baseline, provenance');
  const result = compareRegionCorpora(baselineDir, actual);
  assert.equal(result.pass, true);
});
