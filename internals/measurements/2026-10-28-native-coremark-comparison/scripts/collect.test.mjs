import {test} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtempSync, readFileSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {createBenchmarkEnvironment, parseCollectArguments, runWithFailureEvidence, streamIdentity, validateWasmBuildManifest,
  validateWasmProductionBuild} from './collect_support.mjs';

const validBuild = {
  sourceRoot: '/repo',
  build: {type: 'Release', profile: false, a64RegionFocus: '', cxxFlags: '', configCxxFlags: '-O3 -DNDEBUG',
    linkFlags: '', configLinkFlags: ''},
};
const validFeatures = {regionDiagnostics: false, focusTestExceptions: false, examples: true};
const validSource = {revision: '1'.repeat(40), upstream: '2'.repeat(40), dirty: false};

test('collector arguments preserve the three-way default and opt into four-way explicitly', () => {
  assert.deepEqual(parseCollectArguments(['linux.json', 'host', 'guest', 'out']), {runs: 5, wasmBuildDir: null});
  assert.deepEqual(parseCollectArguments(['linux.json', 'host', 'guest', 'out', '3']), {runs: 3, wasmBuildDir: null});
  assert.deepEqual(parseCollectArguments(['linux.json', 'host', 'guest', 'out', '2', 'build-em']),
    {runs: 2, wasmBuildDir: 'build-em'});
});

for (const [name, args, pattern] of [
  ['missing output', ['linux.json', 'host', 'guest'], /usage/],
  ['extra argument', ['linux.json', 'host', 'guest', 'out', '5', 'build-em', 'extra'], /usage/],
  ['fractional runs', ['linux.json', 'host', 'guest', 'out', '1.5'], /run count/],
  ['zero runs', ['linux.json', 'host', 'guest', 'out', '0'], /run count/],
  ['excess runs', ['linux.json', 'host', 'guest', 'out', '21'], /run count/],
]) test(`collector rejects ${name}`, () => assert.throws(() => parseCollectArguments(args), pattern));

test('collector accepts only a clean production Wasm build', () => {
  assert.equal(validateWasmProductionBuild(validBuild, validFeatures, validSource), true);
});

test('collector flag validation does not reject unrelated macro prefixes', () => {
  const info = structuredClone(validBuild);
  info.build.cxxFlags = '-DDYNARMIC_WASM_PROFILE_ADD=1 -DOTHER=DYNARMIC_JS_A64_REGION_FOCUS';
  assert.equal(validateWasmProductionBuild(info, validFeatures, validSource), true);
});

for (const [name, change, pattern] of [
  ['debug build', data => { data.build.build.type = 'Debug'; }, /Release/],
  ['profiling', data => { data.build.build.profile = true; }, /profil/],
  ['region focus', data => { data.build.build.a64RegionFocus = '0x1'; }, /focus/],
  ['region diagnostics', data => { data.features.regionDiagnostics = true; }, /diagnostics/],
  ['focus exceptions', data => { data.features.focusTestExceptions = true; }, /exceptions/],
  ['disabled examples', data => { data.features.examples = false; }, /examples/],
  ['dirty source', data => { data.source.dirty = true; }, /clean/],
  ['focus macro in global flags', data => {
    data.build.build.cxxFlags = '-DDYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\"';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['focus macro in Release flags', data => {
    data.build.build.configCxxFlags = '-O3 -D DYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\"';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['quoted focus macro in global flags', data => {
    data.build.build.cxxFlags = '"-DDYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\""';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['Xclang focus macro forwarding', data => {
    data.build.build.cxxFlags = '-Xclang -D -Xclang DYNARMIC_JS_A64_REGION_FOCUS=\\"0x1,0x2,0x3\\"';
  }, /indirect compiler input/],
  ['Xpreprocessor focus macro forwarding', data => {
    data.build.build.cxxFlags = '-Xpreprocessor -D -Xpreprocessor DYNARMIC_JS_A64_REGION_FOCUS';
  }, /indirect compiler input/],
  ['preprocessor focus macro forwarding', data => {
    data.build.build.cxxFlags = '-Wp,-D,DYNARMIC_JS_A64_REGION_FOCUS';
  }, /indirect compiler input/],
  ['response file flags', data => {
    data.build.build.cxxFlags = '@/tmp/focus.rsp';
  }, /indirect compiler input/],
  ['forced include flags', data => {
    data.build.build.cxxFlags = '-include /tmp/focus.h';
  }, /indirect compiler input/],
  ['attached forced include flags', data => {
    data.build.build.cxxFlags = '-include/tmp/focus.h';
  }, /indirect compiler input/],
  ['quoted attached forced macros flags', data => {
    data.build.build.cxxFlags = '"-imacros/tmp/focus.h"';
  }, /indirect compiler input/],
  ['long profile macro definition', data => {
    data.build.build.cxxFlags = '--define-macro=DYNARMIC_WASM_PROFILE=0';
  }, /DYNARMIC_WASM_PROFILE/],
  ['long diagnostics macro definition', data => {
    data.build.build.configCxxFlags = '--define-macro DYNARMIC_WASM_REGION_DIAGNOSTICS=1';
  }, /DYNARMIC_WASM_REGION_DIAGNOSTICS/],
  ['long focus macro definition', data => {
    data.build.build.cxxFlags = '--define-macro=DYNARMIC_JS_A64_REGION_FOCUS=1';
  }, /DYNARMIC_JS_A64_REGION_FOCUS/],
  ['long forced include flags', data => {
    data.build.build.cxxFlags = '--include=/tmp/focus.h';
  }, /indirect compiler input/],
  ['long forced macros flags', data => {
    data.build.build.cxxFlags = '--imacros /tmp/focus.h';
  }, /indirect compiler input/],
  ['compiler configuration file flags', data => {
    data.build.build.cxxFlags = '--config=/tmp/focus.cfg';
  }, /indirect compiler input/],
  ['profile macro in global flags', data => {
    data.build.build.cxxFlags = '-DDYNARMIC_WASM_PROFILE=1';
  }, /DYNARMIC_WASM_PROFILE/],
  ['diagnostics macro in Release flags', data => {
    data.build.build.configCxxFlags = '-O3 /DDYNARMIC_WASM_REGION_DIAGNOSTICS';
  }, /DYNARMIC_WASM_REGION_DIAGNOSTICS/],
]) test(`collector rejects ${name}`, () => {
  const data = structuredClone({build: validBuild, features: validFeatures, source: validSource});
  change(data);
  assert.throws(() => validateWasmProductionBuild(data.build, data.features, data.source), pattern);
});

test('benchmark children receive only a fixed minimal environment', () => {
  assert.deepEqual(createBenchmarkEnvironment(), {
    PATH: '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
    LC_ALL: 'C',
    NODE_OPTIONS: '',
  });
});

test('collector stream identity covers exact bytes', () => {
  assert.deepEqual(streamIdentity(Buffer.from('abc')), {
    sha256: 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', bytes: 3,
  });
  assert.notDeepEqual(streamIdentity(Buffer.from('abc')), streamIdentity(Buffer.from('abd')));
});

const validArtifacts = {
  js: {path: '/build/arm_jit.js', sha256: '3'.repeat(64), bytes: 100},
  wasm: {path: '/build/arm_jit.wasm', sha256: '4'.repeat(64), bytes: 200},
};
const validBuildSources = {browserCmakeSha256: '5'.repeat(64), manifestScriptSha256: '6'.repeat(64)};
const validIdentity = {info: validBuild, features: validFeatures, source: validSource,
  artifacts: validArtifacts, buildSources: validBuildSources};
const validManifest = {
  schema: 'dynarmic.js/wasm-coremark-host-build-v1', sourceRoot: validBuild.sourceRoot,
  build: structuredClone(validBuild.build), configuration: structuredClone(validFeatures),
  buildEnvironment: {configureEmccCflags: '', buildEmccCflags: ''},
  source: structuredClone(validSource), artifacts: structuredClone(validArtifacts),
  buildSources: structuredClone(validBuildSources),
};

test('collector requires the exact post-link Wasm build identity', () => {
  assert.equal(validateWasmBuildManifest(validManifest, validIdentity), true);
});

for (const [name, change] of [
  ['stale artifact under a production cache', data => { data.identity.artifacts.js.sha256 = '7'.repeat(64); }],
  ['stale Debug manifest under a production cache', data => { data.manifest.build.type = 'Debug'; }],
  ['changed source revision', data => { data.identity.source.revision = '8'.repeat(40); }],
  ['changed browser CMake', data => { data.identity.buildSources.browserCmakeSha256 = '9'.repeat(64); }],
  ['missing buildSources', data => { delete data.manifest.buildSources; }],
  ['collector-injected runtime', data => { data.manifest.runtime = {nodeVersion: 'v1'}; }],
  ['build-time EMCC_CFLAGS injection', data => {
    data.manifest.buildEnvironment.buildEmccCflags = '-DDYNARMIC_WASM_PROFILE=1';
  }],
]) test(`collector rejects ${name}`, () => {
  const data = structuredClone({manifest: validManifest, identity: validIdentity});
  change(data);
  assert.throws(() => validateWasmBuildManifest(data.manifest, data.identity));
});

test('collector CLI usage errors retain exit status 64', () => {
  const collector = fileURLToPath(new URL('./collect.mjs', import.meta.url));
  const environment = {...process.env};
  delete environment.NODE_V8_COVERAGE;
  const result = spawnSync(process.execPath, [collector], {encoding: 'utf8', env: environment});
  assert.equal(result.status, 64);
  assert.match(result.stderr, /^usage: collect\.mjs /);
  assert.doesNotMatch(result.stderr, /AssertionError|at parseCollectArguments/);
});

test('unified cohort boundary preserves post-cohort verification failures', t => {
  const output = mkdtempSync(join(tmpdir(), 'collector-failure-test-'));
  t.after(() => rmSync(output, {recursive: true, force: true}));
  assert.throws(() => runWithFailureEvidence(output, () => {
    throw new Error('artifact changed after final sample');
  }), /artifact changed/);
  const failure = JSON.parse(readFileSync(join(output, 'failure.json'), 'utf8'));
  assert.match(failure.failure, /artifact changed after final sample/);
  assert(Number.isFinite(Date.parse(failure.failedAt)));
});
