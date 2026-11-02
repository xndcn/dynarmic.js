// Small dependency-free CDP harness for real local browser acceptance checks.
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

export const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const types = {'.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
  '.wasm': 'application/wasm', '.json': 'application/json', '.txt': 'text/plain', '.c': 'text/plain', '.elf': 'application/octet-stream',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.webmanifest': 'application/manifest+json'};

export async function withChromium({browser, root, serve, width = 1280, height = 900}, check) {
  root = path.resolve(root);
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-quickjs-browser-'));
  const prefix = '/nested/quickjs/';
  const server = http.createServer(async (request, response) => {
    try {
      const url = new URL(request.url, 'http://localhost');
      if (!url.pathname.startsWith(prefix)) return response.writeHead(404).end();
      const name = decodeURIComponent(url.pathname.slice(prefix.length)) || 'index.html';
      const file = path.resolve(root, name);
      if (!file.startsWith(root + path.sep)) return response.writeHead(403).end();
      if (await serve?.({name, response})) return;
      const body = await fs.promises.readFile(file);
      if (!response.destroyed) response.writeHead(200, {'content-type': types[path.extname(name)] || 'application/octet-stream'}).end(body);
    } catch (error) {
      if (!response.destroyed) response.writeHead(error.code === 'ENOENT' ? 404 : 500).end();
    }
  });
  let child, closed, exited = false, childError, socket, stderr = '';
  const pending = new Map();
  const until = async (condition, description, limit = 30000) => {
    const deadline = Date.now() + limit;
    do {
      if (childError) throw childError;
      if (exited) throw new Error('Chromium exited before the checks completed');
      const value = await condition();
      if (value) return value;
      await pause(50);
    } while (Date.now() < deadline);
    throw new Error(`Timed out: ${description}`);
  };
  const rejectPending = () => {
    for (const entry of pending.values()) entry.reject(new Error('Chrome DevTools connection closed'));
    pending.clear();
  };
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
    });
    child = spawn(browser, ['--headless=new', '--no-sandbox', '--disable-gpu',
      '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'],
    {stdio: ['ignore', 'ignore', 'pipe']});
    closed = new Promise(resolve => child.once('close', () => { exited = true; resolve(); }));
    child.once('error', error => { childError = new Error(`Could not start Chromium: ${error.message}`); });
    child.stderr.on('data', chunk => { stderr = (stderr + chunk).slice(-12000); });
    const port = await until(() => {
      try { return Number(fs.readFileSync(path.join(profile, 'DevToolsActivePort'), 'utf8').split('\n')[0]); }
      catch { return null; }
    }, 'Chrome DevTools startup', 15000);
    const response = await fetch(`http://127.0.0.1:${port}/json/list`, {signal: AbortSignal.timeout(5000)});
    if (!response.ok) throw new Error(`Chrome target discovery: HTTP ${response.status}`);
    const targets = await response.json();
    const target = Array.isArray(targets) && targets.find(entry => entry?.type === 'page');
    if (!target?.webSocketDebuggerUrl) throw new Error('Chrome did not expose a page target');
    socket = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('Chrome DevTools connection timed out')), 10000);
      socket.onopen = () => { clearTimeout(timer); resolve(); };
      socket.onerror = socket.onclose = () => { clearTimeout(timer); reject(new Error('Chrome DevTools connection failed')); };
    });
    let id = 0;
    socket.onmessage = ({data}) => {
      let message;
      try { message = JSON.parse(data); } catch { return; }
      const entry = pending.get(message.id);
      if (!entry) return;
      pending.delete(message.id);
      if (message.error) entry.reject(new Error(JSON.stringify(message.error)));
      else entry.resolve(message.result);
    };
    socket.onclose = socket.onerror = rejectPending;
    const call = (method, params = {}) => new Promise((resolve, reject) => {
      const key = ++id;
      const timer = setTimeout(() => { pending.delete(key); reject(new Error(`Chrome DevTools timeout: ${method}`)); }, 15000);
      pending.set(key, {
        resolve: value => { clearTimeout(timer); resolve(value); },
        reject: error => { clearTimeout(timer); reject(error); },
      });
      try { socket.send(JSON.stringify({id: key, method, params})); }
      catch (error) { clearTimeout(timer); pending.delete(key); reject(error); }
    });
    const evaluate = async expression => {
      const result = await call('Runtime.evaluate', {expression, returnByValue: true, awaitPromise: true});
      if (result.exceptionDetails) throw new Error(`Page evaluation: ${result.exceptionDetails.text}`);
      return result.result?.value;
    };
    const base = `http://127.0.0.1:${server.address().port}${prefix}`;
    const navigate = async (suffix = '') => {
      const url = new URL(suffix, base).href;
      const result = await call('Page.navigate', {url});
      if (result.errorText) throw new Error(`Chrome navigation: ${result.errorText}`);
      await until(() => evaluate(`location.href === ${JSON.stringify(url)} && document.readyState === 'complete'`), 'page load');
    };
    await call('Page.enable');
    await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: 1, mobile: false});
    await check({base, call, evaluate, navigate, until, browserPid: child.pid});
  } catch (error) {
    if (stderr) console.error(stderr);
    throw error;
  } finally {
    rejectPending(); socket?.close();
    if (child && !exited) {
      child.kill('SIGTERM');
      await Promise.race([closed, pause(1500)]);
      if (!exited) { child.kill('SIGKILL'); await Promise.race([closed, pause(1500)]); }
    }
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    if (!child || exited) fs.rmSync(profile, {recursive: true, force: true, maxRetries: 3});
    else throw new Error(`Chromium did not close; profile retained at ${profile}`);
  }
}
