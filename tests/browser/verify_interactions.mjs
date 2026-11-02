#!/usr/bin/env node
// Real Chromium checks for the built example; no npm dependencies required.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import {pathToFileURL} from 'node:url';

const pause = ms => new Promise(resolve => setTimeout(resolve, ms));
async function closesWithin(closed, milliseconds) {
  let timer;
  try {
    return await Promise.race([
      closed.then(() => true),
      new Promise(resolve => { timer = setTimeout(() => resolve(false), milliseconds); }),
    ]);
  } finally { clearTimeout(timer); }
}
const [browser, directory = 'build-em/examples/browser/dist', ...extra] = process.argv.slice(2);
const checks = [];
let userAgent = '', artifactHashes = {};

async function main() {
  if (!browser || (extra.length && !(extra.length === 2 && extra[0] === '--report'))) {
    throw new Error('usage: verify_interactions.mjs <chrome-executable> [dist-directory] [--report output.json]');
  }
  const root = path.resolve(directory);
  const artifacts = ['index.html', 'styles.css', 'app.js', 'site.js', 'worker.js', 'arm_jit.js',
    'arm_jit.wasm', 'demo.bin', 'demo_a64.bin', 'expected.txt',
    'quickjs-app.js', 'quickjs-worker.js', 'quickjs-workloads.js',
    'quickjs-arm.bin', 'quickjs-wasm.js', 'quickjs-wasm.wasm', 'quickjs-manifest.json', 'wasm-build-manifest.json',
    'official-benchmarks-app.js', 'official-benchmarks-worker.js', 'official-benchmarks.json', 'official-v8-upstream.js', 'quickjs-extras-LICENSE.txt',
    'c-bench-app.js', 'c-bench-worker.js', 'c-bench-protocol.js', 'c-bench-manifest.json', 'c-bench-reference.json',
    'c-bench-source.c', 'c-bench-arm.bin', 'c-bench-x86.elf', 'c-bench.wasm', 'c-bench-v86.js', 'c-bench-v86.wasm', 'c-bench-v86-LICENSE.txt',
    'vendor/prism-1.30.0/prism-core.js', 'vendor/prism-1.30.0/prism-clike.js',
    'vendor/prism-1.30.0/prism-c.js', 'vendor/prism-1.30.0/prism-javascript.js', 'vendor/prism-1.30.0/LICENSE',
    'icon.svg', 'icon-192.png', 'icon-512.png', 'site.webmanifest', 'social-card.png',
    '404.html', 'robots.txt', 'sitemap.xml'];
  const missing = artifacts.filter(name => !fs.existsSync(path.join(root, name)));
  if (missing.length) throw new Error(`Missing browser artifacts: ${missing.join(', ')}; build browser_page first`);
  const hash = name => createHash('sha256').update(fs.readFileSync(path.join(root, name))).digest('hex');
  artifactHashes = Object.fromEntries(artifacts.map(name => [name, hash(name)]));
  const expected = fs.readFileSync(path.join(root, 'expected.txt'), 'utf8');
  assert.ok(expected.trim(), 'native reference must be nonempty');
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-browser-interactions-'));
  const prefix = '/nested/example/';
  let legacyScriptRequests = 0;
  let broken = null, delayExpected = false, loopGuest = false, loopGuestServed = false;
  const server = http.createServer(async (request, response) => {
    const url = new URL(request.url, 'http://localhost');
    if (!url.pathname.startsWith(prefix)) return response.writeHead(404).end();
    const name = url.pathname.slice(prefix.length) || 'index.html';
    if (name === 'cache-prime.html') {
      response.writeHead(200, {'content-type': 'text/html', 'cache-control': 'no-store'}).end(
        '<button id="c-bench-run">Ready</button><button id="stop" disabled>Stop</button><script src="official-benchmarks-app.js"></script>');
      return;
    }
    if (name === 'official-benchmarks-app.js' && !url.searchParams.has('v')) {
      // The previous UI read a playground control that the new HTML removed.
      // Keep that response fresh in the browser cache across the deployment.
      legacyScriptRequests++;
      response.writeHead(200, {'content-type': 'text/javascript', 'cache-control': 'max-age=3600'}).end(
        "window.cachedPlayground = !document.getElementById('stop').disabled;");
      return;
    }
    if (name === 'arm-smoke.html') return response.writeHead(200, {'content-type': 'text/html'}).end(fs.readFileSync(new URL('./fixtures/arm.html', import.meta.url)));
    if (!artifacts.includes(name) || name === broken) return response.writeHead(404).end();
    if (delayExpected && name === 'expected.txt') await pause(500);
    if (response.destroyed) return;
    try {
      // ARM32 `b .` never returns. Stop must kill actual guest execution.
      const servingLoop = loopGuest && name === 'demo.bin';
      const body = servingLoop
        ? Buffer.from([0xfe, 0xff, 0xff, 0xea]) : fs.readFileSync(path.join(root, name));
      const types = {'.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript',
        '.wasm': 'application/wasm', '.c': 'text/plain', '.elf': 'application/octet-stream', '.txt': 'text/plain', '.svg': 'image/svg+xml',
        '.png': 'image/png', '.webmanifest': 'application/manifest+json', '.xml': 'application/xml', '.json': 'application/json'};
      response.writeHead(200, {'content-type': types[path.extname(name)] || 'application/octet-stream'})
        .end(body, () => { if (servingLoop) loopGuestServed = true; });
    } catch { response.writeHead(500).end(); }
  });
  let child, closed, childError, hasClosed = false, socket;
  let log = '';
  const pending = new Map();
  const rejectPending = () => {
    for (const entry of pending.values()) entry.reject(new Error('Chrome DevTools connection closed'));
    pending.clear();
  };
  const until = async (condition, description, limit = 30000) => {
    const deadline = Date.now() + limit;
    do {
      if (childError) throw childError;
      if (hasClosed) throw new Error('Chrome exited before completing the interaction checks');
      const result = await condition();
      if (result) return result;
      await pause(50);
    } while (Date.now() < deadline);
    throw new Error(`Timed out: ${description}`);
  };
  const record = (check, state = {}) => {
    checks.push({check, ...state}); console.log(`PASS ${check}`);
  };
  try {
    await new Promise((resolve, reject) => {
      server.once('error', reject); server.listen(0, '127.0.0.1', resolve);
    });
    const base = `http://127.0.0.1:${server.address().port}${prefix}`;
    child = spawn(browser, ['--headless=new', '--no-sandbox', '--disable-gpu',
      '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'],
    {stdio: ['ignore', 'ignore', 'pipe']});
    child.once('error', error => { childError = new Error(`Could not start Chrome: ${error.message}`); });
    closed = new Promise(resolve => child.once('close', () => { hasClosed = true; resolve(); }));
    child.stderr.on('data', chunk => { log = (log + chunk).slice(-12000); });
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
      if (entry) {
        pending.delete(message.id);
        if (message.error) entry.reject(new Error(JSON.stringify(message.error)));
        else entry.resolve(message.result);
      }
    };
    socket.onclose = socket.onerror = rejectPending;
    const call = (method, params = {}) => new Promise((resolve, reject) => {
      const key = ++id;
      const timer = setTimeout(() => { pending.delete(key); reject(new Error(`Chrome DevTools timeout: ${method}`)); }, 10000);
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
    const navigate = async url => {
      const result = await call('Page.navigate', {url});
      if (result.errorText) throw new Error(`Chrome navigation: ${result.errorText}`);
      await until(() => evaluate(`location.href === ${JSON.stringify(url)} && document.readyState === 'complete' && !!(document.getElementById('run') || document.getElementById('c-bench-run'))`), 'page load');
    };
    const status = () => evaluate(`({
      title: document.title, status: (document.getElementById('status') || document.getElementById('c-bench-status')).textContent,
      runDisabled: (document.getElementById('run') || document.getElementById('c-bench-run')).disabled, stopDisabled: (document.getElementById('stop') || document.getElementById('c-bench-stop')).disabled,
      mode: document.getElementById('memory-mode')?.value,
      output: [0, 1].map(i => document.getElementById('out' + i)?.textContent),
      width: document.documentElement.clientWidth, scrollWidth: document.documentElement.scrollWidth
    })`);
    const click = id => evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);
    const tabTo = async id => {
      for (let count = 0; count < 20; count++) {
        if (await evaluate(`document.activeElement.id === ${JSON.stringify(id)}`)) return;
        await call('Input.dispatchKeyEvent', {type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9});
        await call('Input.dispatchKeyEvent', {type: 'keyUp', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9});
      }
      throw new Error(`Keyboard focus did not reach ${id}`);
    };
    const focusGeometry = () => evaluate(`(() => {
      const element = document.activeElement;
      const rect = element.getBoundingClientRect();
      const header = document.querySelector('.site-header').getBoundingClientRect();
      const style = getComputedStyle(element);
      const hit = document.elementFromPoint(rect.left + rect.width / 2, rect.top + rect.height / 2);
      return {id: element.id, focusVisible: element.matches(':focus-visible'),
        outlineStyle: style.outlineStyle, outlineWidth: parseFloat(style.outlineWidth),
        top: rect.top, bottom: rect.bottom, headerBottom: header.bottom, viewportHeight: innerHeight,
        hit: hit === element || element.contains(hit)};
    })()`);
    const stickyGeometry = async (statusId, toolbarSelector, stopId) => {
      await evaluate(`(() => {
        const toolbar = document.querySelector('${toolbarSelector}');
        const toolbarDocumentTop = toolbar.getBoundingClientRect().top + scrollY;
        document.getElementById('${statusId}').dataset.state = 'running';
        if (innerHeight <= 420) {
          document.getElementById('${stopId}').scrollIntoView({behavior: 'instant', block: 'center'});
        } else {
          const target = toolbarDocumentTop - parseFloat(getComputedStyle(toolbar).top) + 50;
          document.getElementById('${stopId}').scrollIntoView({behavior: 'instant', block: 'center'});
          window.scrollTo({top: target, behavior: 'instant'});
        }
      })(); undefined;`);
      const expectedPosition = await evaluate("innerHeight <= 420 ? 'static' : 'sticky'");
      await until(() => evaluate(`getComputedStyle(document.querySelector('${toolbarSelector}')).position === '${expectedPosition}'`), `${statusId} ${expectedPosition} toolbar`);
      await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
      return evaluate(`(() => {
        const toolbar = document.querySelector('${toolbarSelector}');
        const lab = toolbar.closest('.c-bench-lab, .q-lab, .official-lab');
        const stop = document.getElementById('${stopId}');
        const toolbarRect = toolbar.getBoundingClientRect();
        const stopRect = stop.getBoundingClientRect();
        const hit = document.elementFromPoint(stopRect.left + stopRect.width / 2, stopRect.top + stopRect.height / 2);
        return {position: getComputedStyle(toolbar).position, toolbarTop: toolbarRect.top, toolbarBottom: toolbarRect.bottom,
          stickyTop: parseFloat(getComputedStyle(toolbar).top),
          headerBottom: document.querySelector('.site-header').getBoundingClientRect().bottom,
          labBottom: lab.getBoundingClientRect().bottom, stopTop: stopRect.top, stopBottom: stopRect.bottom,
          viewportHeight: innerHeight, stopHit: hit === stop || stop.contains(hit)};
      })()`);
    };
    const themeState = () => evaluate(`({
      label: document.getElementById('theme-label').textContent,
      theme: document.documentElement.dataset.theme || 'system',
      stored: localStorage.getItem('dynarmic-theme')
    })`);
    const activeSection = async id => {
      await until(() => evaluate(`document.querySelector('.site-header nav a[href="#${id}"]').getAttribute('aria-current') === 'location'`), `${id} navigation state`);
      assert.deepEqual(await evaluate(`[...document.querySelectorAll('.site-header nav a[aria-current]')].map(link => link.getAttribute('href'))`), [`#${id}`]);
    };
    const completed = () => until(() => evaluate("document.title.startsWith('DONE ')"), 'guest completion', 125000);
    const passing = async description => {
      await completed(); const state = await status();
      assert.equal(state.title, 'DONE PASS', state.status);
      assert.deepEqual(state.output, [expected, expected]);
      assert.equal(state.runDisabled, false); assert.equal(state.stopDisabled, true);
      record(description, state);
    };
    const stopped = async description => {
      await completed(); const state = await status();
      assert.equal(state.title, 'DONE CANCELLED', state.status);
      assert.equal(state.runDisabled, false); assert.equal(state.stopDisabled, true);
      record(description, state);
    };

    await call('Page.enable');
    await call('Emulation.setDeviceMetricsOverride', {width: 1280, height: 900, deviceScaleFactor: 1, mobile: false});
    await navigate(new URL('cache-prime.html', base).href);
    assert.equal(await evaluate('window.cachedPlayground'), false, 'old playground script must be cached first');
    await call('Page.addScriptToEvaluateOnNewDocument', {source: `window.pageErrors = [];
      addEventListener('error', event => pageErrors.push(event.message));
      addEventListener('unhandledrejection', event => pageErrors.push(String(event.reason)));`});
    await navigate(base);
    await until(() => evaluate("!document.getElementById('official-run').disabled"), 'current QuickJS UI initialization');
    assert.deepEqual(await evaluate('window.pageErrors'), []);
    assert.equal(await evaluate('typeof window.cachedPlayground'), 'undefined');
    assert.equal(legacyScriptRequests, 1, 'the new page must bypass the cached, incompatible script URL');
    for (const [asset, version] of await evaluate("[...document.querySelectorAll('script[src], link[rel=stylesheet]')].map(element => {const url = new URL(element.src || element.href); return [url.pathname.split('/').pop(), url.searchParams.get('v')];})")) {
      assert.equal(version, hash(asset), `${asset} URL must identify its exact built content`);
    }
    record('a returning visitor with cached playground scripts loads the matching new QuickJS UI');
    userAgent = await evaluate('navigator.userAgent');
    assert.equal((await status()).runDisabled, false);
    assert.equal(await evaluate("document.getElementById('c-bench-status').getAttribute('role')"), 'status');
    record('nested-path first load and accessible status', await status());
    await call('Input.dispatchKeyEvent', {type: 'keyDown', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9});
    await call('Input.dispatchKeyEvent', {type: 'keyUp', key: 'Tab', code: 'Tab', windowsVirtualKeyCode: 9});
    assert.equal(await evaluate('document.activeElement.textContent'), 'Skip to benchmarks');
    assert.equal(await evaluate('document.activeElement.getAttribute("href")'), '#c-bench');
    record('keyboard skip link');
    const skipFocus = {};
    for (const [width, height] of [[1280, 900], [375, 844]]) {
      await call('Emulation.setDeviceMetricsOverride', {width, height, deviceScaleFactor: 1, mobile: false});
      await evaluate(`window.scrollTo({top: 0, behavior: 'instant'}); document.querySelector('.skip-link').focus()`);
      await evaluate('new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)))');
      const geometry = skipFocus[width] = await focusGeometry();
      assert.equal(await evaluate("document.activeElement === document.querySelector('.skip-link')"), true);
      assert.equal(geometry.focusVisible, true, `${width}px skip link must have keyboard-visible focus`);
      assert.ok(geometry.top >= 0 && geometry.bottom <= geometry.viewportHeight, `${width}px focused skip link must be inside the viewport`);
      assert.ok(geometry.top < geometry.headerBottom, `${width}px skip link regression must overlap the sticky header`);
      assert.equal(geometry.hit, true, `${width}px focused skip link must be hit-testable above the sticky header: ${JSON.stringify(geometry)}`);
    }
    record('focused skip link is hit-testable above the sticky header at desktop and narrow widths', skipFocus);
    await call('Emulation.setDeviceMetricsOverride', {width: 1280, height: 900, deviceScaleFactor: 1, mobile: false});

    // The first screen must offer a visible action and load no execution runtime
    // until it is requested. The heavier Wasm engine is not presentation artwork.
    assert.equal(await evaluate(`document.querySelector('.hero-actions a').getBoundingClientRect().bottom < innerHeight`), true);
    assert.deepEqual(await evaluate(`performance.getEntriesByType('resource').filter(r => /arm_jit|demo\\.bin|demo_a64/.test(r.name)).map(r => r.name)`), []);
    record('visible first action and on-demand runtime');
    assert.deepEqual(await evaluate("performance.getEntriesByType('resource').filter(resource => resource.name.includes('/prism-')).map(resource => resource.name)"), [],
      'the highlighter must not load before a source viewer opens');
    assert.deepEqual(await evaluate("[...document.querySelectorAll('main > section[id]')].map(section => section.id)"),
      ['intro', 'build', 'c-bench', 'quickjs', 'architecture']);
    assert.equal(await evaluate("!!document.getElementById('demo') || !!document.querySelector('script[src=\"app.js\"]')"), false);
    assert.equal(await evaluate("document.getElementById('quickjs-suite').value"), 'official');
    assert.equal(await evaluate("document.getElementById('quickjs-custom').hidden"), true);
    record('package-first layout, one QuickJS benchmark area and no ARM playground');

    assert.deepEqual(await themeState(), {label: 'System', theme: 'system', stored: null});
    await click('theme-toggle');
    assert.deepEqual(await themeState(), {label: 'Light', theme: 'light', stored: 'light'});
    await navigate(base);
    assert.deepEqual(await themeState(), {label: 'Light', theme: 'light', stored: 'light'});
    await click('theme-toggle');
    assert.deepEqual(await themeState(), {label: 'Dark', theme: 'dark', stored: 'dark'});
    await navigate(base);
    assert.deepEqual(await themeState(), {label: 'Dark', theme: 'dark', stored: 'dark'});
    record('System to Light to Dark theme cycling persists explicit choices across reloads');
    await click('theme-toggle');
    assert.deepEqual(await themeState(), {label: 'System', theme: 'system', stored: null});
    await navigate(base);
    assert.deepEqual(await themeState(), {label: 'System', theme: 'system', stored: null});
    record('System theme removes the stored preference and remains the default after reload');

    await evaluate(`window.savedLocalStorage = window.localStorage;
      Object.defineProperty(window, 'localStorage', {configurable: true, get() {
        throw new DOMException('Storage blocked by interaction test', 'SecurityError');
      }}); undefined;`);
    for (const [label, theme] of [['Light', 'light'], ['Dark', 'dark'], ['System', 'system']]) {
      await click('theme-toggle');
      assert.equal(await evaluate("document.getElementById('theme-label').textContent"), label);
      assert.equal(await evaluate("document.documentElement.dataset.theme || 'system'"), theme);
    }
    assert.deepEqual(await evaluate('window.pageErrors'), []);
    await evaluate(`Object.defineProperty(window, 'localStorage', {configurable: true, value: window.savedLocalStorage});
      delete window.savedLocalStorage;`);
    record('blocked localStorage does not prevent System, Light and Dark theme changes');

    await until(() => evaluate(`!document.getElementById('pipeline-toggle').hidden && document.querySelector('.engine').classList.contains('is-animated')`), 'visible pipeline animation');
    await click('pipeline-toggle');
    assert.equal(await evaluate("document.getElementById('pipeline-toggle').getAttribute('aria-pressed')"), 'true');
    assert.equal(await evaluate("document.getElementById('pipeline-toggle').textContent"), 'Play motion');
    assert.equal(await evaluate("document.querySelector('.engine').classList.contains('is-animated')"), false);
    assert.equal(await evaluate("getComputedStyle(document.querySelector('.pipeline-flow')).animationName"), 'none');
    await click('pipeline-toggle');
    assert.equal(await evaluate("document.getElementById('pipeline-toggle').getAttribute('aria-pressed')"), 'false');
    assert.equal(await evaluate("document.getElementById('pipeline-toggle').textContent"), 'Pause motion');
    await until(() => evaluate("document.querySelector('.engine').classList.contains('is-animated')"), 'pipeline play');
    record('pipeline pause and play update animation and accessible control state');

    await activeSection('intro');
    for (const id of ['build', 'c-bench', 'quickjs', 'architecture']) {
      await evaluate(`document.getElementById('${id}').scrollIntoView({behavior: 'instant', block: 'start'})`);
      await activeSection(id);
    }
    record('active-section navigation exposes exactly one aria-current location');
    await until(() => evaluate("!document.querySelector('.engine').classList.contains('is-animated')"), 'offscreen pipeline pause');
    assert.equal(await evaluate("document.getElementById('pipeline-toggle').getAttribute('aria-pressed')"), 'false');
    await evaluate("window.scrollTo({top: 0, behavior: 'instant'})");
    await activeSection('intro');
    await until(() => evaluate("document.querySelector('.engine').classList.contains('is-animated')"), 'onscreen pipeline resume');
    record('pipeline automatically pauses offscreen and resumes when visible');

    await call('Emulation.setEmulatedMedia', {features: [{name: 'prefers-reduced-motion', value: 'reduce'}]});
    await until(() => evaluate(`document.getElementById('pipeline-toggle').hidden && !document.querySelector('.engine').classList.contains('is-animated')`), 'reduced-motion pipeline state');
    assert.equal(await evaluate("getComputedStyle(document.querySelector('.pipeline-flow')).animationName"), 'none');
    assert.equal(await evaluate("getComputedStyle(document.querySelector('.diagram-node')).animationName"), 'none');
    record('reduced motion makes the pipeline static and hides its motion control');
    await call('Emulation.setEmulatedMedia', {features: []});
    await until(() => evaluate(`!document.getElementById('pipeline-toggle').hidden && document.querySelector('.engine').classList.contains('is-animated')`), 'pipeline motion preference restoration');

    await evaluate("document.querySelector('.skip-link').focus()");
    await tabTo('theme-toggle');
    const themeFocus = await focusGeometry();
    assert.equal(themeFocus.id, 'theme-toggle');
    assert.equal(themeFocus.focusVisible, true);
    assert.notEqual(themeFocus.outlineStyle, 'none');
    assert.ok(themeFocus.outlineWidth > 0, 'theme keyboard focus must have a visible outline');
    assert.ok(themeFocus.top >= 0 && themeFocus.bottom <= themeFocus.viewportHeight, 'theme control must be inside the viewport');
    assert.equal(themeFocus.hit, true, 'theme control must not be covered at its center');
    record('theme control has visible, unobstructed keyboard focus');
    await tabTo('pipeline-toggle');
    const pipelineFocus = await focusGeometry();
    assert.equal(pipelineFocus.id, 'pipeline-toggle');
    assert.equal(pipelineFocus.focusVisible, true);
    assert.notEqual(pipelineFocus.outlineStyle, 'none');
    assert.ok(pipelineFocus.outlineWidth > 0, 'pipeline keyboard focus must have a visible outline');
    assert.ok(pipelineFocus.top >= 0 && pipelineFocus.bottom <= pipelineFocus.viewportHeight, 'pipeline control must be inside the viewport');
    assert.ok(pipelineFocus.top >= pipelineFocus.headerBottom - 1, 'pipeline control must not sit underneath the sticky header');
    assert.equal(pipelineFocus.hit, true, 'pipeline control must not be covered at its center');
    record('pipeline control has visible keyboard focus clear of the sticky header');

    for (const [stage, heading] of [['emit', 'Turn optimized IR into WebAssembly.'],
      ['execute', 'Compile once. Reuse the translation.'], ['decode', "Start with the guest's instructions."]]) {
      await evaluate(`document.querySelector('[data-stage="${stage}"]').click()`);
      assert.equal(await evaluate(`document.getElementById('stage-title').textContent`), heading);
      assert.equal(await evaluate(`document.querySelectorAll('[data-stage][aria-pressed="true"]').length`), 1);
      assert.equal(await evaluate(`document.querySelector('[data-stage="${stage}"]').getAttribute('aria-pressed')`), 'true');
    }
    record('architecture stages expose the selected content and state');
    await call('Browser.grantPermissions', {origin: new URL(base).origin,
      permissions: ['clipboardReadWrite', 'clipboardSanitizedWrite']});
    await evaluate(`document.getElementById('cpp-example').open = true; document.getElementById('npm-browser-example').open = true`);
    for (const id of ['npm-install-command', 'npm-node-example', 'npm-vite-config', 'npm-vite-html', 'npm-vite-main', 'npm-vite-worker']) {
      await evaluate(`document.querySelector('[data-copy="${id}"]').click()`);
      await until(() => evaluate(`document.getElementById('copy-status').textContent === document.querySelector('[data-copy="${id}"]').dataset.copyLabel + ' copied.'`), 'package example clipboard copy');
      assert.equal(await evaluate('navigator.clipboard.readText()'),
        await evaluate(`document.getElementById('${id}').textContent`));
    }
    record('complete package examples copy as plain executable source');
    await evaluate(`document.querySelector('[data-copy="clone-command"]').click()`);
    await until(() => evaluate(`document.getElementById('copy-status').textContent === 'Clone commands copied.'`), 'clipboard copy');
    assert.equal(await evaluate('navigator.clipboard.readText()'),
      await evaluate(`document.getElementById('clone-command').textContent`));
    await evaluate(`window.savedClipboardWriteText = navigator.clipboard.writeText.bind(navigator.clipboard); navigator.clipboard.writeText = async () => { throw new Error('permission denied'); }; document.querySelector('[data-copy="run-command"]').click()`);
    await until(() => evaluate(`document.getElementById('copy-status').textContent.startsWith('Clipboard unavailable.')`), 'clipboard fallback');
    assert.equal(await evaluate('window.getSelection().toString()'), 'node examples/node/hello.mjs');
    record('copy commands and denied-clipboard selection fallback');
    await evaluate('navigator.clipboard.writeText = window.savedClipboardWriteText');
    for (const [id, file, suite] of [['official-source', 'official-v8-upstream.js', 'official'],
      ['quickjs-source', 'quickjs-workloads.js', 'custom']]) {
      await evaluate(`document.getElementById('quickjs-suite').value = '${suite}'; document.getElementById('quickjs-suite').dispatchEvent(new Event('change')); document.getElementById('${id}').open = true`);
      await until(() => evaluate(`document.getElementById('${id}').getAttribute('aria-busy') === 'false' && !!document.querySelector('#${id} code').textContent`), 'inline benchmark source');
      assert.equal(await evaluate(`document.querySelector('#${id} code').textContent`), fs.readFileSync(path.join(root, file), 'utf8'));
      assert.equal(await evaluate(`document.getElementById('${id}').closest('.benchmark-panel').hidden`), false);
      assert.equal(await evaluate(`!!document.querySelector('#${id} .token.keyword')`), true, 'JavaScript keywords must be highlighted');
      await evaluate(`document.querySelector('#${id} .source-copy').click()`);
      await until(() => evaluate(`document.querySelector('#${id} .source-status').textContent === 'Source code copied.'`), 'source copy');
      assert.equal(await evaluate('navigator.clipboard.readText()'), fs.readFileSync(path.join(root, file), 'utf8'));
    }
    broken = 'c-bench-source.c';
    await evaluate("document.getElementById('c-bench-source').open = true");
    await until(() => evaluate("!document.querySelector('#c-bench-source .source-retry').hidden"), 'source error recovery');
    assert.match(await evaluate("document.querySelector('#c-bench-source .source-status').textContent"), /HTTP 404/);
    broken = null;
    await evaluate("document.querySelector('#c-bench-source .source-retry').click()");
    await until(() => evaluate("document.getElementById('c-bench-source').getAttribute('aria-busy') === 'false' && !!document.querySelector('#c-bench-source code').textContent"), 'C source retry');
    assert.equal(await evaluate("document.querySelector('#c-bench-source code').textContent"), fs.readFileSync(path.join(root, 'c-bench-source.c'), 'utf8'));
    assert.equal(await evaluate("!!document.querySelector('#c-bench-source .token.keyword')"), true, 'C keywords must be highlighted');
    assert.deepEqual(await evaluate("performance.getEntriesByType('resource').filter(r => /\\.(wasm|bin)$/.test(r.name)).map(r => r.name)"), []);
    record('both QuickJS suites and C show exact benchmark code, retry failed loads and load no engines');
    broken = 'vendor/prism-1.30.0/prism-javascript.js';
    await navigate(base + '?quickjs-suite=custom');
    await evaluate("document.getElementById('quickjs-source').open = true");
    await until(() => evaluate("document.querySelector('#quickjs-source .source-status').textContent.includes('plain text')"), 'plain source fallback');
    assert.equal(await evaluate("document.querySelector('#quickjs-source code').textContent"), fs.readFileSync(path.join(root, 'quickjs-workloads.js'), 'utf8'));
    assert.equal(await evaluate("document.querySelector('#quickjs-source .source-copy').disabled"), false);
    broken = null;
    record('missing syntax grammar falls back to readable, copyable original source');
    await navigate(new URL('arm-smoke.html', base).href);
    for (const [mode, hint] of [['0', 'Callbacks route'], ['1', 'Page table uses'], ['2', 'Flat window uses']]) {
      await evaluate(`document.getElementById('memory-mode').value = '${mode}'; document.getElementById('memory-mode').dispatchEvent(new Event('change'))`);
      assert.ok((await evaluate(`document.getElementById('mode-hint').textContent`)).startsWith(hint));
    }
    record('memory choices explain the active path');

    // Finish module initialization and prove the cached Worker can execute before
    // testing interruption. Otherwise a slow host might only cancel startup.
    await click('run'); await completed();
    const warm = await status();
    assert.equal(warm.title, 'DONE PASS', warm.status);
    assert.deepEqual(warm.output, [expected, expected]);
    loopGuest = true;
    await click('run');
    await until(() => loopGuestServed, 'infinite guest response');
    await pause(750);
    const looping = await status();
    assert.equal(looping.title, 'Running demo guests'); assert.match(looping.status, /Running ARM32/);
    await click('stop'); await stopped('stop an infinite ARM guest loop');
    loopGuest = false;
    await click('run'); await passing('retry after termination / flat-window memory');
    for (const mode of ['0', '1']) {
      await navigate(`${new URL('arm-smoke.html', base).href}?auto=1&mode=${mode}`);
      await passing(`memory mode ${mode}`); assert.equal((await status()).mode, mode);
    }

    await navigate(base);
    await call('Emulation.setDeviceMetricsOverride', {width: 375, height: 844, deviceScaleFactor: 1, mobile: true});
    await evaluate("document.querySelectorAll('details').forEach(element => element.open = true)");
    const mobile = await status();
    assert.ok(mobile.scrollWidth <= mobile.width, '375px page must not overflow horizontally');
    record('375px layout with expanded statistics', mobile);
    for (const width of [320, 390, 768, 1024, 1280, 1440]) {
      await call('Emulation.setDeviceMetricsOverride', {width, height: 900, deviceScaleFactor: 1, mobile: false});
      const layout = await status();
      assert.ok(layout.scrollWidth <= layout.width, `${width}px page must not overflow horizontally`);
      assert.deepEqual(await evaluate(`[...document.querySelectorAll('main *')].filter(e => {
        const rect = e.getBoundingClientRect(); return rect.width && rect.right > document.documentElement.clientWidth + 1;
      }).map(e => e.id || e.tagName)`), [], `${width}px content must not be clipped`);
    }
    record('320px through 1440px layout without hidden overflow');

    const runningGeometry = {};
    for (const height of [900, 320, 300]) {
      await call('Emulation.setDeviceMetricsOverride', {width: 640, height, deviceScaleFactor: 1, mobile: false});
      const geometryAtHeight = runningGeometry[height] = {};
      geometryAtHeight.c = await stickyGeometry('c-bench-status', '.c-bench-controls', 'c-bench-stop');
      await evaluate(`document.getElementById('quickjs-suite').value = 'custom';
        document.getElementById('quickjs-suite').dispatchEvent(new Event('change'));`);
      await until(() => evaluate("!document.getElementById('quickjs-custom').hidden"), 'custom QuickJS panel');
      geometryAtHeight.quickjs = await stickyGeometry('quickjs-status', '.q-controls', 'quickjs-stop');
      await evaluate(`document.getElementById('quickjs-suite').value = 'official';
        document.getElementById('quickjs-suite').dispatchEvent(new Event('change'));`);
      await until(() => evaluate("!document.getElementById('official-benchmarks').hidden"), 'official QuickJS panel');
      geometryAtHeight.official = await stickyGeometry('official-status', '.official-toolbar', 'official-stop');
      await evaluate(`for (const id of ['c-bench-status', 'quickjs-status', 'official-status']) {
        document.getElementById(id).dataset.state = 'idle';
      }`);
    }
    for (const [height, geometryAtHeight] of Object.entries(runningGeometry)) {
      for (const [name, geometry] of Object.entries(geometryAtHeight)) {
        const context = `640x${height} ${name}: ${JSON.stringify(geometry)}`;
        if (height === '900') {
          assert.ok(Math.abs(geometry.toolbarTop - geometry.stickyTop) < 1, `${context} toolbar must be held at its sticky top offset`);
          assert.ok(geometry.toolbarTop >= geometry.headerBottom - 1, `${context} sticky toolbar must not sit underneath the header`);
        } else {
          assert.equal(geometry.position, 'static', `${context} short viewports must use ordinary document flow`);
        }
        assert.ok(geometry.labBottom >= geometry.toolbarBottom, `${context} sticky toolbar must still be within its lab`);
        assert.ok(geometry.stopTop >= 0 && geometry.stopBottom <= geometry.viewportHeight, `${context} Stop must remain fully inside the viewport`);
        assert.equal(geometry.stopHit, true, `${context} Stop must remain hit-testable`);
      }
    }
    record('640px running C, QuickJS and official toolbars keep Stop hit-testable below the header in 900px, 320px and 300px viewports', runningGeometry);

    await call('Emulation.setEmulatedMedia', {features: [{name: 'prefers-reduced-motion', value: 'reduce'}]});
    assert.equal(await evaluate(`getComputedStyle(document.documentElement).scrollBehavior`), 'auto');
    await evaluate(`document.getElementById('c-bench-status').className = 'status running'`);
    assert.equal(await evaluate(`getComputedStyle(document.getElementById('c-bench-status'), '::before').animationName`), 'none');
    record('reduced motion disables smooth scrolling and animation');
    await call('Emulation.setEmulatedMedia', {features: []});
    broken = 'expected.txt';
    await navigate(`${new URL('arm-smoke.html', base).href}?auto=1`); await completed();
    const missing = await status();
    assert.equal(missing.title, 'DONE FAIL'); assert.match(missing.status, /HTTP 404/);
    assert.equal(missing.runDisabled, false); record('missing asset failure', missing);
    broken = null;
    await click('run'); await passing('retry after asset recovery');
    delayExpected = true;
    await click('run'); await pause(30); await click('stop');
    await stopped('stop while loading'); delayExpected = false;
    await navigate(pathToFileURL(path.join(root, 'index.html')).href);
    assert.match(await evaluate("document.getElementById('file-guidance').textContent"), /Serve the built dist/);
    record('file URL deployment guidance');
    for (const name of artifacts) assert.equal(hash(name), artifactHashes[name], `${name} changed during verification`);
    console.log(`PASS Chromium interactions (${checks.length} checks): ${userAgent}`);
  } catch (error) {
    if (log) console.error(log);
    throw error;
  } finally {
    rejectPending(); socket?.close();
    if (child && !hasClosed) {
      child.kill('SIGTERM');
      if (!await closesWithin(closed, 1500)) {
        child.kill('SIGKILL'); await closesWithin(closed, 1500);
      }
    }
    server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    if (!child || hasClosed) fs.rmSync(profile, {recursive: true, force: true, maxRetries: 3});
    else throw new Error(`Chrome did not close; its profile was retained at ${profile}`);
  }
}

let failure;
try { await main(); }
catch (error) { failure = error.message; console.error(failure); process.exitCode = 1; }
if (extra.length === 2 && extra[0] === '--report') {
  try {
    fs.writeFileSync(extra[1], JSON.stringify({browser: userAgent, directory: path.resolve(directory),
      artifactHashes, checks, ...(failure ? {error: failure} : {})}, null, 2) + '\n', {flag: 'wx'});
  } catch (error) { console.error(`Could not write report: ${error.message}`); process.exitCode = 1; }
}
