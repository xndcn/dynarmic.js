#!/usr/bin/env node
// Run the installed npm artifact in a real Firefox module Worker. The browser
// reports over same-origin HTTP; no browser automation dependency is required.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash, randomBytes} from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import {install} from './fixture.mjs';

const REPORT_TIMEOUT_MS = 120000;
const SHUTDOWN_GRACE_MS = 2000;
const MAX_REPORT_BYTES = 128 * 1024;
const mimeTypes = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.json': 'application/json',
};

async function closesWithin(closed, milliseconds) {
  let timer;
  try {
    return await Promise.race([
      closed.then(() => true),
      new Promise(resolve => { timer = setTimeout(() => resolve(false), milliseconds); }),
    ]);
  } finally { clearTimeout(timer); }
}

async function stopBrowser(child, lifecycle) {
  if (lifecycle.isClosed()) return;
  child.kill('SIGTERM');
  if (await closesWithin(lifecycle.closed, SHUTDOWN_GRACE_MS)) return;
  child.kill('SIGKILL');
  if (!await closesWithin(lifecycle.closed, SHUTDOWN_GRACE_MS))
    throw new Error('Firefox did not close after SIGKILL; its profile is still in use');
}

async function verify(tarball, browser) {
  const tarballSha256 = createHash('sha256').update(fs.readFileSync(tarball)).digest('hex');
  const fixture = install(tarball);
  // Snap Firefox may be unable to access /tmp profiles. Keep the profile in
  // the working directory and delete it only after process/stdio closure.
  let profile, server, child, lifecycle, timeoutHandle;
  let report, failure, browserLog = '';
  const cleanupErrors = [];
  const requests = new Set();
  let phase = 'default';
  const overridePhases = ['wasmURL', 'wasmBinary', 'invalidWasmBinary'];
  const phaseRequests = {default: [], wasmURL: [], wasmBinary: [], invalidWasmBinary: []};
  const token = randomBytes(16).toString('hex');
  const base = `/nested/npm-consumer/${token}/`;
  const reportPath = `${base}__report`;
  let resolveResult;
  const resultPromise = new Promise(resolve => { resolveResult = resolve; });
  const onInterrupt = () => resolveResult({type: 'interrupted', signal: 'SIGINT'});
  const onTerminate = () => resolveResult({type: 'interrupted', signal: 'SIGTERM'});
  process.once('SIGINT', onInterrupt);
  process.once('SIGTERM', onTerminate);

  try {
    profile = fs.mkdtempSync(path.join(process.cwd(), '.firefox-npm-profile-'));
    fs.writeFileSync(path.join(fixture.root, 'index.html'), `<!doctype html>
<meta charset="utf-8"><title>Installed npm package Firefox consumer</title>
<script type="module">
let reported = false;
const worker = new Worker('./consumer-worker.mjs', {type: 'module'});
async function finish(report) {
  if (reported) return;
  reported = true;
  worker.terminate();
  document.body.textContent = report.status;
  await fetch('./__report', {method: 'POST', headers: {'content-type': 'application/json'},
    body: JSON.stringify(report)});
}
worker.onmessage = ({data}) => finish(data);
worker.onerror = event => finish({status: 'fail', message: event.message || 'Worker failed to load'});
worker.onmessageerror = () => finish({status: 'fail', message: 'Worker message could not be decoded'});
window.addEventListener('error', event => finish({status: 'fail', message: event.message}));
window.addEventListener('unhandledrejection', event => {
  if (!reported) finish({status: 'fail', message: String(event.reason)});
});
</script><body>Running installed package checks</body>`);
    fs.writeFileSync(path.join(fixture.root, 'consumer-worker.mjs'), `
import {createEmulator} from './node_modules/dynarmic.js/index.js';
import {exercise} from './cases.mjs';
try {
  const checks = await exercise(createEmulator);
  const wasmURL = new URL('./relocated/runtime.wasm', import.meta.url);
  const response = await fetch(wasmURL);
  if (!response.ok) throw new Error('Wasm byte fetch failed: HTTP ' + response.status);
  const wasmBinary = new Uint8Array(await response.arrayBuffer());
  async function enterPhase(stage) {
    const changed = await fetch('./__phase?stage=' + stage, {method: 'POST'});
    if (!changed.ok) throw new Error('Could not activate network check for ' + stage);
  }
  const assetOverrides = [];
  for (const [name, options] of [['wasmURL', {wasmURL}], ['wasmBinary', {wasmBinary}]]) {
    await enterPhase(name);
    const cpu = await createEmulator({...options, architecture: 'a32', memorySize: 65536});
    try {
      // mov r0,#40; add r0,r0,#2; svc #7
      cpu.writeMemory(0x1000, Uint8Array.of(0x28,0x00,0xa0,0xe3,0x02,0x00,0x80,0xe2,0x07,0x00,0x00,0xef));
      cpu.setPC(0x1000);
      const result = cpu.run({maxTicks: 100});
      if (result.reason !== 'svc' || result.svc !== 7 || cpu.getRegister(0) !== 42)
        throw new Error(name + ': guest did not return the expected SVC and value');
      assetOverrides.push(name);
      checks.push(name + ': actual ARM guest with relocated runtime asset');
    } finally { cpu.dispose(); }
  }
  await enterPhase('invalidWasmBinary');
  let invalidBytesRejected = false;
  try {
    const unexpected = await createEmulator({memorySize: 65536, wasmBinary: Uint8Array.of(0, 1, 2, 3)});
    unexpected.dispose();
  } catch { invalidBytesRejected = true; }
  if (!invalidBytesRejected) throw new Error('Invalid supplied Wasm bytes were silently ignored');
  checks.push('invalid supplied Wasm bytes reject without fetching a fallback');
  postMessage({status: 'pass', environment: navigator.userAgent, checks, assetOverrides, invalidBytesRejected});
} catch (error) {
  postMessage({status: 'fail', message: error.stack || String(error)});
}
`);
    fs.mkdirSync(path.join(fixture.root, 'relocated'));
    fs.copyFileSync(path.join(fixture.root, 'node_modules/dynarmic.js/runtime.wasm'),
      path.join(fixture.root, 'relocated/runtime.wasm'));

    server = http.createServer((request, response) => {
      let requestUrl, pathname;
      try {
        requestUrl = new URL(request.url, 'http://localhost');
        pathname = decodeURIComponent(requestUrl.pathname);
        if (pathname.includes('\0')) throw new Error('invalid pathname');
      } catch { response.writeHead(400).end(); return; }
      if (pathname === `${base}__phase`) {
        const next = requestUrl.searchParams.get('stage');
        const expected = overridePhases[overridePhases.indexOf(phase) + 1];
        if (request.method !== 'POST' || next !== expected) {
          response.writeHead(400).end();
          resolveResult({type: 'error', message: `invalid consumer phase transition: ${phase} -> ${next}`});
          return;
        }
        phase = next;
        response.writeHead(204).end();
        return;
      }
      if (pathname === reportPath) {
        if (request.method !== 'POST' || request.headers['content-type'] !== 'application/json') {
          response.writeHead(400).end();
          resolveResult({type: 'error', message: 'invalid browser report request'});
          return;
        }
        const chunks = [];
        let length = 0;
        request.on('data', chunk => {
          if (length > MAX_REPORT_BYTES) return;
          length += chunk.length;
          if (length > MAX_REPORT_BYTES) {
            response.writeHead(413).end();
            request.destroy();
            resolveResult({type: 'error', message: 'browser report exceeded size limit'});
          } else chunks.push(chunk);
        });
        request.on('error', error => resolveResult({type: 'error', message: `browser report failed: ${error.message}`}));
        request.on('end', () => {
          if (length > MAX_REPORT_BYTES) return;
          try {
            const data = JSON.parse(Buffer.concat(chunks).toString('utf8'));
            response.writeHead(204).end();
            resolveResult({type: 'report', data});
          } catch {
            response.writeHead(400).end();
            resolveResult({type: 'error', message: 'browser returned malformed JSON'});
          }
        });
        return;
      }
      if (request.method !== 'GET' || !pathname.startsWith(base)) {
        response.writeHead(404).end(); return;
      }
      const relative = pathname.slice(base.length) || 'index.html';
      phaseRequests[phase].push(relative);
      if (relative.endsWith('.wasm') && phase !== 'default' &&
        !(phase === 'wasmURL' && relative === 'relocated/runtime.wasm')) {
        response.writeHead(404).end();
        resolveResult({type: 'error', message: `unexpected Wasm fetch in ${phase} phase: ${relative}`});
        return;
      }
      const filename = path.resolve(fixture.root, relative);
      if (!filename.startsWith(fixture.root + path.sep)) {
        response.writeHead(403).end(); return;
      }
      fs.readFile(filename, (error, body) => {
        if (error) { response.writeHead(error.code === 'ENOENT' ? 404 : 500).end(); return; }
        requests.add(relative);
        response.writeHead(200, {
          'content-type': mimeTypes[path.extname(filename)] || 'application/octet-stream',
          'cache-control': 'no-store',
          'x-content-type-options': 'nosniff',
        }).end(body);
      });
    });
    server.on('error', error => resolveResult({type: 'error', message: `HTTP server failed: ${error.message}`}));
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', () => { server.removeListener('error', reject); resolve(); });
    });
    const url = `http://127.0.0.1:${server.address().port}${base}index.html`;
    child = spawn(browser, ['--headless', '--no-remote', '--profile', profile, url],
      {stdio: ['ignore', 'pipe', 'pipe']});
    let hasClosed = false;
    const closed = new Promise(resolve => child.once('close', (code, signal) => {
      hasClosed = true;
      resolve({type: 'exit', code, signal});
    }));
    const failed = new Promise(resolve => child.once('error', error =>
      resolve({type: 'error', message: `could not start Firefox: ${error.message}`})));
    lifecycle = {closed, isClosed: () => hasClosed};
    const capture = chunk => { browserLog = (browserLog + chunk.toString()).slice(-65536); };
    child.stdout.on('data', capture);
    child.stderr.on('data', capture);
    const timeout = new Promise(resolve => {
      timeoutHandle = setTimeout(() => resolve({type: 'timeout'}), REPORT_TIMEOUT_MS);
    });
    const result = await Promise.race([resultPromise, closed, failed, timeout]);
    if (result.type === 'error') throw new Error(result.message);
    if (result.type === 'exit')
      throw new Error(`Firefox exited before reporting (code=${result.code}, signal=${result.signal})`);
    if (result.type === 'timeout') throw new Error('Firefox npm consumer timed out waiting for a Worker report');
    if (result.type === 'interrupted') throw new Error(`Firefox npm consumer interrupted by ${result.signal}`);
    assert.equal(result.type, 'report', 'missing browser report');
    report = result.data;
    assert(report && typeof report === 'object' && !Array.isArray(report), 'browser report must be an object');
    assert.equal(report.status, 'pass', report.message || 'browser consumer did not pass');
    assert.equal(typeof report.environment, 'string', 'missing browser environment');
    assert.match(report.environment, /Firefox\//, 'report did not originate from Firefox');
    assert(Array.isArray(report.checks) && report.checks.length >= 16 &&
      report.checks.every(check => typeof check === 'string' && check.length > 0), 'incomplete consumer checks');
    assert.deepEqual(report.assetOverrides, ['wasmURL', 'wasmBinary'], 'incomplete Wasm asset override checks');
    assert.equal(report.invalidBytesRejected, true, 'invalid supplied Wasm bytes did not reject');
    assert.equal(phase, 'invalidWasmBinary', 'incomplete asset network checks');
    assert(phaseRequests.wasmURL.includes('relocated/runtime.wasm'), 'wasmURL override did not fetch its configured asset');
    assert(!phaseRequests.wasmBinary.some(name => name.endsWith('.wasm')), 'supplied Wasm bytes fetched an asset');
    assert(!phaseRequests.invalidWasmBinary.some(name => name.endsWith('.wasm')), 'invalid Wasm bytes fetched a fallback');
    for (const required of ['consumer-worker.mjs', 'cases.mjs', 'node_modules/dynarmic.js/index.js',
      'node_modules/dynarmic.js/runtime.wasm', 'relocated/runtime.wasm'])
      assert(requests.has(required), `expected installed-package asset was not served: ${required}`);
    assert.equal(createHash('sha256').update(fs.readFileSync(tarball)).digest('hex'), tarballSha256,
      'package tarball changed during the consumer check');
    report = {...report, browser: path.resolve(browser), nestedPath: base,
      tarball: path.resolve(tarball),
      tarballSha256,
      requests: [...requests].sort(), phaseRequests};
  } catch (error) { failure = error; }
  finally {
    clearTimeout(timeoutHandle);
    try { if (child && lifecycle) await stopBrowser(child, lifecycle); }
    catch (error) { cleanupErrors.push(error.message); }
    if (server) {
      await new Promise(resolve => {
        server.close(resolve);
        server.closeAllConnections();
      });
    }
    try {
      if (profile && (!lifecycle || lifecycle.isClosed()))
        fs.rmSync(profile, {recursive: true, force: true, maxRetries: 3});
    } catch (error) { cleanupErrors.push(error.message); }
    try { fixture.cleanup(); } catch (error) { cleanupErrors.push(error.message); }
    process.removeListener('SIGINT', onInterrupt);
    process.removeListener('SIGTERM', onTerminate);
  }
  if (failure || cleanupErrors.length) {
    const message = [failure?.message, ...cleanupErrors].filter(Boolean).join('\n');
    throw new Error(message + (browserLog ? `\nFirefox output:\n${browserLog}` : ''));
  }
  return report;
}

const [tarball, browser, ...extra] = process.argv.slice(2);
const validArguments = tarball && browser &&
  (!extra.length || (extra.length === 2 && extra[0] === '--report' && extra[1]));
let finalReport;
try {
  if (!validArguments)
    throw new Error('Usage: node tests/npm/verify_firefox.mjs package.tgz firefox-executable [--report result.json]');
  finalReport = await verify(path.resolve(tarball), browser);
} catch (error) {
  finalReport = {status: 'fail', message: error.stack || String(error)};
  process.exitCode = 1;
}
if (validArguments && extra.length) {
  try { fs.writeFileSync(extra[1], JSON.stringify(finalReport, null, 2) + '\n'); }
  catch (error) {
    finalReport = {status: 'fail', message: `could not write report: ${error.message}`};
    process.exitCode = 1;
  }
}
console[finalReport.status === 'pass' ? 'log' : 'error'](JSON.stringify(finalReport, null, 2));
