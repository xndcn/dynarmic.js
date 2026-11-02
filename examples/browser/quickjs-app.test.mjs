// Fault injection for the UI protocol. Real browsers separately run all engines.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('quickjs-app.js', import.meta.url), 'utf8');
const workloadSource = readFileSync(new URL('../quickjs/workloads.js', import.meta.url), 'utf8');
const tick = () => new Promise(resolve => setImmediate(resolve));
const kernels = ['lcg', 'sieve', 'pi'];
function reply(runtime, profile = 'light') {
  return {ok: true, runtime, loadMs: 1, runMs: 20,
    memoryBytes: runtime === 'native' ? null : 16777216,
    stats: runtime === 'dynarmic' ? {blocks: 100, bytes: 4096, compileMs: 2, instMs: 1} : {},
    benchmark: {profile, parameters: {lcg: 20000, sieve: 8192, pi: 20000},
      outputs: {lcg: 2778886796, sieve: 1028, pi: 3141542}, warmupRuns: 1, measuredRuns: 3,
      coldMs: {lcg: 5, sieve: 5, pi: 5},
      samplesMs: Object.fromEntries(kernels.map(name => [name, [1, 2, 3]])),
      summary: Object.fromEntries(kernels.map(name => [name,
        {medianMs: 2, minMs: 1, maxMs: 3, meanMs: 2, stddevMs: Math.sqrt(2 / 3)}]))}};
}
const validFetch = async url => {
  if (String(url) === 'quickjs-manifest.json') return {ok: true, json: async () => ({schema: 'dynarmic.js/quickjs-build-v1', upstream: {version: 'test'}})};
  if (String(url) === 'wasm-build-manifest.json') return {ok: true, json: async () => ({schema: 'dynarmic.js/wasm-coremark-host-build-v1',
    source: {revision: 'test'}, build: {type: 'Release'}, artifacts: {js: {sha256: 'a'.repeat(64), bytes: 20}, wasm: {sha256: 'b'.repeat(64), bytes: 200}}})};
  return {ok: true, text: async () => 'expected\n', arrayBuffer: async () => new ArrayBuffer(4)};
};

function harness({fetch = validFetch, onPost, onConstruct, missingWorkload = false} = {}) {
  const elements = new Map(), listeners = new Map(), timers = new Map(), workers = [], requests = [];
  let timerId = 0;
  function element() {
    return {textContent: '', disabled: false, hidden: false, value: '', dataset: {}, style: {}, children: [],
      append(...nodes) { this.children.push(...nodes); },
      replaceChildren(...nodes) { this.children = nodes; },
      setAttribute(name, value) { this[name] = value; }};
  }
  const get = id => { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); };
  const document = {hidden: false, title: '', createElement: element, getElementById: get,
    querySelector: selector => get(selector),
    addEventListener(type, fn) { if (!listeners.has(type)) listeners.set(type, new Set()); listeners.get(type).add(fn); },
    removeEventListener(type, fn) { listeners.get(type)?.delete(fn); },
    dispatchEvent(event) { for (const fn of listeners.get(event.type) || []) fn(event); }};
  get('stop').disabled = get('quickjs-stop').disabled = true;
  get('quickjs-export').disabled = get('quickjs-results').hidden = true;
  get('quickjs-profile').value = 'light'; get('memory-mode').value = '2';
  get('quickjs-status').dataset.state = 'idle';
  class Worker {
    constructor(script) { onConstruct?.(script); this.script = script; this.terminated = false; workers.push(this); }
    postMessage(message) {
      requests.push({script: this.script, message});
      this.deliver = data => this.onmessage?.({data});
      if (onPost) return onPost(this, message);
      queueMicrotask(() => this.deliver(reply(message.runtime, message.profile)));
    }
    terminate() { this.terminated = true; }
  }
  const context = vm.createContext({document, fetch, Worker, URL, URLSearchParams, AbortController, AbortSignal,
    location: new URL('http://localhost:8000/nested/'), WebAssembly,
    navigator: {userAgent: 'test', hardwareConcurrency: 4}, crossOriginIsolated: false,
    performance: {timeOrigin: 0},
    CustomEvent: class {constructor(type, options) { this.type = type; this.detail = options.detail; }},
    setTimeout(callback, ms) { timers.set(++timerId, {callback, ms}); return timerId; },
    clearTimeout(id) { timers.delete(id); }});
  vm.runInContext('globalThis.window = globalThis;', context);
  if (!missingWorkload) vm.runInContext(workloadSource, context);
  vm.runInContext(source, context);
  return {context, get, document, workers, requests, timers, listeners,
    run: () => get('quickjs-run').onclick(), stop: () => get('quickjs-stop').onclick(),
    timeout: () => [...timers.values()].find(timer => timer.ms === 180000).callback()};
}
function ready(h, state) {
  assert.equal(h.get('quickjs-status').dataset.state, state, h.get('quickjs-status').textContent);
  for (const id of ['quickjs-run', 'quickjs-profile']) assert.equal(h.get(id).disabled, false, id);
  assert.equal(h.get('quickjs-stop').disabled, true);
  assert.equal(h.get('quickjs-export').disabled, state !== 'pass');
  assert.equal(h.get('quickjs-view-results').hidden, state !== 'pass');
  assert.equal(h.context.quickjsRunning, false);
  assert.equal(h.timers.size, 0, 'deadline must be released');
  assert.equal(h.listeners.get('visibilitychange').size, 0, 'visibility listener must be released');
  assert.ok(h.workers.every(worker => worker.terminated), 'all comparison Workers must be released');
}

test('QuickJS missing manifest, invalid JSON, missing workload and Worker script failure cannot pass', async () => {
  for (const fail of [async () => ({ok: false, status: 404}),
    async () => ({ok: true, json: async () => { throw new SyntaxError('Invalid JSON'); }}),
    async () => ({ok: true, json: async () => ({})}),
    async () => { throw new Error('offline'); }]) {
    let failing = true;
    const h = harness({fetch: (...args) => failing ? fail(...args) : validFetch(...args)});
    await h.run(); ready(h, 'fail'); assert.equal(h.workers.length, 0);
    failing = false; await h.run(); ready(h, 'pass');
  }
  const absent = harness({missingWorkload: true});
  await absent.run(); ready(absent, 'fail');
  assert.match(absent.get('quickjs-status').textContent, /quickjs-workloads.js is missing/);
  for (const kind of ['constructor', 'error', 'messageerror', 'post']) {
    let failing = true;
    const h = harness({onConstruct() { if (failing && kind === 'constructor') throw new Error('Worker script unavailable'); },
      onPost(worker, message) {
        if (!failing) return queueMicrotask(() => worker.deliver(reply(message.runtime)));
        if (kind === 'post') throw new Error('Could not transfer request');
        queueMicrotask(() => kind === 'error'
          ? worker.onerror({message: 'Script failed to load', preventDefault() {}}) : worker.onmessageerror());
      }});
    await h.run(); ready(h, 'fail');
    failing = false; await h.run(); ready(h, 'pass');
  }
});

test('QuickJS rejects wrong outputs, mismatched runtimes and malformed timing protocols', async () => {
  const cases = [() => null, () => ({ok: false, error: 'QuickJS guest fault'}),
    value => ({...value, ok: 'true'}),
    value => ({...value, runtime: 'unknown'}), value => ({...value, runMs: NaN}),
    value => ({...value, loadMs: -1}),
    value => ({...value, stats: null}), value => ({...value, stats: {blocks: Infinity}}),
    value => ({...value, memoryBytes: NaN}),
    value => ({...value, benchmark: {...value.benchmark, summary: {}}}),
    value => ({...value, benchmark: {...value.benchmark, summary: {...value.benchmark.summary,
      lcg: {...value.benchmark.summary.lcg, medianMs: 200}}}}),
    value => ({...value, benchmark: {...value.benchmark, summary: {...value.benchmark.summary,
      lcg: {...value.benchmark.summary.lcg, meanMs: 200}}}}),
    value => ({...value, benchmark: {...value.benchmark, summary: {...value.benchmark.summary,
      lcg: {...value.benchmark.summary.lcg, stddevMs: NaN}}}}),
    value => ({...value, benchmark: {...value.benchmark, measuredRuns: 0}}),
    value => ({...value, benchmark: {...value.benchmark, outputs: {lcg: 0, sieve: 1028, pi: 3141542}}}),
    value => ({...value, benchmark: {...value.benchmark, samplesMs: {lcg: [1, -1, 2], sieve: [1, 2, 3], pi: [1, 2, 3]}}}),
    value => ({...value, benchmark: {...value.benchmark, samplesMs: {lcg: [1, 2], sieve: [1, 2, 3], pi: [1, 2, 3]}}})];
  for (const corrupt of cases) {
    const h = harness({onPost(worker, message) { queueMicrotask(() => worker.deliver(corrupt(reply(message.runtime)))); }});
    await h.run(); ready(h, 'fail');
    assert.equal(h.context.quickjsReport, null, 'invalid measurements must not be exportable');
  }
});

test('QuickJS refuses missing or malformed host provenance and exports the actual host build record', async () => {
  for (const failure of [{ok: false, status: 404}, {ok: true, json: async () => ({})}]) {
    const h = harness({fetch: url => url === 'wasm-build-manifest.json' ? failure : validFetch(url)});
    await h.run(); ready(h, 'fail'); assert.equal(h.workers.length, 0);
  }
  const h = harness(); await h.run(); ready(h, 'pass');
  assert.equal(h.context.quickjsReport.dynarmicBuild.source.revision, 'test');
  assert.equal(h.context.quickjsReport.dynarmicBuild.artifacts.wasm.sha256, 'b'.repeat(64));
});

test('QuickJS cancellation and the whole-run deadline cover both fetch and synchronous execution', async () => {
  for (const stage of ['fetch', 'worker']) for (const cancel of [true, false]) {
    let hang = true, fetchAborted = false;
    const h = harness({fetch: (url, options) => hang && stage === 'fetch'
      ? new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => {
        fetchAborted = true; reject(options.signal.reason);
      }, {once: true})) : validFetch(url),
    onPost(worker, message) { if (!hang) queueMicrotask(() => worker.deliver(reply(message.runtime))); }});
    const running = h.run(); await tick();
    if (cancel) h.stop(); else h.timeout();
    await running; ready(h, cancel ? 'cancelled' : 'fail');
    if (!cancel) assert.match(h.get('quickjs-status').textContent, /timed out/);
    assert.equal(fetchAborted, stage === 'fetch');
    hang = false; await h.run(); ready(h, 'pass');
  }
});

test('cancelled QuickJS Workers cannot overwrite a retry or its completed results with queued progress', async () => {
  let hang = true;
  const h = harness({onPost(worker, message) { if (!hang) queueMicrotask(() => worker.deliver(reply(message.runtime))); }});
  const first = h.run(); await tick();
  const old = h.workers[0];
  h.stop(); await first; ready(h, 'cancelled');
  const second = h.run(); await tick();
  old.deliver({type: 'progress', runtime: 'dynarmic', phase: 'executing'});
  old.deliver(reply('dynarmic'));
  assert.equal(h.get('quickjs-status').dataset.state, 'running');
  h.stop(); await second; ready(h, 'cancelled');
  hang = false; await h.run(); ready(h, 'pass');
  old.deliver({type: 'progress', runtime: 'dynarmic', phase: 'loading'});
  ready(h, 'pass');
});

test('QuickJS respects both benchmark locks and releases its own lock on cancellation', async () => {
  const h = harness({onPost() {}});
  for (const name of ['cBenchRunning', 'officialBenchRunning']) {
    h.context[name] = true;
    await h.run();
    assert.equal(h.requests.length, 0, `${name} must prevent direct handler calls`);
    h.context[name] = false;
  }
  const pending = h.run(); await tick();
  assert.equal(h.context.quickjsRunning, true);
  await h.run();
  assert.equal(h.requests.length, 1, 'double clicks cannot start a second Worker');
  h.stop(); await pending; ready(h, 'cancelled');
  const retry = h.run(); await tick();
  assert.equal(h.requests.length, 2, 'cancellation releases the benchmark lock');
  h.stop(); await retry; ready(h, 'cancelled');
});
