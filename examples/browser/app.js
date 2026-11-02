// Dependency-free UI. Serve the complete dist/ directory from any HTTP(S) server.
const ARCHES = [{name: 'ARM32', bin: 'demo.bin'}, {name: 'AArch64', bin: 'demo_a64.bin'}];
const KERNELS = ['lcg', 'sieve', 'pi'];
const RUN_TIMEOUT_MS = 120000;
const el = id => document.getElementById(id);
let worker = null;
let pending = null;
let activeRun = null;

function setStatus(message, kind = '') {
  el('status').textContent = message;
  el('status').className = `status ${kind}`;
}

function badge(arch, message, kind = '') {
  el('status' + arch).textContent = message;
  el('status' + arch).className = `badge ${kind}`;
}

function hostProblem() {
  if (location.protocol === 'file:') return 'Serve the built dist/ directory over HTTP(S), for example: python3 -m http.server 8000 --directory build-em/examples/browser/dist. Then open http://localhost:8000/.';
  if (typeof Worker !== 'function') return 'Web Workers are unavailable. Use a current browser with Web Workers enabled.';
  if (typeof WebAssembly !== 'object') return 'WebAssembly is unavailable. Use a current browser with WebAssembly enabled.';
  return '';
}

function discardWorker() {
  worker?.terminate();
  worker = null;
}

function runInWorker(message, signal) {
  signal.throwIfAborted();
  if (pending) return Promise.reject(new Error('A guest is already running.'));
  return new Promise((resolve, reject) => {
    const finish = (error, result) => {
      signal.removeEventListener('abort', abort);
      pending = null;
      if (error) { discardWorker(); reject(error); }
      else resolve(result);
    };
    const abort = () => finish(signal.reason);
    try {
      if (!worker) {
        const instance = new Worker('worker.js');
        worker = instance;
        // A terminated Worker's queued event must never settle a later run.
        instance.onmessage = ({data}) => { if (worker === instance) pending?.(null, data); };
        instance.onerror = event => {
          event.preventDefault();
          if (worker === instance) pending?.(new Error(event.message || 'Worker failed. Check that arm_jit.js and arm_jit.wasm are served from the same build.'));
        };
        instance.onmessageerror = () => { if (worker === instance) pending?.(new Error('Worker returned an unreadable message.')); };
      }
      pending = finish;
      signal.addEventListener('abort', abort, {once: true});
      worker.postMessage(message, [message.bin]);
    } catch (error) { finish(error); }
  });
}

async function fetchFile(path, signal) {
  const response = await fetch(path, {signal, cache: 'no-store'});
  if (!response.ok) throw new Error(`Could not load ${path}: HTTP ${response.status}. Build the browser_page target and serve its complete dist/ directory.`);
  return response;
}

function addStat(dl, name, value) {
  const dt = document.createElement('dt'); dt.textContent = name;
  const dd = document.createElement('dd'); dd.textContent = value;
  dl.append(dt, dd);
}

function show(arch, result, expected) {
  if (!result || result.arch !== arch || typeof result.ok !== 'boolean') throw new Error('Worker returned an invalid response.');
  const output = el('out' + arch), stats = el('stats' + arch);
  if (!result.ok) {
    // A caught Wasm trap or Emscripten abort is delivered as a normal message.
    // Its runtime may be unusable; the next guest must load a fresh module.
    discardWorker();
    output.textContent = result.error || 'Worker failed without an error message.';
    badge(arch, 'Worker error', 'fail');
    return false;
  }
  if (typeof result.text !== 'string' || !Number.isFinite(result.runMs) || !Number.isFinite(result.loadMs) || !Array.isArray(result.kernelMs) || !result.kernelMs.every(Number.isFinite)) throw new Error('Worker returned invalid output or timing data.');
  output.textContent = result.text;
  const pass = result.text === expected && result.haltReason === 0x01000000;
  badge(arch, pass ? 'PASS · matches reference' : 'FAIL · differs from reference', pass ? 'pass' : 'fail');
  addStat(stats, 'Guest run', `${result.runMs.toFixed(1)} ms`);
  addStat(stats, 'Per kernel', result.kernelMs.map((ms, i) => `${KERNELS[i] || `k${i}`} ${ms.toFixed(1)} ms`).join(', ') || 'No timing marks');
  if (arch === 0) addStat(stats, 'Module initialization', `${result.loadMs.toFixed(1)} ms (reused between runs)`);
  const s = result.stats;
  if (s && s.blocks !== undefined) {
    addStat(stats, 'Compiled blocks', `${s.blocks} (${s.bytes} bytes of Wasm)`);
    addStat(stats, 'Engine compile + instantiate', `${(s.compileMs + s.instMs).toFixed(1)} ms`);
    addStat(stats, 'Translate / optimize / emit', [s.translateMs, s.optMs, s.emitMs].map(v => (v || 0).toFixed(1)).join(' / ') + ' ms');
  }
  if (s && s.profile_compiledBlocks !== undefined) addStat(stats, 'Profile', `dispatch ${s.profile_dispatchCount || 0}, cache ${s.profile_dispatchCacheHits || 0}/${s.profile_dispatchCacheMisses || 0}, regions ${s.profile_compiledRegions || 0}, bytes ${s.profile_emittedBytes || 0}`);
  return pass;
}

// Kept for the Firefox smoke runner; only report to this page's own origin.
async function reportResult(result) {
  const report = new URLSearchParams(location.search).get('report');
  if (!report) return;
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 5000);
  try {
    const url = new URL(report, location.href);
    if (url.origin !== location.origin) return;
    url.searchParams.set('status', [result, el('status').textContent, ...[0, 1].map(i => el('status' + i).textContent), ...[0, 1].map(i => el('out' + i).textContent.slice(0, 500))].join('|'));
    await fetch(url, {cache: 'no-store', signal: controller.signal});
  } catch { /* A smoke callback must not change the visible guest result. */ }
  finally { clearTimeout(timer); }
}

async function run() {
  if (activeRun || globalThis.quickjsRunning || globalThis.officialBenchRunning || globalThis.cBenchRunning) return;
  const controller = new AbortController();
  activeRun = controller;
  document.dispatchEvent?.(new CustomEvent('dynarmic-demo-state', {detail: {running: true}}));
  const signal = controller.signal;
  let cancelled = false;
  let currentArch = null;
  let outcome = 'FAIL';
  const timer = setTimeout(() => controller.abort(new Error('Run timed out after 120 seconds. The Worker was stopped. Retry or check the browser console and network.')), RUN_TIMEOUT_MS);
  el('run').disabled = true; el('stop').disabled = false; el('memory-mode').disabled = true;
  el('stop').onclick = () => { cancelled = true; controller.abort(new Error('Run stopped. You can run the guests again.')); discardWorker(); };
  document.title = 'Running demo guests';
  el('expected').textContent = 'Loading native reference…';
  for (let arch = 0; arch < ARCHES.length; arch++) {
    badge(arch, 'Waiting'); el('out' + arch).textContent = 'Waiting for guest…'; el('stats' + arch).textContent = '';
  }
  try {
    const problem = hostProblem();
    if (problem) throw new Error(problem);
    setStatus('Loading native reference…', 'running');
    const expected = await (await fetchFile('expected.txt', signal)).text();
    signal.throwIfAborted();
    if (!expected.trim()) throw new Error('expected.txt is empty. Rebuild browser_page to generate the native reference.');
    el('expected').textContent = expected;
    const mode = Number(el('memory-mode').value);
    let allPass = true;
    for (let arch = 0; arch < ARCHES.length; arch++) {
      currentArch = arch;
      setStatus(`Running ${ARCHES[arch].name} (${arch + 1} of ${ARCHES.length}) — loading the guest and executing in a Worker…`, 'running');
      badge(arch, 'Running'); el('out' + arch).textContent = 'Executing guest…';
      const bin = await (await fetchFile(ARCHES[arch].bin, signal)).arrayBuffer();
      signal.throwIfAborted();
      const result = await runInWorker({bin, mode, arch}, signal);
      signal.throwIfAborted();
      allPass = show(arch, result, expected) && allPass;
      currentArch = null;
    }
    outcome = allPass ? 'PASS' : 'FAIL';
    setStatus(allPass ? 'PASS — both guests match the native reference.' : 'FAIL — a guest failed or its output differs. Inspect the guest output and native reference below.', allPass ? 'pass' : 'fail');
  } catch (error) {
    outcome = cancelled ? 'CANCELLED' : 'FAIL';
    const reason = signal.aborted ? signal.reason : error;
    setStatus(cancelled ? reason.message : `Could not run demo: ${reason.message || reason}`, cancelled ? '' : 'fail');
    discardWorker();
    for (let arch = 0; arch < ARCHES.length; arch++) {
      if (arch === currentArch || el('status' + arch).textContent === 'Waiting') {
        badge(arch, cancelled ? 'Stopped' : (arch === currentArch ? 'Failed' : 'Not run'), cancelled ? '' : 'fail');
        el('out' + arch).textContent = cancelled ? 'Run stopped.' : (arch === currentArch ? String(reason.message || reason) : 'Guest was not run.');
      }
    }
    if (el('expected').textContent === 'Loading native reference…') el('expected').textContent = 'Native reference was not loaded.';
  } finally {
    clearTimeout(timer); activeRun = null;
    el('run').disabled = false; el('stop').disabled = true; el('memory-mode').disabled = false;
    document.dispatchEvent?.(new CustomEvent('dynarmic-demo-state', {detail: {running: false}}));
  }
  document.title = 'DONE ' + outcome;
  await reportResult(outcome);
}

el('run').onclick = run;
el('run').disabled = false;
const query = new URLSearchParams(location.search);
if (['0', '1', '2'].includes(query.get('mode'))) el('memory-mode').value = query.get('mode');
const problem = hostProblem();
if (problem) setStatus(problem, 'fail');
if (query.has('auto')) run();

document.addEventListener("official-bench-state", event => {
  el("run").disabled = event.detail.running || !!activeRun || !!globalThis.quickjsRunning || !!globalThis.cBenchRunning;
  el("memory-mode").disabled = event.detail.running || !!activeRun || !!globalThis.quickjsRunning || !!globalThis.cBenchRunning;
});
document.addEventListener('c-bench-state', () => {
  const busy = !!globalThis.cBenchRunning || !!activeRun || !!globalThis.quickjsRunning || !!globalThis.officialBenchRunning;
  el('run').disabled = busy; el('memory-mode').disabled = busy;
});
