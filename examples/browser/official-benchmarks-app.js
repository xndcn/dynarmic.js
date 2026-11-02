// The JS suite shipped in QuickJS extras, measured in three sequential Workers.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const runtimes = {dynarmic: 'QuickJS on ARM', wasm: 'QuickJS on Wasm', native: 'Browser JavaScript'};
  const caseIds = ['Richards/Richards', 'DeltaBlue/DeltaBlue', 'Crypto/Encrypt', 'Crypto/Decrypt',
    'RayTrace/RayTrace', 'EarleyBoyer/Earley', 'EarleyBoyer/Boyer', 'RegExp/RegExp', 'Splay/Splay', 'NavierStokes/NavierStokes'];
  const TIMEOUT_MS = 300000;
  let inventory = null, active = null, report = null, runNumber = 0;
  const el = (tag, text, className) => {
    const element = document.createElement(tag);
    if (text !== undefined) element.textContent = text;
    if (className) element.className = className;
    return element;
  };
  const numeric = value => typeof value === 'number' && Number.isFinite(value) && value >= 0;
  const ms = value => value === 0 ? '≈0 ms' : value < .01 ? '<0.01 ms' : `${value.toFixed(2)} ms`;
  function status(text, state = 'idle') {
    $('official-status').textContent = text;
    $('official-status').dataset.state = state;
    $('official-status').className = `status ${['running', 'pass', 'fail'].includes(state) ? state : ''}`;
  }
  function blocked() { return !!(globalThis.quickjsRunning || globalThis.cBenchRunning); }
  function controls() {
    const busy = !!active;
    $('official-run').disabled = !inventory || busy || blocked();
    $('official-run-all').disabled = !inventory || busy || blocked();
    $('official-stop').disabled = !busy;
    $('official-case').disabled = !inventory || busy;
    $('official-reload').hidden = !!inventory || busy;
  }
  async function jsonFile(name, signal) {
    const response = await fetch(name, {signal, cache: 'no-store'});
    if (!response.ok) throw new Error(`Could not load ${name}: HTTP ${response.status}. Rebuild browser_page and serve the complete distribution.`);
    return response.json();
  }
  async function loadInventory() {
    if (active) return;
    $('official-reload').disabled = true;
    try {
      const value = await jsonFile('official-benchmarks.json', AbortSignal.timeout(30000));
      if (value.schema !== 'dynarmic.js/official-benchmarks-v1' || !Array.isArray(value.cases) || !Array.isArray(value.suites) || value.suites.length !== 8 ||
          JSON.stringify(value.cases.map(item => item.id).sort()) !== JSON.stringify([...caseIds].sort())) throw new Error('Official JavaScript benchmark inventory is incomplete.');
      const mapped = value.suites.flatMap(suite => suite.benchmarks.map(name => `${suite.id}/${name}`));
      if (JSON.stringify(mapped.sort()) !== JSON.stringify([...caseIds].sort())) throw new Error('Official benchmark suite mapping is incomplete.');
      inventory = value;
      $('official-case').replaceChildren();
      for (const suite of inventory.suites) {
        const group = el('optgroup'); group.label = suite.title;
        for (const name of suite.benchmarks) { const option = el('option', name); option.value = `${suite.id}/${name}`; group.append(option); }
        $('official-case').append(group);
      }
      const requested = new URLSearchParams(location.search).get('official');
      if (caseIds.includes(requested)) $('official-case').value = requested;
      status('Ready. Choose a JavaScript case, or run all eight suites (ten cases).');
      controls();
      if (requested === 'all' || caseIds.includes(requested)) runCases(requested === 'all' ? caseIds : [requested]);
    } catch (error) { status(error.message || String(error), 'fail'); }
    finally { controls(); $('official-reload').disabled = false; }
  }
  function runWorker(runtime, caseId, signal, progress) {
    signal.throwIfAborted();
    return new Promise((resolve, reject) => {
      let worker = null, settled = false;
      const finish = (error, result) => {
        if (settled) return;
        settled = true; clearTimeout(timer); signal.removeEventListener('abort', abort); worker?.terminate();
        error ? reject(error) : resolve(result);
      };
      const abort = () => finish(signal.reason);
      const timer = setTimeout(() => finish(new Error(`${runtime}/${caseId} exceeded the five-minute limit. The Worker was stopped.`)), TIMEOUT_MS);
      signal.addEventListener('abort', abort, {once: true});
      try {
        worker = new Worker('official-benchmarks-worker.js');
        worker.onmessage = ({data}) => {
          if (settled) return;
          if (data?.type === 'progress') { if (data.runtime === runtime && data.caseId === caseId) progress(data.phase); }
          else if (!data?.ok) finish(new Error(data?.error || 'The benchmark Worker failed.'));
          else finish(null, data);
        };
        worker.onerror = event => { event.preventDefault(); finish(new Error(event.message || 'Could not load the benchmark Worker.')); };
        worker.onmessageerror = () => finish(new Error('The benchmark Worker reply could not be read.'));
        worker.postMessage({runtime, caseId});
      } catch (error) { finish(error); }
    });
  }
  function validate(result, runtime, caseId) {
    if (!result || result.ok !== true || result.runtime !== runtime || result.caseId !== caseId || !numeric(result.loadMs) || !numeric(result.runMs)) throw new Error('Invalid Worker timing or identity.');
    if (!result.stats || typeof result.stats !== 'object' || Array.isArray(result.stats) || !Object.values(result.stats).every(numeric)) throw new Error('Invalid JIT statistics.');
    if (runtime === 'native' ? result.memoryBytes !== null || result.quickjsMemoryLimitBytes !== null : !Number.isSafeInteger(result.memoryBytes) || result.memoryBytes <= 0 || result.quickjsMemoryLimitBytes !== 192 * 1048576) throw new Error('Unexpected engine memory configuration.');
    const value = result.official;
    const [suiteId, benchmarkName] = caseId.split('/');
    if (value?.schema !== 'dynarmic.js/official-v8-case-v1' || value.caseId !== caseId || value.status !== 'pass' || value.mode !== 'upstream' || value.version !== '7' ||
        value.warmupMs !== 1000 || value.measureMs !== 1000 || value.minimumRuns !== 32 || value.suite !== suiteId || value.name !== benchmarkName) throw new Error('The result does not use the upstream V8 v7 protocol.');
    if (!Array.isArray(value.samplesMs) || value.samplesMs.length !== 1 || !value.samplesMs.every(v => numeric(v) && v > 0) ||
        !Array.isArray(value.runs) || value.runs.length !== 1 || !value.runs.every(v => Number.isSafeInteger(v) && v >= 32) ||
        !Array.isArray(value.elapsedMs) || value.elapsedMs.length !== 1 || !value.elapsedMs.every(v => numeric(v) && v >= 1000)) throw new Error('Incomplete official measurement window.');
    const mean = value.elapsedMs[0] / value.runs[0];
    if (Math.abs(mean - value.samplesMs[0]) > Math.max(1, mean) * 1e-9 || !value.summary || !['minMs','maxMs','medianMs','meanMs'].every(k => numeric(value.summary[k]) && Math.abs(value.summary[k] - mean) <= Math.max(1, mean) * 1e-9)) throw new Error('Timing summary differs from the raw measurements.');
    const suite = inventory.suites.find(item => item.id === suiteId), completeSuite = suite.benchmarks.length === 1;
    if (!numeric(value.referenceUs) || value.referenceUs <= 0 || value.completeSuite !== completeSuite ||
        !Array.isArray(value.suiteCases) || JSON.stringify(value.suiteCases) !== JSON.stringify(suite.benchmarks.map(name => `${suiteId}/${name}`))) throw new Error('Invalid official suite metadata.');
    const score = 100 * value.referenceUs / (mean * 1000);
    if (completeSuite ? !numeric(value.score) || value.score <= 0 || Math.abs(value.score - score) > Math.max(1, score) * 1e-9 : value.score !== null) throw new Error('Invalid suite score.');
    if (!Array.isArray(value.logs) || !value.logs.every(message => typeof message === 'string') || value.validation?.upstreamAssertions !== true || value.validation?.setupAndTearDown !== true) throw new Error('Missing upstream validation metadata.');
    if (!Array.isArray(value.batches) || value.batches.filter(batch => batch.phase === 'warmup').length !== 1 ||
        value.batches[0]?.phase !== 'warmup' || !value.batches.every(batch => numeric(batch.elapsedMs) && batch.elapsedMs >= 1000 &&
          (batch.phase === 'warmup' ? batch.runs === null : batch.phase === 'measurement' && Number.isSafeInteger(batch.runs) && batch.runs > 0))) throw new Error('Invalid upstream measurement batches.');
    const measured = value.batches.filter(batch => batch.phase === 'measurement');
    if (measured.reduce((sum, batch) => sum + batch.runs, 0) !== value.runs[0] ||
        Math.abs(measured.reduce((sum, batch) => sum + batch.elapsedMs, 0) - value.elapsedMs[0]) > Math.max(1, value.elapsedMs[0]) * 1e-9) throw new Error('Measurement batches differ from their totals.');
  }
  function stat(dl, label, text) { dl.append(el('dt', label), el('dd', text)); }
  function render() {
    const container = $('official-results');
    const open = new Set(Array.from(container.querySelectorAll?.('details[open]') || [], item => item.dataset.key));
    const oldScroll = container.querySelector?.('.official-table-wrap');
    const scrollLeft = oldScroll?.scrollLeft || 0;
    const focused = container.contains?.(document.activeElement) ? document.activeElement : null;
    const focusKey = focused?.tagName === 'SUMMARY' ? focused.parentElement.dataset.key : focused === oldScroll ? 'table' : null;
    container.replaceChildren();
    const scroll = el('div', undefined, 'official-table-wrap'); scroll.tabIndex = 0; scroll.setAttribute('role', 'region'); scroll.setAttribute('aria-label', 'Benchmark timings for three engines');
    const table = el('table', undefined, 'official-table'); table.setAttribute('role', 'table'); table.append(el('caption', 'Measured time per iteration · lower is faster'));
    const head = el('thead'), headRow = el('tr'); head.setAttribute('role', 'rowgroup'); headRow.setAttribute('role', 'row');
    for (const name of ['JavaScript case', ...Object.values(runtimes)]) { const cell = el('th', name); cell.scope = 'col'; cell.setAttribute('role', 'columnheader'); headRow.append(cell); }
    head.append(headRow); table.append(head); const body = el('tbody'); body.setAttribute('role', 'rowgroup');
    for (const caseId of report.selectedCases) {
      const parts = caseId.split('/');
      const row = el('tr'), name = el('th', parts[0] === parts[1] ? parts[0] : parts.join(' / '));
      row.setAttribute('role', 'row'); name.scope = 'row'; name.setAttribute('role', 'rowheader'); row.append(name);
      for (const runtime of Object.keys(runtimes)) {
        const entry = report.cases[caseId][runtime], cell = el('td'); cell.dataset.state = entry.status; cell.setAttribute('role', 'cell');
        const label = el('span', runtimes[runtime], 'official-runtime-label'); label.setAttribute('aria-hidden', 'true'); cell.append(label);
        if (entry.status === 'pass') {
          const value = entry.result.official;
          cell.append(el('strong', ms(value.samplesMs[0])), el('small', `${value.runs[0].toLocaleString('en-US')} iterations · completed`));
        } else { cell.append(el('span', {queued:'Waiting',running:'Running…',cancelled:'Stopped',fail:'Failed'}[entry.status], 'official-pending')); }
        row.append(cell);
      }
      body.append(row);
    }
    table.append(body); scroll.append(table); container.append(scroll);
    const completeSuites = report.inventory.suites.filter(suite => suite.benchmarks.every(name => report.selectedCases.includes(`${suite.id}/${name}`)));
    const scores = el('details', undefined, 'official-details'); scores.dataset.key = 'all'; scores.open = open.has('all'); scores.append(el('summary', 'Suite scores, measurements & JIT statistics'));
    for (const suite of completeSuites) {
      const dl = el('dl');
      for (const [runtime, label] of Object.entries(runtimes)) {
        const entries = suite.benchmarks.map(name => report.cases[`${suite.id}/${name}`][runtime]);
        if (!entries.every(entry => entry.status === 'pass')) continue;
        const meanUs = Math.exp(entries.reduce((sum, entry) => sum + Math.log(entry.result.official.samplesMs[0] * 1000), 0) / entries.length);
        stat(dl, `${suite.title} · ${label}`, `${(100 * entries[0].result.official.referenceUs / meanUs).toFixed(1)} points`);
      }
      scores.append(dl);
    }
    scores.append(el('p', 'Suite scores use the upstream v7 reference and geometric mean. Multi-case scores appear only when every case in that suite completes; separate Workers make this a browser adaptation, not a canonical single-process suite score.', 'hint'));
    for (const caseId of report.selectedCases) {
      const details = el('details'); details.dataset.key = caseId; details.open = open.has(caseId); details.append(el('summary', caseId));
      for (const [runtime, label] of Object.entries(runtimes)) {
        const entry = report.cases[caseId][runtime], dl = el('dl'); details.append(el('h3', label));
        if (entry.status === 'pass') {
          const result = entry.result, value = result.official;
          stat(dl, 'Prepare / load', ms(result.loadMs)); stat(dl, 'Complete benchmark call', ms(result.runMs));
          stat(dl, 'Measured work', `${value.runs[0]} iterations in ${ms(value.elapsedMs[0])}`);
          stat(dl, 'QuickJS allocation limit', runtime === 'native' ? 'Not applicable' : '192 MiB');
          stat(dl, 'Wasm memory capacity', runtime === 'native' ? 'Not applicable' : `${(result.memoryBytes / 1048576).toFixed(1)} MiB`);
          if (runtime === 'dynarmic') {
            stat(dl, 'Compiled blocks / generated Wasm', `${result.stats.blocks ?? 'Unavailable'} / ${result.stats.bytes ?? 'Unavailable'} bytes`);
            stat(dl, 'Translate / optimize / emit', [result.stats.translateMs, result.stats.optMs, result.stats.emitMs].map(v => numeric(v) ? ms(v) : 'Unavailable').join(' / '));
            stat(dl, 'Compile / instantiate', [result.stats.compileMs, result.stats.instMs].map(v => numeric(v) ? ms(v) : 'Unavailable').join(' / '));
          }
        } else stat(dl, 'Status', entry.error || entry.status);
        details.append(dl);
      }
      scores.append(details);
    }
    container.append(scores);
    scroll.scrollLeft = scrollLeft;
    if (focusKey === 'table') scroll.focus?.({preventScroll: true});
    else if (focusKey) {
      const detail = Array.from(container.querySelectorAll?.('details') || []).find(item => item.dataset.key === focusKey);
      detail?.querySelector('summary')?.focus({preventScroll: true});
    }
  }
  async function reportSmoke(outcome) {
    const target = new URLSearchParams(location.search).get('official-report');
    if (!target) return;
    try { const url = new URL(target, location.href); if (url.origin !== location.origin) return;
      url.searchParams.set('status', `${outcome}|${$('official-status').textContent}`);
      await fetch(url, {signal: AbortSignal.timeout(5000)});
    } catch { /* Test reporting never changes a visible result. */ }
  }
  async function runCases(selected) {
    if (active || !inventory || blocked()) return;
    const controller = new AbortController(); active = controller; report = null; globalThis.officialBenchRunning = true; window.officialBenchReport = null;
    document.dispatchEvent(new CustomEvent('official-bench-state', {detail: {running: true}}));
    const {signal} = controller;
    let hidden = document.hidden, outcome = 'FAIL', failures = 0;
    const visibility = () => { if (document.hidden) hidden = true; };
    document.addEventListener('visibilitychange', visibility);
    $('official-export').disabled = true;
    $('official-results').replaceChildren(el('p', 'Preparing the selected benchmarks…', 'hint'));
    controls(); status('Preparing the official JavaScript benchmark runtimes…', 'running');
    $('official-stop').onclick = () => controller.abort(new Error('Stopped. Start again to use fresh Workers.'));
    try {
      if (location.protocol === 'file:') throw new Error('Serve the complete built page over HTTP(S) to run benchmarks.');
      const loadSignal = AbortSignal.any([signal, AbortSignal.timeout(30000)]);
      const [quickjsBuild, dynarmicBuild] = await Promise.all([jsonFile('quickjs-manifest.json', loadSignal), jsonFile('arm-bench-build-manifest.json', loadSignal)]);
      if (quickjsBuild.schema !== 'dynarmic.js/quickjs-build-v1' || !Array.isArray(quickjsBuild.artifacts) ||
          !['quickjs-arm.bin','quickjs-wasm.js','quickjs-wasm.wasm','official-v8.js','official-v8-adapter.js'].every(name => quickjsBuild.artifacts.some(item => item.path === name && /^[a-f0-9]{64}$/.test(item.sha256) && Number.isSafeInteger(item.bytes) && item.bytes > 0)) ||
          dynarmicBuild.schema !== 'dynarmic.js/wasm-coremark-host-build-v1' || !dynarmicBuild.source || !dynarmicBuild.build ||
          dynarmicBuild.configuration?.guestRamBytes !== 256 * 1048576 ||
          !['js','wasm'].every(kind => /^[a-f0-9]{64}$/.test(dynarmicBuild.artifacts?.[kind]?.sha256) && Number.isSafeInteger(dynarmicBuild.artifacts[kind].bytes) && dynarmicBuild.artifacts[kind].bytes > 0)) throw new Error('Missing benchmark runtime build provenance.');
      const order = Object.keys(runtimes), offset = runNumber++ % order.length; order.push(...order.splice(0, offset));
      report = {schema:'dynarmic.js/official-v8-comparison-v1', startedAt:new Date().toISOString(), status:'running', inventory,
        methodology:{...inventory.methodology,timeoutPerRuntimeMs:TIMEOUT_MS, quickjsMemoryLimitBytes:192*1048576,
          timingBoundaries:{quickjs:'Preparation loads source/module and sets guest memory. Complete call includes JavaScript parsing, context creation, upstream setup, warmup, measurements, checks and cleanup.',native:'Preparation imports/parses the JS source. Complete call includes upstream setup, warmup, measurements and checks.'}},
        environment:{userAgent:navigator.userAgent,hardwareConcurrency:navigator.hardwareConcurrency || null,visibilityChanged:false},
        quickjsBuild,dynarmicBuild,runtimeOrder:order,selectedCases:[...selected],cases:{}};
      for (const caseId of selected) report.cases[caseId] = Object.fromEntries(Object.keys(runtimes).map(runtime => [runtime,{status:'queued'}]));
      render(); let position = 0;
      for (const caseId of selected) for (const runtime of order) {
        signal.throwIfAborted(); const entry = report.cases[caseId][runtime]; entry.status = 'running'; position++; render();
        try {
          const result = await runWorker(runtime,caseId,signal, phase => status(`${position} / ${selected.length*3} · ${caseId} · ${runtimes[runtime]} · ${phase==='loading'?'preparing':'executing'}…`, 'running'));
          signal.throwIfAborted(); validate(result,runtime,caseId);
          entry.status='pass'; entry.result=result;
        } catch (error) {
          if (signal.aborted) throw error;
          entry.status='fail'; entry.error=error.message || String(error); failures++;
        }
        render();
      }
      report.status = failures ? 'fail' : 'pass'; outcome = failures ? 'FAIL' : 'PASS';
      status(`${outcome} — ${selected.length} cases across three engines. ${failures ? `${failures} measurements failed; inspect the retained results.` : 'All upstream runs completed.'}${hidden ? ' The tab was hidden; timings may be throttled.' : ''}`, report.status);
    } catch (error) {
      outcome = signal.aborted ? 'CANCELLED' : 'FAIL';
      status((signal.aborted ? signal.reason : error).message || String(error), signal.aborted ? 'cancelled' : 'fail');
      if (report?.status === 'running') {
        report.status=signal.aborted?'cancelled':'fail';
        for (const entries of Object.values(report.cases)) for (const entry of Object.values(entries)) if (entry.status==='queued'||entry.status==='running') {
          entry.status=signal.aborted?'cancelled':'fail'; entry.error=signal.aborted?'Stopped by user.':String(error.message||error);
        }
        render();
      }
    } finally {
      if (report) { report.finishedAt=new Date().toISOString(); report.environment.visibilityChanged=hidden; window.officialBenchReport=report; $('official-export').disabled=false; }
      document.removeEventListener('visibilitychange',visibility); active=null; globalThis.officialBenchRunning=false;
      document.dispatchEvent(new CustomEvent('official-bench-state',{detail:{running:false}})); controls();
    }
    await reportSmoke(outcome);
  }
  $('official-run').onclick=()=>runCases([$('official-case').value]);
  $('official-run-all').onclick=()=>runCases(caseIds);
  $('official-reload').onclick=loadInventory;
  $('official-export').onclick=()=>{
    if (!window.officialBenchReport) return;
    const url=URL.createObjectURL(new Blob([JSON.stringify(window.officialBenchReport,null,2)+'\n'],{type:'application/json'}));
    const link=el('a');link.href=url;link.download='dynarmic-official-js-benchmarks.json';link.click();setTimeout(()=>URL.revokeObjectURL(url),1000);
  };
  document.addEventListener('quickjs-comparison-state',controls);
  document.addEventListener('c-bench-state',controls);
  loadInventory();
})();
