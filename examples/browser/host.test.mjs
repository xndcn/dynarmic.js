// UI lifecycle checks. Real Chrome/Firefox checks exercise the Wasm Worker separately.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const pageSource = readFileSync(new URL('app.js', import.meta.url), 'utf8');
const reply = arch => ({ok: true, arch, text: 'expected\n', haltReason: 0x01000000,
  runMs: 2, kernelMs: [1, 1], loadMs: 1, stats: {blocks: 3, bytes: 40, compileMs: 1, instMs: 1}});
const okFetch = async () => ({ok: true, text: async () => 'expected\n', arrayBuffer: async () => new ArrayBuffer(4)});
const tick = () => new Promise(resolve => setImmediate(resolve));
class GoodWorker {
  postMessage({arch}) { queueMicrotask(() => this.onmessage({data: reply(arch)})); }
  terminate() {}
}

function pageHarness({fetch = okFetch, Worker = GoodWorker, search = '', protocol = 'http:', wasm = WebAssembly} = {}) {
  const elements = new Map(), timers = new Map(), listeners = new Map();
  let timerId = 0;
  function element() {
    let text = '';
    return {disabled: false, value: '2', className: '', children: [],
      get textContent() { return text; },
      set textContent(value) { text = value; this.children = []; },
      append(...nodes) { this.children.push(...nodes); }};
  }
  const document = {
    title: '', createElement: element,
    addEventListener(type, fn) { if (!listeners.has(type)) listeners.set(type, new Set()); listeners.get(type).add(fn); },
    dispatchEvent(event) { for (const fn of listeners.get(event.type) || []) fn(event); },
    getElementById(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); },
  };
  const location = new URL(protocol === 'file:' ? `file:///nested/index.html${search}` : `${protocol}//localhost:8000/nested/index.html${search}`);
  const context = vm.createContext({document, fetch, Worker, location, URL, URLSearchParams,
    AbortController, WebAssembly: wasm,
    CustomEvent: class {constructor(type, options) { this.type = type; this.detail = options.detail; }},
    setTimeout: (callback, ms) => { timers.set(++timerId, {callback, ms}); return timerId; },
    clearTimeout: id => timers.delete(id)});
  vm.runInContext(pageSource, context);
  return {document, elements, timers, run: () => vm.runInContext('run()', context),
    stop: () => elements.get('stop').onclick(),
    timeout: () => [...timers.values()].find(t => t.ms === 120000).callback()};
}

function assertReady(h, title) {
  assert.equal(h.document.title, title);
  assert.equal(h.elements.get('run').disabled, false);
  assert.equal(h.elements.get('stop').disabled, true);
  assert.equal(h.elements.get('memory-mode').disabled, false);
  assert.equal(h.timers.size, 0, 'timers are released');
}

test('runs both architectures in every memory mode and shows exact-reference results', async () => {
  for (const mode of [0, 1, 2]) {
    const requests = [];
    class Worker extends GoodWorker { postMessage(message, transfer) {
      requests.push({...message}); assert.equal(transfer[0], message.bin); super.postMessage(message);
    }}
    const h = pageHarness({Worker, search: `?mode=${mode}`});
    await h.run();
    assertReady(h, 'DONE PASS');
    assert.deepEqual(requests.map(r => [r.arch, r.mode]), [[0, mode], [1, mode]]);
    for (const arch of [0, 1]) {
      assert.match(h.elements.get('status' + arch).textContent, /^PASS/);
      assert.equal(h.elements.get('out' + arch).textContent, 'expected\n');
      assert.ok(h.elements.get('stats' + arch).children.length > 0);
    }
  }
});

test('HTTP failure, rejected fetch and empty reference fail cleanly and permit retry', async () => {
  for (const failure of [async () => { throw new Error('offline'); }, async () => ({ok: false, status: 404}),
    async () => ({ok: true, text: async () => ''})]) {
    let first = true;
    const h = pageHarness({fetch: (...args) => first ? failure(...args) : okFetch(...args)});
    await h.run();
    assertReady(h, 'DONE FAIL');
    assert.match(h.elements.get('status').textContent, /Could not run demo/);
    assert.doesNotMatch(h.elements.get('expected').textContent, /Loading/);
    first = false;
    await h.run(); assertReady(h, 'DONE PASS');
  }
});

test('startup, execution, unreadable-message and postMessage errors discard Workers and permit retries', async () => {
  for (const failure of ['startup', 'execution', 'message', 'post']) {
    let created = 0, terminated = 0;
    class Worker {
      constructor() { created++; if (failure === 'startup') throw new Error('Worker unavailable'); }
      postMessage() {
        if (failure === 'post') throw new Error('postMessage failed');
        queueMicrotask(() => failure === 'execution'
          ? this.onerror({message: 'Worker crashed', preventDefault() {}}) : this.onmessageerror());
      }
      terminate() { terminated++; }
    }
    const h = pageHarness({Worker});
    for (let i = 0; i < 2; i++) { await h.run(); assertReady(h, 'DONE FAIL'); }
    assert.equal(created, 2, failure);
    assert.equal(terminated, failure === 'startup' ? 0 : 2, failure);
  }
});

test('mismatch, unexpected halt, Worker error and malformed reply never pass', async () => {
  for (const badReply of [{...reply(0), text: 'wrong'}, {...reply(0), haltReason: 0x02000000},
    {arch: 0, ok: false, error: 'unsupported tail calls'}, null, {...reply(0), arch: 1}]) {
    class Worker extends GoodWorker { postMessage({arch}) { queueMicrotask(() => this.onmessage({data: arch === 0 ? badReply : reply(arch)})); } }
    const h = pageHarness({Worker}); await h.run(); assertReady(h, 'DONE FAIL');
  }
});

test('caught runtime failures discard poisoned modules before the next guest or rerun', async () => {
  for (const failedArch of [0, 1]) {
    const instances = [];
    class Worker extends GoodWorker {
      constructor() { super(); this.requests = []; instances.push(this); }
      postMessage({arch}) {
        this.requests.push(arch);
        // The real worker catches runtime aborts and sends ok:false; it does
        // not emit a browser error event that runInWorker could listen for.
        if (this === instances[0] && arch === failedArch) this.poisoned = true;
        queueMicrotask(() => this.onmessage({data: this.poisoned
          ? {ok: false, arch, error: 'RuntimeError: Aborted(memory access out of bounds)'} : reply(arch)}));
      }
      terminate() { this.terminated = true; }
    }
    const h = pageHarness({Worker});
    await h.run(); assertReady(h, 'DONE FAIL');
    assert.equal(instances[0].terminated, true, 'normal failure replies must discard the runtime');
    assert.deepEqual(instances[0].requests, failedArch === 0 ? [0] : [0, 1]);
    assert.match(h.elements.get('out' + failedArch).textContent, /RuntimeError/);
    if (failedArch === 0) assert.match(h.elements.get('status1').textContent, /^PASS/, 'the second guest uses a fresh module');

    await h.run(); assertReady(h, 'DONE PASS');
    assert.equal(instances.length, 2, 'the replacement runtime remains reusable after success');
  }
});

test('a rerun clears previous success and statistics before fetching', async () => {
  let fail = false;
  const h = pageHarness({fetch: (...args) => { if (fail) throw new Error('offline'); return okFetch(...args); }});
  await h.run(); assertReady(h, 'DONE PASS');
  fail = true; await h.run(); assertReady(h, 'DONE FAIL');
  for (const arch of [0, 1]) {
    assert.equal(h.elements.get('stats' + arch).children.length, 0);
    assert.doesNotMatch(h.elements.get('status' + arch).textContent, /PASS/);
    assert.doesNotMatch(h.elements.get('out' + arch).textContent, /expected/);
  }
});

test('Stop aborts a pending fetch, re-enables controls and allows a fresh run', async () => {
  let hang = true, aborted = false;
  const h = pageHarness({fetch: (_url, {signal}) => hang ? new Promise((_resolve, reject) => {
    signal.addEventListener('abort', () => { aborted = true; reject(signal.reason); }, {once: true});
  }) : okFetch()});
  const run = h.run(); h.stop(); await run;
  assert.equal(aborted, true); assertReady(h, 'DONE CANCELLED');
  hang = false; await h.run(); assertReady(h, 'DONE PASS');
});

test('Stop terminates a synchronous guest Worker, ignores its late replies and permits retry', async () => {
  const instances = [];
  class Worker extends GoodWorker {
    constructor() { super(); instances.push(this); }
    postMessage(message) { this.release = () => super.postMessage(message); }
    terminate() { this.terminated = true; }
  }
  const h = pageHarness({Worker});
  const first = h.run(); await tick();
  await h.run(); assert.equal(instances.length, 1, 'double click does not start another request');
  h.stop(); await first; assertReady(h, 'DONE CANCELLED');
  assert.equal(instances[0].terminated, true);
  const second = h.run(); await tick();
  instances[0].onmessage({data: null});
  await tick();
  assert.equal(h.document.title, 'Running demo guests', 'late reply cannot settle the new request');
  instances[1].release(); await tick(); // ARM32 completes; AArch64 is now pending.
  instances[1].release(); await second; assertReady(h, 'DONE PASS');
});

test('the whole run deadline covers stalled fetches and stalled Workers', async () => {
  for (const stage of ['fetch', 'worker']) {
    let terminated = false;
    class Worker { postMessage() {} terminate() { terminated = true; } }
    const h = pageHarness({Worker, fetch: stage === 'fetch' ? (_url, {signal}) => new Promise((_resolve, reject) => {
      signal.addEventListener('abort', () => reject(signal.reason), {once: true});
    }) : okFetch});
    const run = h.run(); await tick(); h.timeout(); await run;
    assertReady(h, 'DONE FAIL');
    assert.match(h.elements.get('status').textContent, /timed out after 120 seconds/);
    assert.equal(terminated, stage === 'worker');
  }
});

test('file URLs and missing browser capabilities fail with actionable feedback', async () => {
  for (const options of [{protocol: 'file:'}, {Worker: null}, {wasm: undefined}]) {
    // undefined would select the harness default, so pass a primitive unavailable value.
    const h = pageHarness({...options, ...(Object.hasOwn(options, 'wasm') ? {wasm: false} : {})});
    await h.run(); assertReady(h, 'DONE FAIL');
    assert.match(h.elements.get('status').textContent, /HTTP|Web Workers|WebAssembly/);
  }
});

test('reports failures to a same-origin smoke driver, preserves query and refuses foreign callbacks', async () => {
  for (const report of ['/result?token=example', 'https://another.example/result']) {
    const requests = [];
    const h = pageHarness({search: `?report=${encodeURIComponent(report)}`, fetch: async url => {
      requests.push(String(url)); if (String(url) === 'expected.txt') throw new Error('offline'); return {ok: true};
    }});
    await h.run(); assertReady(h, 'DONE FAIL');
    if (report.startsWith('/')) {
      assert.equal(requests.length, 2);
      const url = new URL(requests[1]); assert.equal(url.searchParams.get('token'), 'example');
      assert.match(url.searchParams.get('status'), /^FAIL\|/);
    } else assert.equal(requests.length, 1);
  }
});
