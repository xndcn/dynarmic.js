import assert from 'node:assert/strict';
import {mkdtemp, mkdir, rm, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve} from 'node:path';
import {test} from 'node:test';
import {preview} from 'vite';

test('Vite serves repository paths and a real 404 without rewriting the artifact', async () => {
  const dist = await mkdtemp(resolve(tmpdir(), 'dynarmic-pages-preview-'));
  const oldDist = process.env.DYNARMIC_JS_PAGES_DIST;
  let server;
  try {
    await mkdir(resolve(dist, 'assets'));
    await writeFile(resolve(dist, 'index.html'), '<!doctype html><title>Home</title>');
    await writeFile(resolve(dist, '404.html'), '<!doctype html><title>Not found</title><a href="/dynarmic.js/">Home</a>');
    await writeFile(resolve(dist, 'styles.css'), 'body { color: red; }');
    const wasm = Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]);
    await writeFile(resolve(dist, 'runtime.wasm'), wasm);
    process.env.DYNARMIC_JS_PAGES_DIST = dist;
    server = await preview({
      configFile: resolve(import.meta.dirname, '../../vite.config.mjs'),
      logLevel: 'silent', preview: {port: 0},
    });
    const base = `http://127.0.0.1:${server.httpServer.address().port}`;
    const home = await fetch(`${base}/dynarmic.js/`);
    assert.equal(home.status, 200);
    assert.equal(await home.text(), '<!doctype html><title>Home</title>');
    const styles = await fetch(`${base}/dynarmic.js/styles.css`);
    assert.equal(styles.status, 200);
    assert.match(styles.headers.get('content-type'), /text\/css/);
    const runtime = await fetch(`${base}/dynarmic.js/runtime.wasm`);
    assert.equal(runtime.status, 200);
    assert.match(runtime.headers.get('content-type'), /application\/wasm/);
    assert.deepEqual(Buffer.from(await runtime.arrayBuffer()), wasm);
    for (const path of ['/dynarmic.js/unknown/nested/path', '/dynarmic.js/missing.js']) {
      const missing = await fetch(base + path);
      assert.equal(missing.status, 404);
      assert.match(await missing.text(), /<title>Not found<\/title>/);
    }
    const head = await fetch(`${base}/dynarmic.js/missing`, {method: 'HEAD'});
    assert.equal(head.status, 404);
    assert.equal(await head.text(), '');
  } finally {
    if (oldDist === undefined) delete process.env.DYNARMIC_JS_PAGES_DIST;
    else process.env.DYNARMIC_JS_PAGES_DIST = oldDist;
    await server?.close();
    await rm(dist, {recursive: true, force: true});
  }
});

test('preview rejects an unbuilt directory with an actionable command', async () => {
  const dist = await mkdtemp(resolve(tmpdir(), 'dynarmic-pages-missing-'));
  const oldDist = process.env.DYNARMIC_JS_PAGES_DIST;
  try {
    process.env.DYNARMIC_JS_PAGES_DIST = dist;
    await assert.rejects(preview({
      configFile: resolve(import.meta.dirname, '../../vite.config.mjs'),
      logLevel: 'silent', preview: {port: 0},
    }), /Missing index.html.*Run npm run build/);
  } finally {
    if (oldDist === undefined) delete process.env.DYNARMIC_JS_PAGES_DIST;
    else process.env.DYNARMIC_JS_PAGES_DIST = oldDist;
    await rm(dist, {recursive: true, force: true});
  }
});
