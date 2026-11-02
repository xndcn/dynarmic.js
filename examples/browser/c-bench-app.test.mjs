// UI fault injection uses the production protocol; real browser tests execute C.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const app = readFileSync(new URL('c-bench-app.js', import.meta.url), 'utf8');
const protocol = readFileSync(new URL('c-bench-protocol.js', import.meta.url), 'utf8');
const ids = ['integer', 'branch', 'stream', 'pointer', 'float'];
const tick = () => new Promise(resolve => setImmediate(resolve));
const reference = {schema: 'dynarmic.js/c-bench-reference-v1', cases: ids.map((id, index) => ({id, index, title: id})),
  profiles: Object.fromEntries(['quick', 'full'].map(profile => [profile, {seed: 305419896,
    iterations: Object.fromEntries(ids.map(id => [id, profile === 'quick' ? 1000 : 10000])),
    expected: Object.fromEntries(ids.map((id, index) => [id, index + 100]))}])),
  protocol: {firstRuns: 1, warmupRuns: 2, measuredRuns: 3, totalRuns: 6, rounds: 3, guestRamBytes: 16777216, mailboxAddress: 61440, magic: 0x43424e31}};
const artifact = path => ({path, bytes: 100, sha256: 'a'.repeat(64)});
const build = {schema: 'dynarmic.js/c-bench-build-v1', compiler: {version: 'test compiler'}, commonFlags: ['-O2'],
  targets: {arm: {}, x86: {}, wasm: {}}, sources: [artifact('bench.c')], artifacts: [
    'c-bench-arm.bin', 'c-bench-x86.elf', 'c-bench.wasm', 'c-bench-reference.json', 'c-bench-source.c',
    'c-bench-v86.js', 'c-bench-v86.wasm', 'c-bench-v86-LICENSE.txt'].map(artifact)};
const host = {schema: 'dynarmic.js/wasm-coremark-host-build-v1', source: {revision: 'test revision'}, build: {type: 'Release', profile: false}, configuration: {regionDiagnostics: false},
  artifacts: {js: artifact('arm_jit.js'), wasm: artifact('arm_jit.wasm')}};
function reply(task) {
  const expected = reference.profiles[task.profile].expected[task.caseId];
  const ms = {dynarmic: [1, 10, 1], v86: [2, 10, 10], wasm: [.5, .5, .5]}[task.runtime][task.round];
  return {ok: true, ...task, iterations: reference.profiles[task.profile].iterations[task.caseId], seed: 305419896,
    checksums: Array(6).fill(expected), expected, validation: {pass: true, actual: expected, expected},
    loadMs: 17, firstMs: 40, warmupMs: [30, 20], samplesMs: [.8 * ms, ms, 1.2 * ms], stats: {}, memoryBytes: 16777216};
}
function harness({onPost, fetch, search = ''} = {}) {
  const elements = new Map(), listeners = new Map(), timers = new Map(), workers = [], fetches = [];
  let timerId = 0;
  const element = () => ({textContent: '', disabled: false, value: '', dataset: {}, style: {}, children: [],
    append(...nodes) { this.children.push(...nodes); }, replaceChildren(...nodes) { this.children = nodes; },
    setAttribute(name, value) { this[name] = value; }, click() {}});
  const get = id => { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); };
  const document = {hidden: false, getElementById: get, createElement: element,
    addEventListener(type, fn) { if (!listeners.has(type)) listeners.set(type, new Set()); listeners.get(type).add(fn); },
    removeEventListener(type, fn) { listeners.get(type)?.delete(fn); },
    dispatchEvent(event) { for (const fn of listeners.get(event.type) || []) fn(event); }};
  get('stop').disabled = true; get('c-bench-profile').value = 'quick'; get('c-bench-case').value = 'all';
  const defaultFetch = async name => ({ok: true, json: async () => structuredClone({
    'c-bench-reference.json': reference, 'c-bench-manifest.json': build, 'wasm-build-manifest.json': host}[String(name)])});
  class Worker {
    constructor(url) { this.url = url; workers.push(this); }
    postMessage(message) {
      this.message = message; this.deliver = value => this.onmessage?.({data: value});
      if (onPost) return onPost(this, message);
      queueMicrotask(() => this.deliver(reply(message)));
    }
    terminate() { this.terminated = true; }
  }
  const context = vm.createContext({document, Worker, fetch: (url, options) => {fetches.push({url, options}); return (fetch || defaultFetch)(url, options);},
    URL, URLSearchParams, AbortController, AbortSignal, Blob,
    location: new URL('http://localhost:8000/nested/' + search), navigator: {userAgent: 'test browser', hardwareConcurrency: 4},
    CustomEvent: class {constructor(type, options) {this.type = type; this.detail = options.detail;}},
    setTimeout(callback, ms) {timers.set(++timerId, {callback, ms}); return timerId;}, clearTimeout(id) {timers.delete(id);}});
  vm.runInContext('globalThis.window = globalThis', context);
  vm.runInContext(protocol, context); vm.runInContext(app, context);
  return {context, document, get, workers, timers, listeners, fetches, defaultFetch,
    run: () => get('c-bench-run').onclick(), stop: () => get('c-bench-stop').onclick(),
    timeout: ms => { const timer = [...timers.values()].find(item => item.ms === ms); assert(timer); timer.callback(); }};
}
function released(h, state) {
  assert.equal(h.get('c-bench-status').dataset.state, state, h.get('c-bench-status').textContent);
  assert.equal(h.context.cBenchRunning, false);
  assert.equal(h.get('c-bench-stop').disabled, true);
  assert.equal(h.get('c-bench-run').disabled, false);
  assert.equal(h.get('c-bench-export').disabled, false, 'Partial evidence remains exportable');
  assert.equal(h.timers.size, 0, 'All deadlines released');
  assert.equal(h.listeners.get('visibilitychange')?.size, 0);
  assert(h.workers.every(worker => worker.terminated), 'Every Worker terminated');
}

test('fixed-work comparison uses 45 fresh Workers, rotates each path and pairs round timings', async () => {
  const h = harness(); await h.run(); released(h, 'pass');
  const r = h.context.cBenchReport;
  assert.equal(h.workers.length, 45);
  assert.equal(new Set(h.workers.map(worker => worker.message.token)).size, 45);
  assert(h.workers.every(worker => worker.url === 'c-bench-worker.js'));
  assert.equal(r.summary.cases.integer.runtimes.dynarmic.timing.median, 1);
  assert.equal(r.summary.cases.integer.runtimes.v86.timing.median, 10);
  assert.equal(r.summary.cases.integer.v86OverArm.median, 2, 'Must not divide unpaired time medians');
  assert(Math.abs(r.summary.v86OverArm.median - 2) < 1e-12);
  assert.equal(r.observations[0].result.firstMs, 40);
  assert.equal(r.observations[0].result.loadMs, 17);
  assert.equal(r.performanceValid, true);
  assert.equal(r.dynarmicBuild.source.revision, 'test revision');
  for (const caseId of ids) {
    for (const runtime of ['dynarmic', 'v86', 'wasm']) {
      const positions = r.observations.filter(entry => entry.task.caseId === caseId && entry.task.runtime === runtime).map(entry => entry.task.position);
      assert.equal(new Set(positions).size, 3, 'Every path should occupy every order position for each case');
    }
  }
});

test('a wrong checksum or task token cannot pass, while low-resolution samples retain correctness without ratios', async () => {
  for (const corrupt of [result => {result.checksums[5]++;}, result => {result.token = 'old-run';}]) {
    const h = harness({onPost(worker, task) {
      const result = reply(task); if (task.caseId === 'integer' && task.round === 0 && task.runtime === 'dynarmic') corrupt(result);
      queueMicrotask(() => worker.deliver(result));
    }});
    await h.run(); released(h, 'fail');
    assert.equal(h.context.cBenchReport.observations.filter(entry => entry.status === 'fail').length, 1);
    assert.equal(h.context.cBenchReport.summary.v86OverArm, null);
    assert.equal(h.context.cBenchReport.summary.cases.integer.v86OverArm, null);
    assert.equal(h.context.cBenchReport.verified, false);
  }
  const h = harness({onPost(worker, task) {
    const result = reply(task); if (task.runtime === 'wasm') result.samplesMs = [0, .02, .04];
    queueMicrotask(() => worker.deliver(result));
  }});
  h.get('c-bench-case').value = 'float'; await h.run(); released(h, 'pass');
  assert.equal(h.context.cBenchReport.summary.cases.float.runtimes.wasm.belowResolution, true);
  assert.equal(h.context.cBenchReport.summary.armOverDirect, null);
  assert.equal(h.context.cBenchReport.performanceValid, false);
  assert(h.context.cBenchReport.summary.v86OverArm);
});

test('Stop and hidden-tab cancellation cover loading and execution, preserve partial evidence and ignore stale Workers', async () => {
  for (const stage of ['fetch', 'worker']) for (const hidden of [false, true]) {
    let hanging = true, abortedFetch = false, h;
    h = harness({fetch: (name, options) => hanging && stage === 'fetch'
      ? new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => {abortedFetch = true; reject(options.signal.reason);}, {once: true}))
      : h.defaultFetch(name), onPost(worker, task) {if (!hanging) queueMicrotask(() => worker.deliver(reply(task)));}});
    const run = h.run(); await tick(); const stale = h.workers[0];
    if (hidden) {h.document.hidden = true; h.document.dispatchEvent({type: 'visibilitychange'});} else h.stop();
    await run; released(h, hidden ? 'invalid' : 'cancelled');
    assert.equal(abortedFetch, stage === 'fetch');
    assert.equal(h.context.cBenchReport.performanceValid, false);
    assert.equal(h.context.cBenchReport.summary.v86OverArm, null);
    hanging = false; h.document.hidden = false; h.get('c-bench-case').value = 'integer';
    await h.run(); released(h, 'pass');
    if (stale) {stale.deliver(reply(stale.message)); stale.deliver({type: 'progress', ...stale.message, phase: 'executing'});}
    released(h, 'pass');
  }
  const h = harness(); h.document.hidden = true; await h.run(); released(h, 'invalid');
  assert.equal(h.workers.length, 0); assert.equal(h.fetches.length, 0);
});

test('per-observation and whole-run deadlines release resources and existing labs exclude direct handler calls', async () => {
  const h = harness({onPost(worker, task) {
    if (task.round || task.runtime !== 'dynarmic') queueMicrotask(() => worker.deliver(reply(task)));
  }});
  h.get('c-bench-case').value = 'integer';
  for (const global of ['quickjsRunning', 'officialBenchRunning']) {
    h.context[global] = true; await h.run(); assert.equal(h.workers.length, 0); h.context[global] = false;
  }
  const run = h.run(); await tick(); await h.run(); assert.equal(h.workers.length, 1);
  h.timeout(90000); await run; released(h, 'fail');
  assert.equal(h.context.cBenchReport.observations.filter(entry => entry.status === 'pass').length, 8);
  const fetchH = harness({fetch: (_url, {signal}) => new Promise((_resolve, reject) => signal.addEventListener('abort', () => reject(signal.reason), {once: true}))});
  const pending = fetchH.run(); await tick(); fetchH.timeout(600000); await pending; released(fetchH, 'fail');
  assert.match(fetchH.context.cBenchReport.error, /timed out/);
});

test('missing provenance never starts a Worker; auto-run reporting stays on the same origin', async () => {
  const bad = harness({fetch: async () => ({ok: false, status: 404})});
  await bad.run(); released(bad, 'fail'); assert.equal(bad.workers.length, 0);
  for (const target of ['/callback', 'https://other.example/callback']) {
    const h = harness({search: '?cbench=quick&cbench-report=' + encodeURIComponent(target)});
    await tick(); released(h, 'pass');
    const callbacks = h.fetches.filter(entry => entry.url instanceof URL);
    assert.equal(callbacks.length, target === '/callback' ? 1 : 0);
    if (callbacks.length) assert.match(callbacks[0].url.searchParams.get('status'), /^PASS\|/);
  }
});

test('instrumented hosts retain correctness evidence but cannot publish performance ratios', async () => {
  for (const field of ['profile', 'regionDiagnostics']) {
    const record = structuredClone(host);
    (field === 'profile' ? record.build : record.configuration)[field] = true;
    let h;
    h = harness({fetch: name => String(name) === 'wasm-build-manifest.json' ? Promise.resolve({ok: true, json: async () => record}) : h.defaultFetch(name)});
    h.get('c-bench-case').value = 'integer'; await h.run(); released(h, 'pass');
    const r = h.context.cBenchReport;
    assert.equal(r.verified, true);
    assert.equal(r.observations.filter(entry => entry.status === 'pass').length, 9);
    assert.equal(r.performanceValid, false);
    assert.equal(r.methodology.performanceEligible, false);
    assert.equal(r.methodology.instrumentation[field], true);
    assert.equal(r.summary.v86OverArm, null); assert.equal(r.summary.armOverDirect, null);
    assert.equal(r.summary.cases.integer.v86OverArm, null); assert.equal(r.summary.cases.integer.armOverDirect, null);
    assert(r.summary.cases.integer.runtimes.dynarmic.timing, 'Diagnostic timings remain inspectable');
    assert.match(h.get('c-bench-status').textContent, /instrumented.*unavailable/);
  }
  for (const value of [undefined, 'false', 0]) {
    const record = structuredClone(host); record.build.profile = value;
    let h;
    h = harness({fetch: name => String(name) === 'wasm-build-manifest.json' ? Promise.resolve({ok: true, json: async () => record}) : h.defaultFetch(name)});
    await h.run(); released(h, 'fail'); assert.equal(h.workers.length, 0);
  }
});
