#!/usr/bin/env node
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {withChromium} from '../browser/chromium.mjs';
import {install} from './fixture.mjs';
const [tarball, browser, ...extra] = process.argv.slice(2);
if (!tarball || !browser || (extra.length && !(extra.length === 2 && extra[0] === '--report')))
  throw new Error('Usage: node tests/npm/verify_browser.mjs package.tgz chrome-executable [--report result.json]');
const fixture = install(tarball);
const requests = new Set();
let phase = 'default';
const forbiddenRequests = [];
try {
  fs.writeFileSync(path.join(fixture.root, 'index.html'), `<!doctype html><title>Installed package consumer</title>
<script type="module">
const worker = new Worker('./worker.js', {type:'module'});
worker.onmessage = ({data}) => { window.report = data; worker.terminate(); };
worker.onerror = e => { window.report = {status:'fail',message:e.message}; worker.terminate(); };
</script>`);
  fs.writeFileSync(path.join(fixture.root, 'worker.js'), `
import {createEmulator} from './node_modules/dynarmic.js/index.js';
import {exercise} from './cases.mjs';
try {
  const checks = await exercise(createEmulator);
  const wasmURL = new URL('./relocated/runtime.wasm', import.meta.url);
  const wasmBinary = new Uint8Array(await (await fetch(wasmURL)).arrayBuffer());
  for (const options of [{wasmURL}, {wasmBinary}]) {
    await fetch('./phase/' + (options.wasmURL ? 'url' : 'bytes'));
    const cpu = await createEmulator({...options, memorySize:65536});
    try { cpu.setRegister(0, 42); if (cpu.getRegister(0) !== 42) throw new Error('asset override failed'); }
    finally { cpu.dispose(); }
  }
  await fetch('./phase/invalid-bytes');
  let rejected = false;
  try {const cpu = await createEmulator({wasmBinary:Uint8Array.of(0,1,2)});cpu.dispose();}
  catch {rejected = true;}
  if (!rejected) throw new Error('invalid Wasm bytes were silently ignored');
  checks.push('relocated Wasm URL and supplied bytes without asset fallback in a module Worker');
  checks.push('invalid Wasm bytes reject in a module Worker');
  postMessage({status:'pass',environment:navigator.userAgent,checks});
} catch (e) {postMessage({status:'fail',message:e.stack || String(e)});}
`);
  fs.mkdirSync(path.join(fixture.root, 'relocated'));
  fs.copyFileSync(path.join(fixture.root, 'node_modules/dynarmic.js/runtime.wasm'), path.join(fixture.root, 'relocated/runtime.wasm'));
  await withChromium({browser, root: fixture.root, serve: ({name, response}) => {
    requests.add(name);
    if (name.startsWith('phase/')) {phase = name.slice(6);response.writeHead(204).end();return true;}
    if ((phase !== 'default' && name === 'node_modules/dynarmic.js/runtime.wasm') ||
        (['bytes', 'invalid-bytes'].includes(phase) && name.endsWith('.wasm'))) {
      forbiddenRequests.push({phase, name});response.writeHead(404).end();return true;
    }
    if (name.endsWith('.mjs')) {
      response.writeHead(200, {'content-type': 'text/javascript'}).end(fs.readFileSync(path.join(fixture.root, name)));
      return true;
    }
    return false;
  }}, async ({navigate, evaluate, until}) => {
    await navigate();
    const report = await until(() => evaluate('window.report'), 'installed package Worker checks', 90000);
    assert.equal(report.status, 'pass', report.message);
    assert(requests.has('node_modules/dynarmic.js/runtime.wasm'), 'default asset was not fetched');
    assert(requests.has('relocated/runtime.wasm'), 'custom asset was not fetched');
    assert.deepEqual(forbiddenRequests, [], 'overrides must not fall back to fetching runtime assets');
    report.requests = [...requests];
    report.tarballSha256 = fixture.tarballSha256;
    if (extra.length) fs.writeFileSync(extra[1], JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report, null, 2));
  });
} finally { fixture.cleanup(); }
