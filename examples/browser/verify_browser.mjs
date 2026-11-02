#!/usr/bin/env node
// Firefox Worker smoke test. The page reports through a same-origin HTTP
// callback; process launch and profile flags below are Firefox-specific.
// Use verify_chrome.js with Chrome's DevTools endpoint for Chromium.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';

const REPORT_TIMEOUT_MS = 120000;
const SHUTDOWN_GRACE_MS = 1500;

// Instrument only the local test response, never the distributed page. The
// existing smoke callback happens after the app publishes its complete report.
function reportCollector(reportName) {
  const fetchOriginal = globalThis.fetch.bind(globalThis);
  globalThis.fetch = (input, options) => {
    const url = new URL(typeof input === 'string' ? input : input.url || input.href, location.href);
    if (url.origin === location.origin && url.pathname === '/__result') {
      return fetchOriginal(url, {...options, method: 'POST',
        headers: {'content-type': 'application/json'},
        body: JSON.stringify({status: url.searchParams.get('status'), userAgent: navigator.userAgent,
          report: reportName ? globalThis[reportName] ?? null : null})});
    }
    return fetchOriginal(input, options);
  };
}

function parseOptions(extra) {
  let mode = 'worker', reportPath;
  for (let i = 0; i < extra.length; i++) {
    const option = extra[i];
    if (['--quickjs', '--official', '--cbench'].includes(option) && mode === 'worker') mode = option.slice(2);
    else if (option === '--report' && !reportPath && extra[i + 1] && !extra[i + 1].startsWith('--')) reportPath = path.resolve(extra[++i]);
    else throw new Error('usage: verify_browser.mjs <firefox-executable> [dist-directory] [--quickjs|--official|--cbench] [--report output.json]');
  }
  return {mode, reportPath};
}

async function closesWithin(closed, milliseconds) {
  let timer;
  try {
    return await Promise.race([
      closed.then(() => true),
      new Promise(resolve => { timer = setTimeout(() => resolve(false), milliseconds); }),
    ]);
  } finally {
    clearTimeout(timer);
  }
}

async function stopBrowser(child, lifecycle) {
  if (lifecycle.isClosed()) return;
  child.kill('SIGTERM');
  if (await closesWithin(lifecycle.closed, SHUTDOWN_GRACE_MS)) return;
  child.kill('SIGKILL');
  if (!await closesWithin(lifecycle.closed, SHUTDOWN_GRACE_MS)) {
    throw new Error('Firefox did not close after SIGKILL; its profile is still in use');
  }
}

async function main() {
  const [browser, directory = 'build-em/examples/browser/dist', ...extra] = process.argv.slice(2);
  const {mode, reportPath} = parseOptions(extra);
  const quickjs = mode === 'quickjs', official = mode === 'official', cbench = mode === 'cbench';
  if (!browser) throw new Error('usage: verify_browser.mjs <firefox-executable> [dist-directory] [--quickjs|--official|--cbench] [--report output.json]');
  const root = path.resolve(directory);
  const observation = {schema: 'dynarmic.js/firefox-smoke-v1', mode, startedAt: new Date().toISOString(),
    browser, directory: root, outcome: 'fail', status: null, userAgent: null, report: null};
  const saveObservation = () => {
    if (!reportPath) return;
    observation.finishedAt = new Date().toISOString();
    fs.mkdirSync(path.dirname(reportPath), {recursive: true});
    fs.writeFileSync(reportPath, JSON.stringify(observation, null, 2) + '\n');
  };
  const requiredArtifacts = ['index.html', 'app.js', 'site.js', 'styles.css', 'worker.js', 'arm_jit.js', 'arm_jit.wasm',
    'demo.bin', 'demo_a64.bin', 'expected.txt'];
  if (quickjs) requiredArtifacts.push('quickjs-app.js', 'quickjs-worker.js', 'quickjs-workloads.js',
    'quickjs-arm.bin', 'quickjs-wasm.js', 'quickjs-wasm.wasm', 'quickjs-manifest.json', 'wasm-build-manifest.json');
  if (official) requiredArtifacts.push('official-benchmarks-app.js', 'official-benchmarks-worker.js',
    'official-benchmarks.json', 'official-v8.js', 'official-v8-adapter.js', 'quickjs-arm.bin',
    'quickjs-wasm.js', 'quickjs-wasm.wasm', 'quickjs-manifest.json',
    'arm_bench_jit.js', 'arm_bench_jit.wasm', 'arm-bench-build-manifest.json');
  if (cbench) requiredArtifacts.push('c-bench-app.js', 'c-bench-worker.js', 'c-bench-protocol.js',
    'c-bench-manifest.json', 'c-bench-reference.json', 'c-bench-source.c', 'c-bench-arm.bin',
    'c-bench-x86.elf', 'c-bench.wasm', 'c-bench-v86.js', 'c-bench-v86.wasm', 'c-bench-v86-LICENSE.txt', 'wasm-build-manifest.json');
  const missing = requiredArtifacts.filter(name => !fs.existsSync(path.join(root, name)));
  if (missing.length) {
    observation.error = `missing browser artifacts: ${missing.join(', ')}; build the browser_page target`;
    saveObservation();
    throw new Error(observation.error);
  }
  // Snap Firefox cannot access arbitrary /tmp profiles in some CI images.
  const profile = fs.mkdtempSync(path.join(process.cwd(), '.firefox-profile-'));
  let resolveResult;
  const resultPromise = new Promise(resolve => { resolveResult = resolve; });
  const reportName = official ? 'officialBenchReport' : quickjs ? 'quickjsReport' : cbench ? 'cBenchReport' : null;
  const instrumentHTML = body => {
    if (!reportPath) return body;
    const script = '<script src="/__firefox-report.js"></script>';
    const html = body.toString();
    return /<head\b[^>]*>/i.test(html) ? html.replace(/<head\b[^>]*>/i, match => match + script) : script + html;
  };
  const types = {'.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript', '.wasm': 'application/wasm',
    '.bin': 'application/octet-stream', '.elf': 'application/octet-stream', '.c': 'text/plain', '.txt': 'text/plain', '.svg': 'image/svg+xml',
    '.png': 'image/png', '.webmanifest': 'application/manifest+json', '.xml': 'application/xml', '.json': 'application/json'};
  const server = http.createServer((request, response) => {
    let requestUrl, relative;
    try {
      requestUrl = new URL(request.url, 'http://localhost');
      relative = decodeURIComponent(requestUrl.pathname === '/' ? '/index.html' : requestUrl.pathname);
    } catch {
      response.writeHead(400).end();
      return;
    }
    if (requestUrl.pathname === '/__result') {
      const status = requestUrl.searchParams.get('status');
      if (!reportPath) {
        response.writeHead(204).end();
        resolveResult({type: 'report', status});
        return;
      }
      if (request.method !== 'POST') {
        response.writeHead(400).end();
        resolveResult({type: 'report-error', error: new Error('Firefox report callback omitted its JSON evidence')});
        return;
      }
      const chunks = [];
      let bytes = 0;
      request.on('data', chunk => {
        bytes += chunk.length;
        if (bytes > 10 * 1024 * 1024) {
          response.writeHead(413).end();
          resolveResult({type: 'report-error', error: new Error('Firefox report exceeds 10 MiB')});
          request.destroy();
        } else chunks.push(chunk);
      });
      request.on('end', () => {
        try {
          const payload = JSON.parse(Buffer.concat(chunks).toString());
          if (!payload || payload.status !== status || typeof payload.userAgent !== 'string' ||
              (payload.report !== null && (typeof payload.report !== 'object' || Array.isArray(payload.report)))) {
            throw new Error('invalid Firefox report payload');
          }
          Object.assign(observation, {status, userAgent: payload.userAgent, report: payload.report});
          if (status?.startsWith('PASS|') && reportName && !payload.report) throw new Error('passing benchmark omitted its detailed report');
          response.writeHead(204).end();
          resolveResult({type: 'report', status});
        } catch (error) {
          response.writeHead(400).end();
          resolveResult({type: 'report-error', error});
        }
      });
      request.on('error', error => resolveResult({type: 'report-error', error}));
      return;
    }
    if (reportPath && requestUrl.pathname === '/__firefox-report.js') {
      response.writeHead(200, {'content-type': 'text/javascript'}).end(`(${reportCollector})(${JSON.stringify(reportName)});`);
      return;
    }
    if (relative === '/arm-smoke.html') {
      response.writeHead(200, {'content-type': 'text/html'}).end(instrumentHTML(fs.readFileSync(new URL('../../tests/browser/fixtures/arm.html', import.meta.url))));
      return;
    }
    const file = path.resolve(root, '.' + relative);
    if (!file.startsWith(root + path.sep)) {
      response.writeHead(403).end();
      return;
    }
    fs.readFile(file, (error, body) => {
      if (error) {
        response.writeHead(error.code === 'ENOENT' ? 404 : 500).end();
        return;
      }
      response.writeHead(200, {'content-type': types[path.extname(file)] || 'application/octet-stream'}).end(
        path.extname(file) === '.html' ? instrumentHTML(body) : body);
    });
  });
  let child, lifecycle, timeoutHandle;
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject);
      server.listen(0, '127.0.0.1', resolve);
    });
    const port = server.address().port;
    const url = new URL(`http://127.0.0.1:${port}/${quickjs || official || cbench ? 'index.html' : 'arm-smoke.html'}`);
    url.searchParams.set(cbench ? 'cbench' : official ? 'official' : quickjs ? 'quickjs' : 'auto', cbench ? 'quick' : official ? 'all' : '1');
    url.searchParams.set(cbench ? 'cbench-report' : official ? 'official-report' : quickjs ? 'quickjs-report' : 'report', `http://127.0.0.1:${port}/__result`);
    child = spawn(browser, ['--headless', '--no-remote', '--profile', profile, url.href], {stdio: ['ignore', 'pipe', 'pipe']});
    let hasClosed = false;
    const closed = new Promise(resolve => child.once('close', (code, signal) => {
      hasClosed = true;
      resolve({type: 'exit', code, signal});
    }));
    const failed = new Promise(resolve => child.once('error', error => resolve({type: 'error', error})));
    lifecycle = {closed, isClosed: () => hasClosed};
    child.stdout.on('data', chunk => process.stderr.write(chunk));
    child.stderr.on('data', chunk => process.stderr.write(chunk));
    const timeout = new Promise(resolve => {
      timeoutHandle = setTimeout(() => resolve({type: 'timeout'}), cbench ? 605000 : official ? 1805000 : quickjs ? 185000 : REPORT_TIMEOUT_MS);
    });
    const result = await Promise.race([resultPromise, closed, failed, timeout]);
    if (result.type === 'error') throw new Error(`could not start Firefox: ${result.error.message}`);
    if (result.type === 'report-error') throw new Error(`Firefox report: ${result.error.message}`);
    if (result.type === 'exit') throw new Error(`Firefox exited before reporting (code=${result.code}, signal=${result.signal})`);
    if (result.type === 'timeout') {
      process.exitCode = 2;
      throw new Error('Firefox Worker smoke timed out waiting for a page report');
    }
    observation.status = result.status;
    if (!result.status?.startsWith('PASS|')) throw new Error(`Firefox Worker smoke ${result.status || 'missing status'}`);
    observation.outcome = 'pass';
  } catch (error) {
    observation.error = error.message;
    throw error;
  } finally {
    clearTimeout(timeoutHandle);
    try {
      try {
        saveObservation();
      } finally {
        // Firefox may flush profile data after SIGTERM. Wait for process/stdio
        // closure before deleting it, or a late write can recreate the directory.
        if (child && lifecycle) await stopBrowser(child, lifecycle);
      }
    } finally {
      await new Promise(resolve => {
        server.close(resolve);
        server.closeAllConnections();
      });
      if (!lifecycle || lifecycle.isClosed()) fs.rmSync(profile, {recursive: true, force: true, maxRetries: 3});
    }
  }
  console.log(`PASS Firefox ${cbench ? 'C benchmark comparison' : official ? 'official V8 benchmark' : quickjs ? 'QuickJS comparison' : 'Worker'} smoke`);
}

try {
  await main();
} catch (error) {
  console.error(error.message);
  process.exitCode ||= 1;
}
