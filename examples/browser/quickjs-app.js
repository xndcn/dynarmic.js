// Local, auditable measurements. All expensive work lives in disposable Workers.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const paths = {dynarmic: 'QuickJS on ARM', wasm: 'QuickJS on Wasm', native: 'Browser JavaScript'};
  const kernels = ['lcg', 'sieve', 'pi'];
  let active = null, runNumber = 0;
  const node = (tag, text, className) => {
    const element = document.createElement(tag);
    if (text !== undefined) element.textContent = text;
    if (className) element.className = className;
    return element;
  };
  const ms = value => value === 0 ? '≈0 ms' : value < 0.01 ? '<0.01 ms' : `${value.toFixed(2)} ms`;
  const median = samples => [...samples].sort((a, b) => a - b)[1];
  const numeric = value => typeof value === 'number' && Number.isFinite(value) && value >= 0;
  function status(text, state) {
    $('quickjs-status').textContent = text;
    $('quickjs-status').dataset.state = state;
    $('quickjs-view-results').hidden = state !== 'pass';
    $('quickjs-status').className = `status ${state === 'running' ? 'running' : state === 'pass' ? 'pass' : state === 'fail' ? 'fail' : ''}`;
  }
  function badge(runtime, text, state = '') {
    $(`q-badge-${runtime}`).textContent = text;
    $(`q-badge-${runtime}`).className = `badge ${state}`;
  }
  function execute(runtime, profile, signal) {
    signal.throwIfAborted();
    return new Promise((resolve, reject) => {
      const worker = new Worker('quickjs-worker.js');
      let settled = false;
      const finish = (error, result) => {
        if (settled) return;
        settled = true;
        signal.removeEventListener('abort', abort);
        worker.terminate();
        error ? reject(error) : resolve(result);
      };
      const abort = () => finish(signal.reason);
      signal.addEventListener('abort', abort, {once: true});
      worker.onmessage = ({data}) => {
        if (settled) return;
        if (data?.type === 'progress') {
          const label = data.phase === 'loading' ? 'Loading' : 'Executing';
          badge(runtime, label, 'running');
          status(`${label} ${paths[runtime]}… Other paths wait so their work does not overlap. Stop is always available.`, 'running');
        } else if (!data?.ok) finish(new Error(data?.error || `${paths[runtime]} returned an invalid response.`));
        else finish(null, data);
      };
      worker.onerror = event => { event.preventDefault(); finish(new Error(event.message || `Could not start ${paths[runtime]}. Check that all built assets are served.`)); };
      worker.onmessageerror = () => finish(new Error('The Worker reply could not be read.'));
      try { worker.postMessage({runtime, profile}); } catch (error) { finish(error); }
    });
  }
  function validate(result, runtime, profile) {
    if (result.ok !== true || result.runtime !== runtime || !numeric(result.loadMs) || !numeric(result.runMs)) throw new Error('Invalid runtime measurement.');
    if (!result.stats || typeof result.stats !== 'object' || Array.isArray(result.stats) || !Object.values(result.stats).every(numeric)) throw new Error('Invalid runtime statistics.');
    if (runtime === 'native' ? result.memoryBytes !== null : !Number.isSafeInteger(result.memoryBytes) || result.memoryBytes <= 0) throw new Error('Invalid runtime memory measurement.');
    const b = result.benchmark;
    if (b?.profile !== profile || b.warmupRuns !== 1 || b.measuredRuns !== 3) throw new Error('Unexpected sample protocol.');
    for (const kernel of kernels) {
      if (b.parameters?.[kernel] !== QuickJSBench.profiles[profile][kernel] || b.outputs?.[kernel] !== QuickJSBench.expected[profile][kernel]) throw new Error(`${paths[runtime]}: ${kernel} did not match the reference.`);
      if (!numeric(b.coldMs?.[kernel]) || !Array.isArray(b.samplesMs?.[kernel]) || b.samplesMs[kernel].length !== 3 || !b.samplesMs[kernel].every(numeric)) throw new Error('Invalid kernel timing samples.');
      const samples = b.samplesMs[kernel], summary = b.summary?.[kernel];
      if (!summary || !['medianMs', 'minMs', 'maxMs', 'meanMs', 'stddevMs'].every(key => numeric(summary[key])) ||
          summary.medianMs !== median(samples) || summary.minMs !== Math.min(...samples) || summary.maxMs !== Math.max(...samples)) throw new Error('Invalid kernel timing summary.');
      const mean = samples.reduce((total, value) => total + value, 0) / samples.length;
      const deviation = Math.sqrt(samples.reduce((total, value) => total + (value - mean) ** 2, 0) / samples.length);
      // Math.sqrt/libm may differ by an ulp across the two interpreter builds.
      if (Math.abs(summary.meanMs - mean) > Math.max(1, mean) * 1e-9 ||
          Math.abs(summary.stddevMs - deviation) > Math.max(1, deviation) * 1e-9) throw new Error('Kernel statistics do not match their samples.');
    }
  }
  function stat(dl, label, value) { dl.append(node('dt', label), node('dd', value)); }
  function render(report) {
    const charts = $('q-charts'), details = $('q-runtime-details');
    charts.replaceChildren(); details.replaceChildren();
    for (const kernel of kernels) {
      const chart = node('section', undefined, 'q-chart');
      chart.append(node('h4', {lcg: '01 / LCG', sieve: '02 / Prime sieve', pi: '03 / Pi approximation'}[kernel]));
      chart.append(node('p', `${report.parameters[kernel].toLocaleString('en-US')} ${kernel === 'sieve' ? 'entries' : 'iterations'}`));
      const maximum = Math.max(...Object.keys(paths).map(runtime => median(report.runtimes[runtime].benchmark.samplesMs[kernel])));
      for (const [runtime, name] of Object.entries(paths)) {
        const samples = report.runtimes[runtime].benchmark.samplesMs[kernel], middle = median(samples);
        const row = node('div', undefined, 'q-bar-row'), label = node('div', undefined, 'q-bar-label');
        label.append(node('span', name), node('strong', ms(middle)));
        const track = node('div', undefined, 'q-bar-track'), bar = node('div', undefined, 'q-bar');
        bar.dataset.runtime = runtime; bar.style.width = `${maximum ? middle / maximum * 100 : 0}%`;
        track.setAttribute('aria-hidden', 'true'); track.append(bar);
        row.append(label, track, node('div', `${ms(Math.min(...samples))} – ${ms(Math.max(...samples))}`, 'q-bar-range'));
        chart.append(row);
      }
      charts.append(chart);
    }
    for (const [runtime, name] of Object.entries(paths)) {
      const result = report.runtimes[runtime], b = result.benchmark;
      const item = node('details'), dl = node('dl');
      item.append(node('summary', `${name} · output & statistics`));
      item.append(node('pre', kernels.map(k => `${k === 'pi' ? 'pi×1e6' : k} = ${b.outputs[k]}`).join('\n')));
      stat(dl, 'Prepare / load', ms(result.loadMs));
      stat(dl, 'Complete benchmark call', ms(result.runMs));
      stat(dl, 'Timing boundary', report.methodology.timingBoundaries[runtime === 'native' ? 'browser' : 'quickjs']);
      for (const kernel of kernels) {
        stat(dl, `${kernel} · first pass`, ms(b.coldMs[kernel]));
        stat(dl, `${kernel} · all repeated samples`, b.samplesMs[kernel].map(ms).join(' / '));
      }
      stat(dl, 'Wasm linear memory', result.memoryBytes === null ? 'Not applicable; JS heap is not measured' : `${(result.memoryBytes / 1048576).toFixed(1)} MiB (capacity, not peak use)`);
      if (runtime === 'dynarmic') {
        const s = result.stats;
        stat(dl, 'JIT compiled blocks', String(s.blocks ?? 'Unavailable'));
        stat(dl, 'Generated Wasm', s.bytes === undefined ? 'Unavailable' : `${s.bytes.toLocaleString('en-US')} bytes`);
        stat(dl, 'Translate / optimize / emit', [s.translateMs, s.optMs, s.emitMs].map(v => numeric(v) ? ms(v) : 'Unavailable').join(' / '));
        stat(dl, 'Engine compile / instantiate', [s.compileMs, s.instMs].map(v => numeric(v) ? ms(v) : 'Unavailable').join(' / '));
        stat(dl, 'JIT scope', 'Whole script, including setup and warmup; overlaps execution timings.');
      } else stat(dl, 'dynarmic.js JIT statistics', 'Not applicable to this path');
      item.append(dl); details.append(item);
    }
    $('q-measurement').textContent = `Repeated-pass median and min–max · 3 samples per kernel · lower is faster. Each chart uses its own linear scale. ≈0 means below timer resolution. QuickJS ${report.build.upstream?.version || 'pinned build'}.`;
    $('quickjs-results').hidden = false;
  }
  async function reportSmoke(outcome) {
    const path = new URLSearchParams(location.search).get('quickjs-report');
    if (!path) return;
    try {
      const url = new URL(path, location.href);
      if (url.origin !== location.origin) return;
      url.searchParams.set('status', `${outcome}|${$('quickjs-status').textContent}`);
      await fetch(url, {signal: AbortSignal.timeout(5000), cache: 'no-store'});
    } catch { /* A test callback does not alter the visible result. */ }
  }
  async function run() {
    if (active || globalThis.officialBenchRunning || globalThis.cBenchRunning) return;
    const controller = new AbortController(), {signal} = controller;
    active = controller; window.quickjsRunning = true;
    document.dispatchEvent(new CustomEvent('quickjs-comparison-state', {detail: {running: true}})); window.quickjsReport = null;
    let cancelled = false, current = null, outcome = 'FAIL';
    const timer = setTimeout(() => controller.abort(new Error('Comparison timed out after 180 seconds. The Worker was stopped; try Preview on an idle device.')), 180000);
    $('quickjs-run').disabled = true; $('quickjs-stop').disabled = false; $('quickjs-profile').disabled = true;
    $('quickjs-export').disabled = true; $('quickjs-results').hidden = true;
    for (const runtime of Object.keys(paths)) badge(runtime, 'Waiting');
    $('quickjs-stop').onclick = () => { cancelled = true; controller.abort(new Error('Comparison stopped. Run again to start with fresh engines.')); };
    const onVisibility = () => { if (document.hidden) visibilityChanged = true; };
    let visibilityChanged = document.hidden;
    document.addEventListener('visibilitychange', onVisibility);
    try {
      if (location.protocol === 'file:') throw new Error('Serve the built dist directory over HTTP(S), then reload this page.');
      if (typeof Worker !== 'function') throw new Error('Web Workers are unavailable. Use a current browser with Workers enabled.');
      if (typeof QuickJSBench !== 'object') throw new Error('quickjs-workloads.js is missing. Rebuild browser_page and serve its complete dist directory.');
      status('Loading the runtime build manifests…', 'running');
      const response = await fetch('quickjs-manifest.json', {signal});
      if (!response.ok) throw new Error(`Could not load quickjs-manifest.json: HTTP ${response.status}. Rebuild browser_page.`);
      const build = await response.json();
      if (build?.schema !== 'dynarmic.js/quickjs-build-v1' || typeof build.upstream?.version !== 'string' || !build.upstream.version) throw new Error('Invalid QuickJS build manifest. Rebuild browser_page.');
      const hostResponse = await fetch('wasm-build-manifest.json', {signal});
      if (!hostResponse.ok) throw new Error(`Could not load wasm-build-manifest.json: HTTP ${hostResponse.status}. Rebuild browser_page.`);
      const dynarmicBuild = await hostResponse.json();
      if (dynarmicBuild?.schema !== 'dynarmic.js/wasm-coremark-host-build-v1' || !dynarmicBuild.source || !dynarmicBuild.build ||
          !['js', 'wasm'].every(kind => /^[a-f0-9]{64}$/.test(dynarmicBuild.artifacts?.[kind]?.sha256) && Number.isSafeInteger(dynarmicBuild.artifacts[kind].bytes) && dynarmicBuild.artifacts[kind].bytes > 0)) throw new Error('Invalid Dynarmic host build manifest. Rebuild browser_page.');
      const profile = $('quickjs-profile').value;
      const order = Object.keys(paths), offset = runNumber++ % order.length;
      order.push(...order.splice(0, offset));
      const report = {schema: 'dynarmic-quickjs-comparison-v1', measuredAt: new Date().toISOString(), build, dynarmicBuild,
        environment: {userAgent: navigator.userAgent, hardwareConcurrency: navigator.hardwareConcurrency || null, crossOriginIsolated,
          timeOrigin: performance.timeOrigin, visibilityChanged: false},
        profile, parameters: QuickJSBench.profiles[profile], runtimeOrder: order,
        methodology: {firstPasses: 1, warmupPasses: 1, measuredPasses: 3, memoryMode: 'flat window', sequentialWorkers: true,
          timingBoundaries: {
            quickjs: 'Benchmark call includes interpreter/context creation, JavaScript parsing, all kernel passes, reporting and cleanup. Module loading is separate.',
            browser: 'Benchmark call includes all kernel passes and statistics. Source import/parsing is counted in prepare/load; there is no QuickJS setup or cleanup.',
            kernels: 'Only the kernel call is timed. First pass, one discarded warmup, then three retained samples.'}}, runtimes: {}};
      for (const runtime of order) {
        current = runtime; badge(runtime, 'Loading', 'running');
        const result = await execute(runtime, profile, signal);
        signal.throwIfAborted(); validate(result, runtime, profile);
        report.runtimes[runtime] = result;
        badge(runtime, 'Verified', 'pass'); current = null;
      }
      report.environment.visibilityChanged = visibilityChanged;
      render(report); window.quickjsReport = report;
      $('quickjs-export').disabled = false;
      status(`PASS — all three paths match every reference output. ${visibilityChanged ? 'The tab was hidden during this run; timing may be throttled. Run again with the tab visible.' : 'Explore the measured times and engine statistics below.'}`, 'pass');
      outcome = 'PASS';
    } catch (error) {
      const reason = signal.aborted ? signal.reason : error;
      status(reason.message || String(reason), cancelled ? 'cancelled' : 'fail');
      for (const runtime of Object.keys(paths)) if (runtime === current || $(`q-badge-${runtime}`).textContent === 'Waiting') badge(runtime, cancelled ? 'Stopped' : runtime === current ? 'Failed' : 'Not run', cancelled ? '' : 'fail');
      outcome = cancelled ? 'CANCELLED' : 'FAIL';
    } finally {
      clearTimeout(timer); document.removeEventListener('visibilitychange', onVisibility);
      active = null; window.quickjsRunning = false;
      document.dispatchEvent(new CustomEvent('quickjs-comparison-state', {detail: {running: false}}));
      $('quickjs-run').disabled = false; $('quickjs-stop').disabled = true; $('quickjs-profile').disabled = false;
    }
    await reportSmoke(outcome);
  }
  $('quickjs-export').onclick = () => {
    if (!window.quickjsReport) return;
    const blob = new Blob([JSON.stringify(window.quickjsReport, null, 2) + '\n'], {type: 'application/json'});
    const url = URL.createObjectURL(blob), link = node('a');
    link.href = url; link.download = `dynarmic-quickjs-${window.quickjsReport.profile}.json`;
    link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  $('quickjs-run').onclick = run;
  $('quickjs-run').disabled = !!globalThis.cBenchRunning || !!globalThis.officialBenchRunning;
  document.addEventListener('official-bench-state', event => { $('quickjs-run').disabled = event.detail.running || !!globalThis.cBenchRunning || !!active; });
  document.addEventListener('c-bench-state', () => { $('quickjs-run').disabled = !!globalThis.cBenchRunning || !!globalThis.officialBenchRunning || !!active; });
  const query = new URLSearchParams(location.search);
  if (query.get('quickjs-profile') === 'full') $('quickjs-profile').value = 'full';
  if (query.get('quickjs') === '1') run();
})();
