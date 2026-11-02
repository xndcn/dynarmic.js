import test from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {mkdtemp, writeFile, readFile, rm, mkdir, symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {check, parseArgs} from '../../scripts/pages.mjs';

const ROOT = resolve(import.meta.dirname, '../..');
const CLI = join(ROOT, 'scripts/pages.mjs');
const identity = value => ({sha256: createHash('sha256').update(value).digest('hex'), bytes: Buffer.byteLength(value)});

async function fixture(t) {
  const root = await mkdtemp(join(tmpdir(), 'dynarmic-pages-check-'));
  t.after(() => rm(root, {recursive: true, force: true}));
  const buildDir = join(root, 'build with spaces');
  const dist = join(buildDir, 'examples/browser/dist');
  await mkdir(dist, {recursive: true});
  const files = {
    'styles.css': 'body { color: #123; }\n', 'site.js': 'console.log("ready");\n',
    'robots.txt': 'User-agent: *\n', 'sitemap.xml': '<urlset/>',
    'icon.svg': '<svg/>', 'icon-192.png': 'icon one', 'icon-512.png': 'icon two',
    'social-card.png': 'social card', 'LICENSE.txt': 'MIT', 'THIRD_PARTY_NOTICES.md': 'Notices',
    'source.tar.gz': 'matching recursive source', 'runtime-licenses.txt': 'runtime notices',
    'arm_jit.wasm': 'runtime bytes', 'arm_bench_jit.wasm': 'official runtime bytes',
    'quickjs-wasm.wasm': 'quickjs runtime bytes', 'c-bench.wasm': 'C runtime bytes',
  };
  const source = {
    schema: 'dynarmic.js/pages-source-v1',
    archive: {path: 'source.tar.gz', ...identity(files['source.tar.gz'])},
    runtimeNotices: {path: 'runtime-licenses.txt', ...identity(files['runtime-licenses.txt'])},
  };
  for (const [section, name] of [
    ['runtimeBuild', 'arm_jit.wasm'], ['officialHostBuild', 'arm_bench_jit.wasm'],
    ['quickjsBuild', 'quickjs-wasm.wasm'], ['cBenchBuild', 'c-bench.wasm'],
  ]) source[section] = {artifacts: {[name]: identity(files[name])}};
  const manifest = {start_url: './', scope: './', icons: [{src: 'icon-192.png'}, {src: './icon-512.png'}]};
  files['source-manifest.json'] = JSON.stringify(source);
  files['site.webmanifest'] = JSON.stringify(manifest);
  files['index.html'] = `<!doctype html><html lang=en><head>
<link rel=canonical href="https://example.com/dynarmic.js/">
<link rel=stylesheet href='styles.css?v=${identity(files['styles.css']).sha256}'>
<script defer src=site.js?v=${identity(files['site.js']).sha256}></script>
<link rel=manifest href=site.webmanifest></head><body>
<a href="source.tar.gz">Download source</a><img src=icon.svg>
<pre><code>&lt;script src="unshipped-example.js"&gt;&lt;/script&gt;</code></pre>
<!-- <img src="unshipped-comment.png"> -->
<script>const sample = '<img src="unshipped-string.png">';</script>
<a href="https://other.example/docs">Docs</a></body></html>`;
  // Pages uses absolute repository paths in a 404 reached from nested URLs.
  files['404.html'] = '<!doctype html><html lang=en><link rel=stylesheet href=/dynarmic.js/styles.css><a href=/dynarmic.js/>Home</a></html>';
  await Promise.all(Object.entries(files).map(([name, value]) => writeFile(join(dist, name), value)));
  return {root, buildDir, dist, source, manifest};
}

async function changeJSON(dist, name, change) {
  const value = JSON.parse(await readFile(join(dist, name), 'utf8'));
  change(value);
  await writeFile(join(dist, name), JSON.stringify(value));
}

const verify = f => check({dist: f.dist, strict: true});

test('valid repository artifact: real HTML references, escaped snippets and unversioned 404 assets', async t => {
  const f = await fixture(t);
  assert.deepEqual(await verify(f), {dist: f.dist, strict: true});
});

test('missing single-quoted and unquoted HTML resources are rejected, including 404 resources', async t => {
  const f = await fixture(t);
  await rm(join(f.dist, 'site.js'));
  await assert.rejects(verify(f), /missing Pages asset: site.js/);
  await writeFile(join(f.dist, 'site.js'), 'console.log("ready");\n');
  await writeFile(join(f.dist, '404.html'), '<!doctype html><a href=missing-guide.html>Guide</a>');
  await assert.rejects(verify(f), /missing Pages asset: missing-guide.html/);
  await writeFile(join(f.dist, '404.html'), '<!doctype html><pre><code><img src=real-missing.png></code></pre>');
  await assert.rejects(verify(f), /missing Pages asset: real-missing.png/);
});

test('unversioned, malformed and stale homepage cache keys fail', async t => {
  const f = await fixture(t);
  const original = await readFile(join(f.dist, 'index.html'), 'utf8');
  for (const suffix of ['', '?v=bad']) {
    await writeFile(join(f.dist, 'index.html'), original.replace(/site\.js\?v=[a-f0-9]+/, `site.js${suffix}`));
    await assert.rejects(verify(f), /missing or invalid content hash/);
  }
  await writeFile(join(f.dist, 'index.html'), original);
  await writeFile(join(f.dist, 'styles.css'), 'body { color: #456; }\n');
  await assert.rejects(verify(f), /stale content hash/);
});

test('manifest scope and every icon src must resolve to local files', async t => {
  const f = await fixture(t);
  await changeJSON(f.dist, 'site.webmanifest', m => { m.scope = '/'; });
  await assert.rejects(verify(f), /relative start_url and scope/);
  for (const src of ['absent.png', 'https://other.example/icon.png', '../icon.png']) {
    await writeFile(join(f.dist, 'site.webmanifest'), JSON.stringify({...f.manifest, icons: [{src}, {src: 'icon-512.png'}]}));
    await assert.rejects(verify(f), /missing Pages asset|must be local|unsafe asset/);
  }
});

test('source archive and notices must exist and retain their recorded content', async t => {
  const f = await fixture(t);
  await writeFile(join(f.dist, 'source.tar.gz'), 'tampered recursive source');
  await assert.rejects(verify(f), /artifact hash\/size mismatch: source.tar.gz/);
  await writeFile(join(f.dist, 'source.tar.gz'), 'matching recursive source');
  await rm(join(f.dist, 'runtime-licenses.txt'));
  await assert.rejects(verify(f), /missing Pages asset: runtime-licenses.txt/);
});

test('all four strict engine sections verify hashes and sizes, not just filenames', async t => {
  const f = await fixture(t);
  for (const [section, name] of [
    ['runtimeBuild', 'arm_jit.wasm'], ['officialHostBuild', 'arm_bench_jit.wasm'],
    ['quickjsBuild', 'quickjs-wasm.wasm'], ['cBenchBuild', 'c-bench.wasm'],
  ]) {
    const bytes = await readFile(join(f.dist, name));
    const modified = Buffer.from(bytes); modified[0] ^= 1;
    await writeFile(join(f.dist, name), modified);
    await assert.rejects(verify(f), new RegExp(`artifact hash/size mismatch: ${name}`));
    await writeFile(join(f.dist, name), bytes);
    await changeJSON(f.dist, 'source-manifest.json', m => { delete m[section]; });
    await assert.rejects(verify(f), new RegExp(`missing ${section}.artifacts`));
    await writeFile(join(f.dist, 'source-manifest.json'), JSON.stringify(f.source));
  }
  await changeJSON(f.dist, 'source-manifest.json', m => { m.runtimeBuild.artifacts['arm_jit.wasm'].bytes += 1; });
  await assert.rejects(verify(f), /artifact hash\/size mismatch/);
});

test('manifest traversal, encoded HTML traversal and symlink parents cannot escape the artifact', async t => {
  const f = await fixture(t);
  await changeJSON(f.dist, 'source-manifest.json', m => { m.runtimeBuild.artifacts = {'../outside': identity('outside')}; });
  await assert.rejects(verify(f), /unsafe asset path/);
  await writeFile(join(f.dist, 'source-manifest.json'), JSON.stringify(f.source));
  const original = await readFile(join(f.dist, 'index.html'), 'utf8');
  await writeFile(join(f.dist, 'index.html'), original.replace('src=icon.svg', 'src=%2e%2e/outside.svg'));
  await assert.rejects(verify(f), /unsafe asset URL/);
  await writeFile(join(f.dist, 'index.html'), original);
  const outside = join(f.root, 'outside');
  await mkdir(outside); await writeFile(join(outside, 'icon.svg'), '<svg/>');
  await symlink(outside, join(f.dist, 'linked'));
  await writeFile(join(f.dist, 'index.html'), original.replace('src=icon.svg', 'src=linked/icon.svg'));
  await assert.rejects(verify(f), /symlink in Pages asset path/);
});

test('CLI help succeeds and incomplete options cannot silently check the default directory', () => {
  const help = spawnSync(process.execPath, [CLI, '--help'], {encoding: 'utf8'});
  assert.equal(help.status, 0); assert.match(help.stdout, /Configure CMake/);
  for (const args of [['check', '--dist'], ['check', '--dist', '--strict'], ['build', '--build-dir']]) {
    const result = spawnSync(process.execPath, [CLI, ...args], {encoding: 'utf8'});
    assert.equal(result.status, 2, result.stderr); assert.match(result.stderr, /requires a path/);
  }
  const configured = parseArgs(['build'], {DYNARMIC_JS_BUILD_DIR: '/tmp/example build', DYNARMIC_JS_PAGES_DIST: '/tmp/ignored'});
  assert.equal(configured.dist, '/tmp/example build/examples/browser/dist');
});

test('build runs configured CMake, then source packaging, then the strict artifact check', async t => {
  const f = await fixture(t);
  const bin = join(f.root, 'bin'); await mkdir(bin);
  const log = join(f.root, 'commands.jsonl');
  for (const name of ['cmake', 'python3']) {
    await writeFile(join(bin, name), `#!${process.execPath}\nconst fs = require('node:fs');\nfs.appendFileSync(process.env.COMMAND_LOG, JSON.stringify({command:${JSON.stringify(name)}, args:process.argv.slice(2)})+'\\n');\nif (process.env.FAIL_COMMAND === ${JSON.stringify(name)}) process.exit(23);\n`, {mode: 0o755});
  }
  const env = {...process.env, PATH: `${bin}:${process.env.PATH}`, COMMAND_LOG: log, DYNARMIC_JS_BUILD_DIR: f.buildDir};
  const result = spawnSync(process.execPath, [CLI, 'build'], {encoding: 'utf8', env});
  assert.equal(result.status, 0, result.stderr); assert.match(result.stdout, /\(strict\)/);
  const commands = (await readFile(log, 'utf8')).trim().split('\n').map(JSON.parse);
  assert.deepEqual(commands, [
    {command: 'cmake', args: ['--build', f.buildDir, '--target', 'browser_page', '--parallel', '2']},
    {command: 'python3', args: [join(ROOT, 'examples/browser/package_source.py'), '--dist', f.dist]},
  ]);
  for (const name of ['cmake', 'python3']) {
    await writeFile(log, '');
    const failure = spawnSync(process.execPath, [CLI, 'build'], {encoding: 'utf8', env: {...env, FAIL_COMMAND: name}});
    assert.equal(failure.status, 23, failure.stderr);
    const attempts = (await readFile(log, 'utf8')).trim().split('\n').map(JSON.parse);
    assert.equal(attempts.length, name === 'cmake' ? 1 : 2);
  }
  await writeFile(join(f.dist, 'c-bench.wasm'), 'changed after packaging');
  const invalid = spawnSync(process.execPath, [CLI, 'build'], {encoding: 'utf8', env});
  assert.equal(invalid.status, 1); assert.match(invalid.stderr, /artifact hash\/size mismatch: c-bench.wasm/);
});
