#!/usr/bin/env node
// Run tools from the pinned test installation against an isolated tarball consumer.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {execFileSync} from 'node:child_process';
import {build} from 'vite';
import {withChromium} from '../browser/chromium.mjs';
import {install} from './fixture.mjs';

const [tarball, browser, ...extra] = process.argv.slice(2);
if (!tarball || !browser || (extra.length && !(extra.length === 2 && extra[0] === '--report')))
  throw new Error('Usage: node tests/npm/verify_tooling.mjs package.tgz chrome-executable [--report result.json]');
const fixture = install(tarball);
const bin = name => fileURLToPath(new URL(`node_modules/.bin/${name}`, import.meta.url));
const checks = [];
try {
  const pkg = path.join(fixture.root, 'node_modules/dynarmic.js');
  execFileSync(bin('publint'), [pkg, '--strict'], {stdio: 'inherit'});
  // runtime.wasm is a binary asset, not a JS/TS module. Its export is exercised
  // below by Node resolution and by a real Vite URL import and browser fetch.
  execFileSync(bin('attw'), [path.resolve(tarball), '--profile', 'esm-only',
    '--exclude-entrypoints', './runtime.wasm'], {stdio: 'inherit'});
  checks.push('publint strict and Are the Types Wrong ESM profile');
  fs.copyFileSync(new URL('./types.mts', import.meta.url), path.join(fixture.root, 'types.mts'));
  for (const [module, moduleResolution] of [['NodeNext', 'NodeNext'], ['ESNext', 'Bundler']]) {
    execFileSync(bin('tsc'), ['--noEmit', '--strict', '--target', 'ES2022', '--module', module,
      '--moduleResolution', moduleResolution, 'types.mts'], {cwd: fixture.root, stdio: 'inherit'});
    checks.push(`TypeScript strict ${moduleResolution} including invalid API uses`);
  }
  const commonJS = `const assert = require('node:assert/strict');
import('dynarmic.js').then(async ({createEmulator}) => {
  const cpu = await createEmulator({memorySize:65536});
  try {cpu.setRegister(0,42); assert.equal(cpu.getRegister(0),42);}
  finally {cpu.dispose();}
}).catch(error => {console.error(error); process.exitCode=1;});`;
  fs.writeFileSync(path.join(fixture.root, 'consumer.cjs'), commonJS);
  execFileSync(process.execPath, ['consumer.cjs'], {cwd: fixture.root, stdio: 'inherit', timeout: 30000});
  checks.push('CommonJS dynamic import');
  const readmeExample = fs.readFileSync(path.join(pkg, 'README.md'), 'utf8').match(/```js\n([\s\S]*?)\n```/);
  assert(readmeExample, 'the package README must contain a runnable Node example');
  fs.writeFileSync(path.join(fixture.root, 'hello.mjs'), readmeExample[1]);
  const hello = execFileSync(process.execPath, ['hello.mjs'], {cwd: fixture.root, encoding: 'utf8', timeout: 30000});
  assert.equal(hello.trim(), 'svc 7 42');
  checks.push('Published README Node quick start executes unchanged');
  fs.writeFileSync(path.join(fixture.root, 'index.html'), `<!doctype html><title>Vite consumer</title>
<script type="module" src="./main.js"></script>`);
  fs.writeFileSync(path.join(fixture.root, 'main.js'), `
const worker = new Worker(new URL('./worker.js', import.meta.url), {type:'module'});
worker.onmessage = ({data}) => {window.report=data;worker.terminate();};
worker.onerror = e => {window.report={status:'fail',message:e.message};worker.terminate();};`);
  fs.writeFileSync(path.join(fixture.root, 'worker.js'), `
import {createEmulator} from 'dynarmic.js';
import wasmURL from 'dynarmic.js/runtime.wasm?url';
import {exercise} from './cases.mjs';
try {postMessage({status:'pass',checks:await exercise(createEmulator, {wasmURL:new URL(wasmURL, self.location.href)})});}
catch(e) {postMessage({status:'fail',message:e.stack || String(e)});}`);
  await build({root: fixture.root, configFile: false, base: '/nested/quickjs/',
    worker: {format: 'es'}, build: {target: 'es2022', assetsInlineLimit: 0}, logLevel: 'warn'});
  const requests = [];
  let workerReport;
  await withChromium({browser, root: path.join(fixture.root, 'dist'), serve: ({name}) => {
    requests.push(name); return false;
  }}, async ({navigate, evaluate, until}) => {
    await navigate();
    workerReport = await until(() => evaluate('window.report'), 'Vite production Worker', 90000);
    assert.equal(workerReport.status, 'pass', workerReport.message);
    assert(requests.some(name => /^assets\/.*\.wasm$/.test(name)), 'Vite must emit and load the Wasm asset');
  });
  checks.push('Vite production module Worker under a nested base with hashed Wasm asset');
  const report = {status: 'pass', environment: process.version, tarballSha256: fixture.tarballSha256,
    checks, workerChecks: workerReport.checks, requests};
  if (extra.length) fs.writeFileSync(extra[1], JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report, null, 2));
} finally { fixture.cleanup(); }
