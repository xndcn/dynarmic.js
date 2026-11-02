import {test} from 'node:test';
import assert from 'node:assert/strict';
import {cacheCases, cacheMethodology, summarizeCache, validateCacheReport, validateCacheSample} from './cache_protocol.mjs';
import {report as fixedReport, samples as fixedSamples} from './fixtures.mjs';
import {cases as fixedCases, validateReport} from './protocol.mjs';
import {populationCases, validatePopulationReport} from './cache_population_protocol.mjs';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';

const sample = (cycle, process = 0, profile = false) => ({
  cycle, process, value: cycle + 1, ms: 1,
  compile: {blocks: 1, bytes: 100, translateMs: 0.1, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0},
  profile: profile ? {
    schema: 'dynarmic.js/profile-v1', enabled: true, runCount: 1, stepCount: 0,
    compiledBlocks: 1, emittedBytes: 100, peakCachedBlocks: cycle === 0 ? 1 : 0,
    peakCachedBytes: cycle === 0 ? 100 : 0,
    ...Object.fromEntries(['dispatchCount', 'dispatchCacheHits', 'dispatchCacheMisses', 'compiledRegions',
      'regionBlocks', 'linkPatches', 'fastDispatchMisses', 'cycleBudgetExits', 'regionCandidates',
      'regionRejectedIncompatible', 'regionRejectedDistance', 'regionRejectedBlockBudget',
      'regionRejectedIrBudget'].map(key => [key, 0])),
    invalidationCount: cycle === 0 ? 0 : 1, invalidatedBlocks: cycle === 0 ? 0 : 1,
  } : null,
});

test('cache sample requires recompilation on every cycle', () => {
  assert.doesNotThrow(() => validateCacheSample(sample(0), false));
  assert.throws(() => validateCacheSample({...sample(1), compile: {...sample(1).compile, blocks: 0}}, false));
});

test('profile cache cycles require invalidation accounting', () => {
  assert.doesNotThrow(() => validateCacheSample(sample(0, 0, true), true));
  assert.doesNotThrow(() => validateCacheSample(sample(1, 0, true), true));
  const broken = sample(1, 0, true);
  broken.profile.invalidationCount = 0;
  assert.throws(() => validateCacheSample(broken, true));
});

test('cache summary includes one median per cycle', () => {
  const samples = [...Array(cacheMethodology.cycles).keys()].map(cycle => sample(cycle, 0, true));
  const summary = summarizeCache(samples, true);
  assert.equal(summary.cycleMedianMs.length, cacheMethodology.cycles);
  assert.equal(summary.peakCachedBytesMedian, 100);
});

test('cache report rejects missing case and wrong cycle order', () => {
  const samples = [...Array(cacheMethodology.cycles).keys()].map(cycle => sample(cycle, 0, false));
  const report = {
    schema: 'dynarmic.js/cache-benchmark-v1', timestamp: '2026-10-22T00:00:00.000Z', node: 'node', v8: 'v8',
    host: {platform: 'linux', release: 'test', arch: 'x64', cpu: 'test', cpus: 1},
    runtime: {nodeOptions: '', cpuAffinity: '0'},
    git: {revision: 'a'.repeat(40), upstream: 'b'.repeat(40), dirty: false},
    build: fixedReport().build, methodology: cacheMethodology,
    cases: cacheCases.map(spec => ({...spec, architecture: 'a32', runs: 1, workloadSha256: 'c'.repeat(64),
      artifact: {jsSha256: 'd'.repeat(64), wasmSha256: 'e'.repeat(64)}, samples: [...samples],
      summary: summarizeCache(samples, false)})),
  };
  assert.doesNotThrow(() => validateCacheReport(report));
  report.cases.pop();
  assert.throws(() => validateCacheReport(report));
});

function collectorFixture(t, {suite = 'cache', wrong = false, mutation = false, status = 0, invalid = false, failAt = 0} = {}) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'cache-collector-'));
  t.after(() => fs.rmSync(root, {recursive: true, force: true}));
  const source = path.join(root, 'source'), build = path.join(root, 'build'), host = path.join(build, 'tests');
  fs.mkdirSync(host, {recursive: true});
  fs.mkdirSync(path.join(source, 'tests/hosts'), {recursive: true});
  fs.mkdirSync(path.join(source, 'externals/dynarmic'), {recursive: true});
  for (const dir of [source, path.join(source, 'externals/dynarmic')]) {
    for (const args of [['init', '-q'], ['-c', 'user.name=Test', '-c', 'user.email=test@example.invalid',
      '-c', 'core.hooksPath=/dev/null', '-c', 'commit.gpgsign=false', 'commit', '-q', '--allow-empty', '-m', 'fixture']]) {
      const result = spawnSync('git', args, {cwd: dir, encoding: 'utf8'});
      assert.equal(result.status, 0, result.stderr);
    }
  }
  fs.mkdirSync(path.join(build, 'CMakeFiles/1'), {recursive: true});
  fs.writeFileSync(path.join(build, 'CMakeCache.txt'), `CMAKE_HOME_DIRECTORY:INTERNAL=${source}
DYNARMIC_JS_PROFILE:BOOL=OFF
DYNARMIC_JS_A64_REGION_FOCUS:STRING=
CMAKE_BUILD_TYPE:STRING=Release
CMAKE_CXX_FLAGS:STRING=-Wall
CMAKE_CXX_FLAGS_RELEASE:STRING=-O3
CMAKE_EXE_LINKER_FLAGS:STRING=
CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=
`);
  fs.writeFileSync(path.join(build, 'CMakeFiles/1/CMakeCXXCompiler.cmake'),
    `set(CMAKE_CXX_COMPILER "${build}/em++")\nset(CMAKE_CXX_COMPILER_ID "Clang")\nset(CMAKE_CXX_COMPILER_VERSION "test")\n`);
  fs.writeFileSync(path.join(build, 'emscripten-version.txt'), 'test\n');
  const order = path.join(root, 'order.jsonl');
  const specs = suite === 'fixed' ? fixedCases : suite === 'population'
    ? populationCases.map(spec => ({...spec, file: 'jit_cachepopbench.js'}))
    : cacheCases.map(spec => ({...spec, file: 'jit_cachebench.js'}));
  fs.writeFileSync(path.join(source, 'tests/hosts/benchmark.h'), 'synthetic measurement helper');
  const rows = suite === 'fixed' ? JSON.stringify(fixedSamples()) : suite === 'cache'
    ? JSON.stringify(Array.from({length: 8}, (_, cycle) => sample(cycle)))
    : `['cold', 'warm'].map(phase => ({phase, population: Number(process.argv[3]), ms: 1, profile: null,
      compile: {blocks: phase === 'cold' ? Number(process.argv[3]) : 0,
        bytes: phase === 'cold' ? 100 * Number(process.argv[3]) : 0,
        translateMs: 0, optMs: 0, emitMs: 0, compileMs: 0, instMs: 0, tableMs: 0}}))`;
  const prefix = suite === 'fixed' ? 'BENCHMARK ' : suite === 'cache' ? 'CACHE_BENCHMARK ' : 'CACHE_POPULATION ';
  const pass = suite === 'fixed' ? 'PASS' : suite === 'cache' ? 'PASS cache pressure' : 'PASS cache population';
  for (const file of new Set(specs.map(spec => spec.file))) {
    fs.writeFileSync(path.join(source, 'tests/hosts', file.replace(/\.js$/, '.cpp')), 'synthetic workload');
    fs.writeFileSync(path.join(host, file.replace(/\.js$/, '.wasm')), 'synthetic artifact');
    fs.writeFileSync(path.join(host, file), `const fs = require('node:fs');
const order = ${JSON.stringify(order)};
const ordinal = fs.existsSync(order) ? fs.readFileSync(order, 'utf8').trim().split('\\n').length : 0;
const selected = ordinal === ${failAt};
fs.appendFileSync(order, (process.argv[2] || 'none') + '\\n');
const rows = ${rows};
if (selected && ${invalid}) rows[0].ms = -1;
for (const row of rows) console.log(${JSON.stringify(prefix)} + JSON.stringify(row));
${wrong ? "if (selected) console.log('WRONG RESULT');" : ''}
${mutation ? "if (selected) fs.appendFileSync(__filename.replace('.js', '.wasm'), 'mutation');" : ''}
console.error('fixture diagnostic process ' + ordinal);
console.log(${JSON.stringify(pass)}); process.exit(selected ? ${status} : 0);
`);
  }
  const output = path.join(root, 'report.json');
  const collector = suite === 'fixed' ? 'run.mjs' : suite === 'cache' ? 'cache_run.mjs' : 'cache_population_run.mjs';
  const run = (extra = [], env = process.env) => spawnSync(process.execPath,
    [new URL(collector, import.meta.url).pathname, host, '--runs', '2', '--output', output, ...extra],
    {encoding: 'utf8', timeout: 10000, env});
  return {root, output, order, run};
}

test('cache collector records full provenance and rotates cases without overwriting prior reports', t => {
  const f = collectorFixture(t), result = f.run();
  assert.equal(result.status, 0, result.stderr);
  const text = fs.readFileSync(f.output, 'utf8'), report = JSON.parse(text);
  validateCacheReport(report);
  assert.equal(report.build.configCxxFlags, '-O3');
  assert.equal(report.build.a64RegionFocus, '');
  assert.equal(report.runtime.nodeOptions, process.env.NODE_OPTIONS || '');
  assert.deepEqual(fs.readFileSync(f.order, 'utf8').trim().split('\n'), ['0', '1', '2', '1', '2', '0']);
  assert.notEqual(f.run().status, 0);
  assert.equal(fs.readFileSync(f.output, 'utf8'), text);
});
for (const [name, options] of [['wrong guest with PASS', {wrong: true}], ['artifact mutation', {mutation: true}],
  ['nonzero exit with PASS', {status: 1}]]) {
  test(`cache collector rejects ${name}`, t => {
    const f = collectorFixture(t, options);
    assert.notEqual(f.run().status, 0);
    assert(!fs.existsSync(f.output));
    const text = fs.readFileSync(`${f.output}.failure.json`, 'utf8'), failure = JSON.parse(text);
    assert.equal(failure.suite, 'cache');
    assert(failure.observations[0].stdout.includes('PASS cache pressure'));
    assert(failure.observations[0].stderr.includes('fixture diagnostic process 0'));
    assert.notEqual(f.run().status, 0);
    assert.equal(fs.readFileSync(`${f.output}.failure.json`, 'utf8'), text);
  });
}
test('cache collector rejects incomplete options and instrumentation before launching', t => {
  const f = collectorFixture(t);
  assert.notEqual(f.run(['--output']).status, 0);
  assert.notEqual(f.run([], {...process.env, DYN_DUMP_DIR: f.root}).status, 0);
  assert(!fs.existsSync(f.order));
});

for (const [suite, validate] of [['fixed', validateReport], ['population', validatePopulationReport]]) {
  test(`${suite} collector validates complete reports and refuses to overwrite them`, t => {
    const f = collectorFixture(t, {suite});
    const result = f.run(); assert.equal(result.status, 0, result.stderr);
    const text = fs.readFileSync(f.output, 'utf8'); validate(JSON.parse(text));
    const order = fs.readFileSync(f.order, 'utf8');
    assert.notEqual(f.run().status, 0);
    assert.equal(fs.readFileSync(f.output, 'utf8'), text);
    assert.equal(fs.readFileSync(f.order, 'utf8'), order, 'must reject existing output before launching');
    assert(!fs.existsSync(`${f.output}.failure.json`));
  });
}

for (const suite of ['fixed', 'cache', 'population']) {
  for (const [name, options] of [['nonzero exit after PASS', {status: 7}], ['invalid timing with PASS', {invalid: true}]]) {
    test(`${suite} collector preserves raw output and earlier samples on ${name}`, t => {
      const f = collectorFixture(t, {suite, failAt: 1, ...options});
      assert.notEqual(f.run().status, 0);
      assert(!fs.existsSync(f.output));
      const text = fs.readFileSync(`${f.output}.failure.json`, 'utf8'), failure = JSON.parse(text);
      assert.equal(failure.schema, 'dynarmic.js/benchmark-collection-failure-v1');
      assert.equal(failure.suite, suite === 'population' ? 'cache-population' : suite);
      assert.equal(failure.observations[1].status, options.status || 0);
      assert(failure.observations[1].stdout.includes('PASS'));
      assert(failure.observations[1].stderr.includes('fixture diagnostic process 1'));
      assert.equal(failure.completedSamples[0].samples[0].ms, 1);
      assert(failure.completedSamples[0].samples.length >= (suite === 'fixed' ? 9 : suite === 'cache' ? 8 : 2));
      if (options.status) assert.equal(failure.completedSamples[1].samples.length, 0);
      const order = fs.readFileSync(f.order, 'utf8');
      assert.notEqual(f.run().status, 0);
      assert.equal(fs.readFileSync(`${f.output}.failure.json`, 'utf8'), text);
      assert.equal(fs.readFileSync(f.order, 'utf8'), order, 'must reject retained failure before launching');
    });
  }
}
