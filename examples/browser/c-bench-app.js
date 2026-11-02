// Fixed-work C workloads. Each observation owns a disposable Worker and engine.
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const runtimes = {dynarmic: 'ARM · dynarmic.js', v86: 'x86 · v86', wasm: 'Direct Wasm'};
  const cases = {integer: 'Integer mix', branch: 'Data-dependent branches', stream: 'Sequential memory', pointer: 'Random memory', float: 'Float64 arithmetic'};
  const ids = Object.keys(cases), paths = Object.keys(runtimes), rounds = 3;
  let active = null, report = null, runNumber = 0;
  const node = (tag, text, className) => {
    const value = document.createElement(tag);
    if (text !== undefined) value.textContent = text;
    if (className) value.className = className;
    return value;
  };
  const median = samples => [...samples].sort((a, b) => a - b)[1];
  const stats = samples => ({samples, median: median(samples), min: Math.min(...samples), max: Math.max(...samples)});
  const geometricMean = samples => Math.exp(samples.reduce((sum, value) => sum + Math.log(value), 0) / samples.length);
  const ms = value => value < 0.1 ? '<0.1 ms' : `${value.toFixed(2)} ms`;
  const ratio = value => `${value.toFixed(2)}×`;
  const blocked = () => globalThis.quickjsRunning || globalThis.officialBenchRunning;
  function status(text, state) {
    $('c-bench-status').textContent = text;
    $('c-bench-status').dataset.state = state;
    $('c-bench-status').className = `status ${state === 'invalid' ? 'fail' : state}`;
  }
  function controls() {
    $('c-bench-run').disabled = !!active || !!blocked();
    $('c-bench-stop').disabled = !active;
    $('c-bench-profile').disabled = $('c-bench-case').disabled = !!active;
    $('c-bench-export').disabled = !report;
  }
  function validateProvenance(build, reference, host) {
    if (!globalThis.CBenchProtocol) throw new Error('The C benchmark protocol is unavailable. Rebuild the complete distribution.');
    CBenchProtocol.validateManifest(build);
    CBenchProtocol.validateReference(reference);
    if (host?.schema !== 'dynarmic.js/wasm-coremark-host-build-v1' || !host.source || host.build?.type !== 'Release' ||
        typeof host.build.profile !== 'boolean' || typeof host.configuration?.regionDiagnostics !== 'boolean' ||
        !['js', 'wasm'].every(kind => /^[a-f0-9]{64}$/.test(host.artifacts?.[kind]?.sha256) && Number.isSafeInteger(host.artifacts[kind].bytes) && host.artifacts[kind].bytes > 0)) throw new Error('Missing release ARM host provenance.');
  }
  function validate(result, task, reference) {
    CBenchProtocol.validateResult(result, task, reference);
    return {medianMs: median(result.samplesMs), belowResolution: median(result.samplesMs) < 0.1};
  }
  function summarize(value) {
    const summary = {cases: {}, v86OverArm: null, armOverDirect: null};
    const invalid = value.status === 'invalid' || value.environment.visibilityChanged || value.methodology.performanceEligible !== true;
    const records = value.observations.filter(entry => entry.status === 'pass').map(entry => entry.result);
    const shared = globalThis.CBenchProtocol?.summarize(records, value.selectedCases, rounds);
    for (const caseId of value.selectedCases) {
      const item = {runtimes: {}, v86OverArm: null, armOverDirect: null};
      for (const runtime of paths) {
        const entries = value.observations.filter(entry => entry.task.caseId === caseId && entry.task.runtime === runtime);
        const passed = entries.filter(entry => entry.status === 'pass');
        const measured = shared?.cases[caseId].runtimes[runtime];
        item.runtimes[runtime] = {completed: passed.length, complete: passed.length === rounds,
          status: entries.some(entry => entry.status === 'running') ? 'running' : entries.some(entry => entry.status === 'fail') ? 'fail' : entries.some(entry => entry.status === 'invalid') ? 'invalid' : entries.some(entry => entry.status === 'cancelled') ? 'cancelled' : passed.length === rounds ? 'pass' : 'queued',
          belowResolution: measured?.belowResolution || false,
          timing: passed.length === rounds ? measured.warmMs : null};
      }
      if (!invalid && shared) {
        item.v86OverArm = shared.cases[caseId].ratios.v86OverDynarmic;
        item.armOverDirect = shared.cases[caseId].ratios.dynarmicOverWasm;
      }
      summary.cases[caseId] = item;
    }
    if (!invalid && value.observations.every(entry => entry.status === 'pass')) {
      for (const name of ['v86OverArm', 'armOverDirect']) {
        if (value.selectedCases.every(id => summary.cases[id][name])) summary[name] = stats(Array.from({length: rounds}, (_, round) =>
          geometricMean(value.selectedCases.map(id => summary.cases[id][name].samples[round]))));
      }
    }
    return summary;
  }
  function stat(dl, label, text) { dl.append(node('dt', label), node('dd', text)); }
  function render() {
    if (!report) return;
    report.summary = summarize(report);
    const container = $('c-bench-results');
    const open = new Set(Array.from(container.querySelectorAll?.('details[open]') || [], item => item.dataset.key));
    const focused = container.contains?.(document.activeElement) ? document.activeElement : null;
    const focusKey = focused?.tagName === 'SUMMARY' ? focused.parentElement.dataset.key : null;
    container.replaceChildren();
    const complete = report.observations.filter(entry => entry.status === 'pass').length;
    const heading = node('div', undefined, 'c-bench-results-heading');
    heading.append(node('h3', 'Fixed work. Three execution paths.'), node('span', `${complete} / ${report.observations.length} observations verified`, 'mono-label'));
    container.append(heading);
    if (report.environment.visibilityChanged) container.append(node('p', 'Timing invalid: this tab was hidden. Completed observations remain in the export; restart with the tab visible.', 'notice'));
    if (report.dynarmicBuild && !report.methodology.performanceEligible) container.append(node('p', 'Instrumented ARM host: correctness checks remain available, but performance comparison and ratios are disabled. ' + report.methodology.instrumentation.description, 'notice'));
    container.append(node('p', 'Milliseconds per fixed workload · lower is faster. Each value is the median of three round medians; ranges span those round medians.', 'hint'));
    const totals = node('div', undefined, 'c-bench-ratios');
    for (const [name, label, note] of [['v86OverArm', 'v86 / ARM', 'Above 1: dynarmic.js takes less time.'], ['armOverDirect', 'ARM / direct Wasm', 'Reference ratio. Above 1: direct Wasm takes less time.']]) {
      const item = node('div'), result = report.summary[name];
      item.append(node('span', label), node('strong', result ? ratio(result.median) : '—'),
        node('small', result ? `${ratio(result.min)}–${ratio(result.max)} across rounds. ${note}` :
          report.dynarmicBuild && !report.methodology.performanceEligible ? 'Unavailable with an instrumented ARM host.' : 'Requires all three valid rounds above the timing floor.'));
      totals.append(item);
    }
    container.append(totals, node('p', `Overall ratios weight the ${report.selectedCases.length} selected ${report.selectedCases.length === 1 ? 'case' : 'cases'} equally within each round. Direct Wasm is a reference execution path.`, 'hint'));
    const cards = node('div', undefined, 'c-bench-cases');
    for (const caseId of report.selectedCases) {
      const data = report.summary.cases[caseId], card = node('article', undefined, 'c-bench-case');
      const config = report.reference?.profiles[report.profile];
      card.append(node('h4', cases[caseId]), node('p', config ? `${config.iterations[caseId].toLocaleString('en-US')} iterations · fixed seed ${config.seed}` : 'Waiting for the reference inputs.', 'c-bench-work'));
      const grid = node('div', undefined, 'c-bench-runtime-grid');
      const maximum = Math.max(0, ...paths.map(runtime => data.runtimes[runtime].timing?.median || 0));
      for (const runtime of paths) {
        const item = data.runtimes[runtime], panel = node('div', undefined, 'c-bench-runtime'); panel.dataset.runtime = runtime;
        panel.append(node('h5', runtimes[runtime]));
        if (item.timing) {
          panel.append(node('strong', ms(item.timing.median)), node('small', `${ms(item.timing.min)}–${ms(item.timing.max)} · 3 rounds`));
          const track = node('div', undefined, 'c-bench-track'), bar = node('span');
          track.setAttribute('aria-hidden', 'true'); bar.style.width = `${maximum ? item.timing.median / maximum * 100 : 0}%`; track.append(bar); panel.append(track);
          if (item.belowResolution) panel.append(node('small', 'Below the timing floor; no ratios.'));
        } else panel.append(node('strong', {running: 'Running…', fail: 'Failed', cancelled: 'Stopped', invalid: 'Invalid', queued: 'Waiting'}[item.status] || 'Waiting'), node('small', `${item.completed} / 3 rounds verified`));
        grid.append(panel);
      }
      card.append(grid);
      const comparison = node('p', undefined, 'c-bench-case-ratios');
      comparison.textContent = `v86 / ARM: ${data.v86OverArm ? ratio(data.v86OverArm.median) : '—'} · ARM / direct: ${data.armOverDirect ? ratio(data.armOverDirect.median) : '—'}`;
      card.append(comparison);
      const details = node('details', undefined, 'c-bench-details'); details.dataset.key = caseId; details.open = open.has(caseId);
      details.append(node('summary', 'Rounds, first runs, loading & validation'));
      for (const entry of report.observations.filter(entry => entry.task.caseId === caseId)) {
        const dl = node('dl'); details.append(node('h5', `Round ${entry.task.round + 1} · ${runtimes[entry.task.runtime]} · ${entry.status}`));
        if (entry.status === 'pass') {
          const result = entry.result;
          stat(dl, 'Measured samples', result.samplesMs.map(ms).join(' / '));
          stat(dl, 'Round median', ms(entry.measurement.medianMs)); stat(dl, 'First run', ms(result.firstMs)); stat(dl, 'Prepare / load', ms(result.loadMs));
          stat(dl, 'Unmeasured warmups', result.warmupMs.map(ms).join(' / '));
          stat(dl, 'Checksum, all six calls', `${result.expected} · matched`);
          stat(dl, 'Wasm memory capacity', `${(result.memoryBytes / 1048576).toFixed(1)} MiB`);
        } else stat(dl, 'Observation', entry.error || entry.status);
        details.append(dl);
      }
      card.append(details); cards.append(card);
    }
    container.append(cards);
    const provenance = node('details', undefined, 'c-bench-provenance'); provenance.dataset.key = 'provenance'; provenance.open = open.has('provenance');
    provenance.append(node('summary', 'Build records & measurement method'), node('p', 'The export includes compiler flags, source and artifact hashes, host build records, runtime order, raw timings and checksums. Loading and the first run are separate from measured work. Two warmups do not establish steady-state performance.', 'hint'));
    const links = node('p');
    for (const [href, label] of [['c-bench-manifest.json', 'C build manifest'], ['c-bench-reference.json', 'Inputs & reference outputs'], ['wasm-build-manifest.json', 'ARM host build']]) {
      const link = node('a', label); link.href = href; links.append(link, node('span', ' · '));
    }
    provenance.append(links); container.append(provenance);
    if (focusKey) Array.from(container.querySelectorAll?.('details') || []).find(item => item.dataset.key === focusKey)?.querySelector('summary')?.focus({preventScroll: true});
  }
  async function jsonFile(name, signal) {
    signal.throwIfAborted();
    const response = await fetch(name, {signal, cache: 'no-store'});
    if (!response.ok) throw new Error(`Could not load ${name}: HTTP ${response.status}. Serve the complete built distribution.`);
    const value = await response.json(); signal.throwIfAborted(); return value;
  }
  function execute(task, signal, timeoutMs, progress) {
    signal.throwIfAborted();
    return new Promise((resolve, reject) => {
      let worker, settled = false;
      const finish = (error, value) => {
        if (settled) return;
        settled = true; clearTimeout(timer); signal.removeEventListener('abort', abort); worker?.terminate();
        error ? reject(error) : resolve(value);
      };
      const abort = () => finish(signal.reason);
      const timer = setTimeout(() => finish(new Error(`Observation exceeded ${timeoutMs / 1000} seconds; its Worker was stopped.`)), timeoutMs);
      signal.addEventListener('abort', abort, {once: true});
      try {
        worker = new Worker('c-bench-worker.js');
        worker.onmessage = ({data}) => {
          if (settled) return;
          if (data?.type === 'progress') {
            if (['runtime', 'caseId', 'profile', 'round', 'token'].every(field => data[field] === task[field])) progress(data.phase);
          } else if (!data?.ok) finish(new Error(data?.error || 'The C benchmark Worker failed.'));
          else finish(null, data);
        };
        worker.onerror = event => { event.preventDefault(); finish(new Error(event.message || 'Could not start the C benchmark Worker.')); };
        worker.onmessageerror = () => finish(new Error('The Worker reply could not be read.'));
        worker.postMessage(task);
      } catch (error) { finish(error); }
    });
  }
  async function reportSmoke(outcome) {
    const target = new URLSearchParams(location.search).get('cbench-report');
    if (!target) return;
    try {
      const url = new URL(target, location.href); if (url.origin !== location.origin) return;
      url.searchParams.set('status', `${outcome}|${$('c-bench-status').textContent}`);
      await fetch(url, {signal: AbortSignal.timeout(5000)});
    } catch { /* Optional local test reporting does not change the result. */ }
  }
  async function run() {
    if (active || blocked()) return;
    const profile = $('c-bench-profile').value, selected = $('c-bench-case').value;
    if (!['quick', 'full'].includes(profile) || (selected !== 'all' && !ids.includes(selected))) return;
    const selectedCases = selected === 'all' ? [...ids] : [selected];
    const controller = new AbortController(), {signal} = controller;
    active = controller; globalThis.cBenchRunning = true;
    const runId = `${Date.now()}-${runNumber++}`;
    const workerTimeoutMs = profile === 'quick' ? 90000 : 180000, totalTimeoutMs = profile === 'quick' ? 600000 : 1200000;
    let abortKind = 'cancelled';
    const cancel = (kind, message) => { if (!signal.aborted) { abortKind = kind; controller.abort(new Error(message)); } };
    const deadline = setTimeout(() => cancel('fail', 'The comparison timed out. Partial results are retained.'), totalTimeoutMs);
    report = {schema: 'dynarmic.js/c-bench-comparison-v1', startedAt: new Date().toISOString(), status: 'running', verified: false,
      profile, selectedCases, methodology: {rounds: 3, firstRuns: 1, warmupRuns: 2, measuredRuns: 3, freshWorkerPerObservation: true,
        timingFloorMs: 0.1, workerTimeoutMs, totalTimeoutMs,
        performanceEligible: false, instrumentation: {profile: null, regionDiagnostics: null, description: 'Awaiting the host build record.'},
        aggregation: 'Median of three measured fixed-work calls per round, then median/min/max of three round medians; ratios pair the same round before equal-case geometric means',
        scope: 'Same C source and compiler version without libc; complete ARM, x86 and direct Wasm execution paths still differ'},
      environment: {userAgent: navigator.userAgent, hardwareConcurrency: navigator.hardwareConcurrency || null, visibilityChanged: false}, observations: []};
    for (let round = 0; round < rounds; round++) for (let index = 0; index < selectedCases.length; index++) {
      const caseId = selectedCases[(index + round) % selectedCases.length];
      const offset = (round + selectedCases.indexOf(caseId) + runNumber - 1) % paths.length;
      for (let position = 0; position < paths.length; position++) {
        const runtime = paths[(position + offset) % paths.length];
        report.observations.push({task: {runtime, caseId, profile, round, position, token: `${runId}-${round}-${caseId}-${runtime}`}, status: 'queued'});
      }
    }
    window.cBenchReport = report;
    const visibility = () => {
      if (document.hidden) { report.environment.visibilityChanged = true; cancel('invalid', 'Stopped: the tab was hidden. Timings are invalid; keep this tab visible and start again.'); }
    };
    document.addEventListener('visibilitychange', visibility);
    document.dispatchEvent(new CustomEvent('c-bench-state', {detail: {running: true}}));
    controls(); render(); status('Loading the C benchmark references and build records…', 'running');
    try {
      if (document.hidden) visibility(); signal.throwIfAborted();
      if (location.protocol === 'file:') throw new Error('Serve the complete built page over HTTP(S) to run C benchmarks.');
      const [build, reference, host] = await Promise.all(['c-bench-manifest.json', 'c-bench-reference.json', 'wasm-build-manifest.json'].map(name => jsonFile(name, signal)));
      signal.throwIfAborted(); validateProvenance(build, reference, host);
      Object.assign(report, {build, reference, dynarmicBuild: host});
      report.methodology.performanceEligible = !host.build.profile && !host.configuration.regionDiagnostics;
      report.methodology.instrumentation = {profile: host.build.profile, regionDiagnostics: host.configuration.regionDiagnostics,
        description: [host.build.profile ? 'ARM profiling is enabled.' : 'ARM profiling is disabled.',
          host.configuration.regionDiagnostics ? 'Region diagnostics are enabled.' : 'Region diagnostics are disabled.'].join(' ')};
      render();
      let completed = 0;
      for (const entry of report.observations) {
        signal.throwIfAborted(); entry.status = 'running'; render();
        const label = `${++completed} / ${report.observations.length} · round ${entry.task.round + 1} · ${cases[entry.task.caseId]} · ${runtimes[entry.task.runtime]}`;
        status(`${label} · preparing…`, 'running');
        try {
          entry.result = await execute(entry.task, signal, workerTimeoutMs, phase => status(`${label} · ${phase === 'loading' ? 'preparing' : 'executing'}…`, 'running'));
          signal.throwIfAborted(); entry.measurement = validate(entry.result, entry.task, reference); entry.status = 'pass';
        } catch (error) {
          if (signal.aborted) throw error;
          entry.status = 'fail'; entry.error = error.message || String(error);
        }
        render();
      }
      report.verified = report.observations.every(entry => entry.status === 'pass');
      report.status = report.verified ? 'pass' : 'fail';
      const short = report.observations.some(entry => entry.measurement?.belowResolution);
      status(report.verified ? `PASS — all ${report.observations.length} observations matched the reference.${!report.methodology.performanceEligible ? ' The ARM host is instrumented; performance ratios are unavailable.' : short ? ' Some timings are below 0.1 ms; their ratios are withheld. Try Extended work.' : ' Three fresh-engine rounds completed.'}` : 'Some observations failed. Partial measurements and errors are retained; inspect the results or export JSON.', report.status);
    } catch (error) {
      report.status = signal.aborted ? abortKind : 'fail'; report.verified = false;
      report.error = (signal.aborted ? signal.reason : error).message || String(error);
      if (!signal.aborted) controller.abort(error); // Release any other pending metadata fetches.
      for (const entry of report.observations) if (entry.status === 'queued' || entry.status === 'running') { entry.status = report.status; entry.error = report.error; }
      status(report.error, report.status);
    } finally {
      clearTimeout(deadline); document.removeEventListener('visibilitychange', visibility);
      report.finishedAt = new Date().toISOString(); report.performanceValid = report.verified && report.methodology.performanceEligible && !report.environment.visibilityChanged && !report.observations.some(entry => entry.measurement?.belowResolution);
      render(); window.cBenchReport = report; active = null; globalThis.cBenchRunning = false;
      document.dispatchEvent(new CustomEvent('c-bench-state', {detail: {running: false}})); controls();
    }
    await reportSmoke(report.status.toUpperCase());
  }
  $('c-bench-run').onclick = run;
  $('c-bench-stop').onclick = () => active?.abort(new Error('Stopped. Partial results are retained; run again to start with fresh Workers.'));
  $('c-bench-export').onclick = () => {
    if (!report) return;
    const url = URL.createObjectURL(new Blob([JSON.stringify(report, null, 2) + '\n'], {type: 'application/json'}));
    const link = node('a'); link.href = url; link.download = `dynarmic-c-benchmarks-${report.profile}.json`; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  for (const name of ['quickjs-comparison-state', 'official-bench-state']) document.addEventListener(name, controls);
  controls();
  const requested = new URLSearchParams(location.search).get('cbench');
  if (requested === 'quick' || requested === 'full') { $('c-bench-profile').value = requested; run(); }
})();
