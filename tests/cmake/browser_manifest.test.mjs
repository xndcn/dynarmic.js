import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {execFileSync, spawnSync} from 'node:child_process';
import {mkdtempSync, readFileSync, rmSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {sourceInfo} from '../bench/environment.mjs';

const generator = fileURLToPath(new URL('../../examples/browser/write_build_manifest.cmake', import.meta.url));
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
function git(cwd, args) {
  return execFileSync('git', ['-C', cwd, ...args], {encoding: 'utf8'}).trim();
}
function repository(root) {
  git(root, ['init', '-q']);
  git(root, ['config', 'user.name', 'Build Manifest Test']);
  git(root, ['config', 'user.email', 'manifest@example.invalid']);
  writeFileSync(join(root, 'tracked.txt'), 'initial\n');
  git(root, ['add', 'tracked.txt']);
  git(root, ['commit', '-q', '-m', 'initial']);
  return git(root, ['rev-parse', 'HEAD']);
}
function fixture(t) {
  const root = mkdtempSync(join(tmpdir(), 'wasm-build-manifest-test-'));
  t.after(() => rmSync(root, {recursive: true, force: true}));
  const source = join(root, 'source');
  const upstream = join(source, 'externals/dynarmic');
  const output = join(root, 'output');
  execFileSync('mkdir', ['-p', upstream, output]);
  repository(source);
  const upstreamRevision = repository(upstream);
  git(source, ['-c', 'protocol.file.allow=always', 'submodule', 'add', '-q', upstream, 'externals/dynarmic']);
  git(source, ['commit', '-q', '-m', 'add upstream']);
  const revision = git(source, ['rev-parse', 'HEAD']);
  const js = join(output, 'arm_jit.js');
  const wasm = join(output, 'arm_jit.wasm');
  const manifest = join(output, 'wasm-build-manifest.json');
  writeFileSync(js, 'javascript\n');
  writeFileSync(wasm, Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]));
  const args = [
    '-DBUILD_TYPE=Release', '-DCXX_FLAGS=', '-DCONFIG_FLAGS=-O3 -DNDEBUG',
    '-DLINK_FLAGS=', '-DCONFIG_LINK_FLAGS=', '-DCOMPILER_ID=Clang',
    '-DCOMPILER_VERSION=24.0.0', '-DEMSCRIPTEN_VERSION=6.0.13',
    '-DPROFILE=OFF', '-DREGION_DIAGNOSTICS=OFF', '-DA64_REGION_FOCUS=',
    '-DFOCUS_TEST_EXCEPTIONS=OFF', '-DEXAMPLES=ON', '-DCONFIGURE_EMCC_CFLAGS=', `-DSOURCE_ROOT=${source}`,
    `-DBROWSER_CMAKE_SHA256=${'1'.repeat(64)}`, `-DMANIFEST_SCRIPT_SHA256=${'2'.repeat(64)}`,
    `-DJS_FILE=${js}`, `-DWASM_FILE=${wasm}`, `-DOUTPUT_FILE=${manifest}`,
  ];
  return {root, source, revision, upstreamRevision, js, wasm, manifest, args};
}
function readManifest(path) {
  const text = readFileSync(path, 'utf8');
  try {
    return JSON.parse(text);
  } catch (error) {
    throw new Error(`${error.message}\n${text}`);
  }
}
function generate(args, environment = {}) {
  const env = {...process.env, ...environment};
  if (!Object.hasOwn(environment, 'EMCC_CFLAGS')) delete env.EMCC_CFLAGS;
  return spawnSync('cmake', [...args, '-P', generator], {encoding: 'utf8', env});
}

test('Wasm post-link manifest binds source, production configuration and actual artifacts', t => {
  const data = fixture(t);
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  const manifest = readManifest(data.manifest);
  assert.equal(manifest.schema, 'dynarmic.js/wasm-coremark-host-build-v1');
  assert.equal(manifest.sourceRoot, data.source);
  assert.deepEqual(manifest.build, {
    type: 'Release', profile: false, a64RegionFocus: '', compilerId: 'Clang',
    compilerVersion: '24.0.0', emscriptenVersion: '6.0.13', cxxFlags: '',
    configCxxFlags: '-O3 -DNDEBUG', linkFlags: '', configLinkFlags: '',
  });
  assert.deepEqual(manifest.configuration, {
    regionDiagnostics: false, focusTestExceptions: false, examples: true,
    guestRamBytes: null, initialMemoryBytes: null,
  });
  assert.deepEqual(manifest.buildEnvironment, {configureEmccCflags: '', buildEmccCflags: ''});
  assert.deepEqual(manifest.source, {revision: data.revision, upstream: data.upstreamRevision, dirty: false});
  assert.deepEqual(manifest.artifacts, {
    js: {path: data.js, sha256: sha256(readFileSync(data.js)), bytes: 11},
    wasm: {path: data.wasm, sha256: sha256(readFileSync(data.wasm)), bytes: 8},
  });
  assert.deepEqual(manifest.buildSources, {
    browserCmakeSha256: '1'.repeat(64), manifestScriptSha256: sha256(readFileSync(generator)),
  });
  assert(!Object.hasOwn(manifest, 'runtime'));
  assert(!Object.hasOwn(manifest, 'sources'));
});

test('Wasm post-link manifest retains the configured guest RAM and initial Wasm memory sizes', t => {
  const data = fixture(t);
  const result = generate([...data.args, '-DGUEST_RAM_BYTES=134217728', '-DINITIAL_MEMORY_BYTES=268435456']);
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(readManifest(data.manifest).configuration, {
    regionDiagnostics: false, focusTestExceptions: false, examples: true,
    guestRamBytes: 134217728, initialMemoryBytes: 268435456,
  });
});

test('Wasm post-link manifest records configure and build EMCC_CFLAGS independently', t => {
  const data = fixture(t);
  let result = generate(data.args, {EMCC_CFLAGS: '-DDYNARMIC_WASM_PROFILE=1'});
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(readManifest(data.manifest).buildEnvironment,
    {configureEmccCflags: '', buildEmccCflags: '-DDYNARMIC_WASM_PROFILE=1'});
  const args = data.args.map(arg => arg === '-DCONFIGURE_EMCC_CFLAGS='
    ? '-DCONFIGURE_EMCC_CFLAGS=-DDYNARMIC_WASM_REGION_DIAGNOSTICS=1' : arg);
  result = generate(args);
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(readManifest(data.manifest).buildEnvironment,
    {configureEmccCflags: '-DDYNARMIC_WASM_REGION_DIAGNOSTICS=1', buildEmccCflags: ''});
});

test('Wasm post-link manifest observes dirty source at generation time', t => {
  const data = fixture(t);
  writeFileSync(join(data.source, 'tracked.txt'), 'changed\n');
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(readManifest(data.manifest).source.dirty, true);
});

test('Wasm source cleanliness cannot hide modified submodules through Git configuration', t => {
  const data = fixture(t);
  git(data.source, ['config', 'diff.ignoreSubmodules', 'all']);
  writeFileSync(join(data.source, 'externals/dynarmic/tracked.txt'), 'changed\n');
  assert.equal(sourceInfo(data.source).dirty, true);
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(readManifest(data.manifest).source.dirty, true);
});

test('Wasm source cleanliness cannot hide untracked files through Git configuration', t => {
  const data = fixture(t);
  git(data.source, ['config', 'status.showUntrackedFiles', 'no']);
  writeFileSync(join(data.source, 'untracked.txt'), 'changed\n');
  assert.equal(sourceInfo(data.source).dirty, true);
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(readManifest(data.manifest).source.dirty, true);
});

test('Wasm post-link manifest fails closed for missing inputs and artifacts', t => {
  const data = fixture(t);
  assert.notEqual(generate(data.args.filter(arg => !arg.startsWith('-DA64_REGION_FOCUS='))).status, 0);
  assert.notEqual(generate(data.args.filter(arg => !arg.startsWith('-DBROWSER_CMAKE_SHA256='))).status, 0);
  rmSync(data.wasm);
  assert.notEqual(generate(data.args).status, 0);
});

test('source archives build with explicitly unknown Git provenance', t => {
  const data = fixture(t);
  rmSync(join(data.source, '.git'), {recursive: true});
  rmSync(join(data.source, 'externals/dynarmic/.git'), {recursive: true});
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  const manifest = readManifest(data.manifest);
  assert.deepEqual(manifest.source, {revision: null, upstream: null, dirty: null});
  assert.equal(manifest.artifacts.wasm.sha256, sha256(readFileSync(data.wasm)));
});

test('a source archive inside another repository never reports the parent revision', t => {
  const data = fixture(t);
  rmSync(join(data.source, '.git'), {recursive: true});
  rmSync(join(data.source, 'externals/dynarmic/.git'), {recursive: true});
  repository(data.root);
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(readManifest(data.manifest).source, {revision: null, upstream: null, dirty: null});
});

test('an unpacked upstream never inherits the containing project revision', t => {
  const data = fixture(t);
  rmSync(join(data.source, 'externals/dynarmic/.git'), {recursive: true});
  const result = generate(data.args);
  assert.equal(result.status, 0, result.stderr);
  const {source} = readManifest(data.manifest);
  assert.equal(source.revision, data.revision);
  assert.equal(source.upstream, null);
});

test('Git is optional for generating a usable build manifest', t => {
  const data = fixture(t);
  const result = generate(['-DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE', ...data.args]);
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(readManifest(data.manifest).source, {revision: null, upstream: null, dirty: null});
});

test('broken checkout metadata fails instead of claiming unknown or clean provenance', t => {
  const data = fixture(t);
  rmSync(join(data.source, '.git'), {recursive: true});
  writeFileSync(join(data.source, '.git'), 'gitdir: missing-git-directory\n');
  assert.notEqual(generate(data.args).status, 0);
});

test('configuration strings round-trip JSON escapes', t => {
  const data = fixture(t);
  const flags = '-DVALUE="quoted"\t-Wall\n-Wextra\\path';
  const result = generate(data.args.map(arg => arg === '-DCXX_FLAGS=' ? `-DCXX_FLAGS=${flags}` : arg));
  assert.equal(result.status, 0, result.stderr);
  assert.equal(readManifest(data.manifest).build.cxxFlags, flags);
});
