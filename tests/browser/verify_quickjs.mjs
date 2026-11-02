#!/usr/bin/env node
// Exercise the actual three-runtime comparison in a Chromium Worker.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pause, withChromium} from './chromium.mjs';

const [browser, directory = 'build-em/examples/browser/dist', ...extra] = process.argv.slice(2);
if (!browser || (extra.length && !(extra.length === 2 && extra[0] === '--report'))) {
  console.error('usage: verify_quickjs.mjs <chrome-executable> [dist-directory] [--report output.json]');
  process.exit(1);
}
const root = path.resolve(directory);
const checks = [];
let userAgent = '', report, artifactHashes = {};
const record = check => { checks.push(check); console.log(`PASS ${check}`); };
let broken = null, delayWorker = false, loopGuest = false, loopGuestServed = false;
const downloads = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-quickjs-export-'));

try {
  const artifacts = ['index.html', 'styles.css', 'quickjs-app.js', 'quickjs-worker.js',
    'quickjs-workloads.js', 'quickjs-arm.bin', 'quickjs-wasm.js', 'quickjs-wasm.wasm',
    'quickjs-manifest.json', 'wasm-build-manifest.json', 'arm_jit.js', 'arm_jit.wasm'];
  const hash = name => createHash('sha256').update(fs.readFileSync(path.join(root, name))).digest('hex');
  artifactHashes = Object.fromEntries(artifacts.map(name => [name, hash(name)]));
  await withChromium({browser, root, serve: async ({name, response}) => {
    if (name === broken) { response.writeHead(404).end(); return true; }
    if (loopGuest && name === 'quickjs-arm.bin') {
      response.writeHead(200, {'content-type': 'application/octet-stream'})
        .end(Buffer.from([0xfe, 0xff, 0xff, 0xea]), () => { loopGuestServed = true; });
      return true;
    }
    if (delayWorker && name === 'quickjs-worker.js') await pause(1000);
    return false;
  }}, async ({call, evaluate, navigate, until}) => {
    const state = () => evaluate(`({
      state: document.getElementById('quickjs-status').dataset.state,
      text: document.getElementById('quickjs-status').textContent,
      runDisabled: document.getElementById('quickjs-run').disabled,
      stopDisabled: document.getElementById('quickjs-stop').disabled,
      resultsLinkHidden: document.getElementById('quickjs-view-results').hidden
    })`);
    const click = id => evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);
    const finished = () => until(async () => {
      const value = await state();
      return ['pass', 'fail', 'cancelled'].includes(value.state) && value;
    }, 'QuickJS comparison completion', 185000);
    await navigate('?quickjs-suite=custom');
    userAgent = await evaluate('navigator.userAgent');
    assert.equal(await evaluate("document.getElementById('quickjs-status').getAttribute('role')"), 'status');
    assert.equal((await state()).state, 'idle');
    assert.equal((await state()).resultsLinkHidden, true);
    assert.deepEqual(await evaluate(`performance.getEntriesByType('resource').filter(r => /quickjs.*(?:wasm|bin)$/.test(r.name)).map(r => r.name)`), []);
    record('QuickJS comparison loads on demand at a nested path');
    await click('quickjs-run');
    let result = await finished();
    assert.equal(result.state, 'pass', result.text);
    assert.equal(result.runDisabled, false); assert.equal(result.stopDisabled, true);
    assert.equal(result.resultsLinkHidden, false, 'successful results must be discoverable beside the status');
    await click('quickjs-view-results');
    assert.equal(await evaluate('location.hash'), '#quickjs-results');
    report = await evaluate('window.quickjsReport');
    assert.ok(report?.schema, 'downloadable report must identify its schema');
    assert.ok(report?.environment, 'report must record the browser environment');
    assert.ok(report?.parameters, 'report must identify workload parameters');
    assert.equal(report.dynarmicBuild.artifacts.js.sha256, hash('arm_jit.js'));
    assert.equal(report.dynarmicBuild.artifacts.wasm.sha256, hash('arm_jit.wasm'));
    assert.deepEqual(Object.keys(report.runtimes).sort(), ['dynarmic', 'native', 'wasm']);
    const expected = {lcg: 2778886796, sieve: 1028, pi: 3141542};
    for (const [name, runtime] of Object.entries(report.runtimes)) {
      assert.equal(runtime.ok, true, `${name} must succeed`);
      assert.equal(runtime.runtime, name);
      assert.ok(Number.isFinite(runtime.runMs) && runtime.runMs >= 0, `${name} end-to-end timing`);
      assert.ok(Number.isFinite(runtime.loadMs) && runtime.loadMs >= 0, `${name} load timing`);
      const benchmark = runtime.benchmark;
      assert.equal(benchmark.profile, 'light');
      assert.deepEqual(benchmark.parameters, {lcg: 20000, sieve: 8192, pi: 20000});
      assert.deepEqual(benchmark.outputs, expected, `${name} independently checked results`);
      assert.equal(benchmark.measuredRuns, 3);
      for (const kernel of Object.keys(expected)) {
        const samples = benchmark.samplesMs[kernel];
        assert.equal(samples.length, benchmark.measuredRuns);
        assert.ok(samples.every(ms => Number.isFinite(ms) && ms >= 0), `${name}/${kernel} timing samples`);
        const sorted = [...samples].sort((a, b) => a - b);
        const summary = benchmark.summary[kernel];
        assert.equal(summary.medianMs, sorted[1]);
        assert.equal(summary.minMs, sorted[0]);
        assert.equal(summary.maxMs, sorted[2]);
      }
    }
    assert.ok(report.runtimes.dynarmic.stats, 'dynarmic compile statistics must be retained');
    record('all three actual runtimes match reference outputs and retain valid repeated statistics');
    await call('Browser.setDownloadBehavior', {behavior: 'allow', downloadPath: downloads});
    await click('quickjs-export');
    const exported = await until(() => fs.readdirSync(downloads).find(name => name.endsWith('.json')), 'JSON report download');
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(downloads, exported), 'utf8')), report);
    record('Export JSON downloads the checked measurements and environment unchanged');

    await evaluate("document.querySelectorAll('#quickjs details').forEach(element => element.open = true)");
    for (const width of [320, 375, 768, 1440]) {
      await call('Emulation.setDeviceMetricsOverride', {width, height: 900, deviceScaleFactor: 1, mobile: false});
      assert.equal(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'), true, `overflow at ${width}px`);
    }
    record('QuickJS results and statistics fit 320–1440px');

    loopGuest = true;
    await navigate('?quickjs-suite=custom'); await click('quickjs-run');
    await until(() => loopGuestServed, 'infinite ARM guest response');
    await until(() => evaluate("document.getElementById('q-badge-dynarmic').textContent === 'Executing'"), 'synchronous ARM execution');
    await pause(150);
    assert.equal(await evaluate("document.getElementById('c-bench-run').disabled"), true, 'C benchmarks must not overlap the comparison');
    assert.equal(await evaluate("document.getElementById('quickjs-suite').disabled"), true, 'the active benchmark must stay visible until stopped');
    await click('quickjs-stop');
    result = await finished();
    assert.equal(result.state, 'cancelled', result.text);
    assert.equal(result.resultsLinkHidden, true);
    assert.equal(result.runDisabled, false); assert.equal(result.stopDisabled, true);
    loopGuest = false;
    assert.equal(await evaluate("document.getElementById('quickjs-suite').disabled"), false);
    record('Stop terminates a synchronous ARM loop and restores benchmark controls');

    await navigate('?quickjs-suite=custom');
    delayWorker = true;
    await click('quickjs-run');
    await until(async () => (await state()).state === 'running', 'QuickJS startup');
    await click('quickjs-stop');
    result = await finished();
    assert.equal(result.state, 'cancelled', result.text);
    assert.equal(result.resultsLinkHidden, true);
    assert.equal(result.runDisabled, false); assert.equal(result.stopDisabled, true);
    delayWorker = false;
    record('Stop cancels pending Worker initialization and releases controls');

    broken = 'quickjs-worker.js';
    await navigate('?quickjs-suite=custom'); await click('quickjs-run');
    result = await finished();
    assert.equal(result.state, 'fail', result.text);
    assert.equal(result.resultsLinkHidden, true);
    assert.equal(result.runDisabled, false); assert.equal(result.stopDisabled, true);
    broken = null;
    record('a missing QuickJS Worker reports failure and releases controls');
    await click('quickjs-run');
    result = await finished();
    assert.equal(result.state, 'pass', result.text);
    assert.equal(result.resultsLinkHidden, false);
    record('retry after an asset failure runs all three runtimes successfully');
    for (const name of artifacts) assert.equal(hash(name), artifactHashes[name], `${name} changed during verification`);
  });
  console.log(`PASS QuickJS Chromium (${checks.length} checks): ${userAgent}`);
} catch (error) {
  console.error(error.message); process.exitCode = 1;
  checks.push({error: error.message});
} finally {
  fs.rmSync(downloads, {recursive: true, force: true});
  if (extra.length === 2) {
    try {
      fs.writeFileSync(extra[1], JSON.stringify({browser: userAgent, artifactHashes, checks, report}, null, 2) + '\n', {flag: 'wx'});
    } catch (error) { console.error(`Could not write report: ${error.message}`); process.exitCode = 1; }
  }
}
