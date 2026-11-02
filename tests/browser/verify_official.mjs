#!/usr/bin/env node
// Complete upstream V8 v7 case coverage in real browser Workers.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {pause, withChromium} from './chromium.mjs';

const [browser, directory = 'build-em/examples/browser/dist', ...extra] = process.argv.slice(2);
if (!browser || (extra.length && !(extra.length === 2 && extra[0] === '--report'))) {
  console.error('usage: verify_official.mjs <chrome-executable> [dist-directory] [--report output.json]');
  process.exit(1);
}
const expected = ['Richards/Richards', 'DeltaBlue/DeltaBlue', 'Crypto/Encrypt', 'Crypto/Decrypt',
  'RayTrace/RayTrace', 'EarleyBoyer/Earley', 'EarleyBoyer/Boyer', 'RegExp/RegExp', 'Splay/Splay', 'NavierStokes/NavierStokes'];
const root = path.resolve(directory), checks = [];
const downloads = fs.mkdtempSync(path.join(os.tmpdir(), 'dynarmic-official-export-'));
let report, userAgent, artifactHashes = {}, broken = null;
const record = name => { checks.push(name); console.log(`PASS ${name}`); };
const hash = name => createHash('sha256').update(fs.readFileSync(path.join(root, name))).digest('hex');
try {
  const artifacts = ['index.html', 'styles.css', 'official-benchmarks-app.js', 'official-benchmarks-worker.js',
    'official-benchmarks.json', 'official-v8.js', 'official-v8-adapter.js', 'official-v8-upstream.js',
    'arm_bench_jit.js', 'arm_bench_jit.wasm', 'arm-bench-build-manifest.json',
    'quickjs-arm.bin', 'quickjs-wasm.js', 'quickjs-wasm.wasm', 'quickjs-manifest.json'];
  artifactHashes = Object.fromEntries(artifacts.map(name => [name, hash(name)]));
  await withChromium({browser, root, serve: ({name, response}) => {
    if (name === broken) { response.writeHead(404).end(); return true; }
    return false;
  }}, async ({call, evaluate, navigate, until}) => {
    const state = () => evaluate(`({state: document.getElementById('official-status').dataset.state,
      text: document.getElementById('official-status').textContent,
      runDisabled: document.getElementById('official-run').disabled,
      stopDisabled: document.getElementById('official-stop').disabled})`);
    const click = id => evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);
    await navigate();
    userAgent = await evaluate('navigator.userAgent');
    await until(async () => !(await state()).runDisabled, 'official inventory load');
    assert.deepEqual(await evaluate("[...document.querySelectorAll('#official-case option')].map(option => option.value)"), expected);
    assert.equal(await evaluate("document.getElementById('official-status').getAttribute('role')"), 'status');
    assert.deepEqual(await evaluate("performance.getEntriesByType('resource').filter(r => /arm_bench_jit|official-v8|quickjs-arm/.test(r.name)).map(r => r.name)"), []);
    record('all ten cases in eight suites are selectable without loading engines');
    await click('official-run-all');
    let lastProgress = '';
    await until(async () => {
      const current = await state();
      if (current.text !== lastProgress) { console.log(current.text); lastProgress = current.text; }
      return ['pass', 'fail', 'cancelled'].includes(current.state);
    }, 'complete official V8 run', 1805000);
    const final = await state();
    report = await evaluate('window.officialBenchReport');
    assert.equal(final.state, 'pass', final.text);
    assert.equal(final.runDisabled, false); assert.equal(final.stopDisabled, true);
    assert.equal(report.status, 'pass');
    assert.deepEqual(report.selectedCases, expected);
    assert.deepEqual(Object.keys(report.cases), expected);
    assert.equal(report.dynarmicBuild.artifacts.js.sha256, hash('arm_bench_jit.js'));
    assert.equal(report.dynarmicBuild.artifacts.wasm.sha256, hash('arm_bench_jit.wasm'));
    assert.equal(report.methodology.quickjsMemoryLimitBytes, 192 * 1048576);
    for (const caseId of expected) {
      assert.deepEqual(Object.keys(report.cases[caseId]).sort(), ['dynarmic', 'native', 'wasm']);
      for (const [runtime, entry] of Object.entries(report.cases[caseId])) {
        assert.equal(entry.status, 'pass', `${runtime}/${caseId}: ${entry.error}`);
        const result = entry.result, value = result.official;
        assert.equal(result.runtime, runtime); assert.equal(result.caseId, caseId);
        assert.equal(value.caseId, caseId); assert.equal(value.mode, 'upstream'); assert.equal(value.version, '7');
        assert.equal(value.warmupMs, 1000); assert.equal(value.measureMs, 1000); assert.equal(value.minimumRuns, 32);
        assert.equal(value.runs.length, 1); assert.ok(value.runs[0] >= 32);
        assert.ok(value.elapsedMs[0] >= 1000); assert.ok(value.samplesMs[0] > 0);
        assert.ok(Math.abs(value.samplesMs[0] - value.elapsedMs[0] / value.runs[0]) < 1e-6);
        assert.ok(result.loadMs >= 0 && result.runMs >= 0);
        if (runtime !== 'native') assert.equal(result.quickjsMemoryLimitBytes, 192 * 1048576);
      }
    }
    record('every upstream case completes on all three engines with original timing windows');
    await call('Browser.setDownloadBehavior', {behavior: 'allow', downloadPath: downloads});
    await click('official-export');
    const downloaded = await until(() => fs.readdirSync(downloads).find(name => name.endsWith('.json')), 'official JSON export');
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(downloads, downloaded), 'utf8')), report);
    record('export retains every case, timing, configuration and runtime identity');
    if (extra.length) {
      const basename = path.join(path.dirname(extra[1]), path.basename(extra[1], '.json'));
      for (const [label, width, dark] of [['desktop',1440,false],['mobile',390,false],['small',320,false],['dark',1440,true]]) {
        await call('Emulation.setDeviceMetricsOverride', {width,height:1000,deviceScaleFactor:1,mobile:false});
        await call('Emulation.setEmulatedMedia', {features:[{name:'prefers-color-scheme',value:dark?'dark':'light'}, {name:'prefers-reduced-motion',value:'reduce'}]});
        await pause(250);
        await evaluate("document.getElementById('official-benchmarks').scrollIntoView({behavior:'instant'})");
        const rectangle = await evaluate("(() => {const r=document.getElementById('official-benchmarks').getBoundingClientRect();return {x:r.left+scrollX,y:r.top+scrollY,width:r.width,height:r.height,scale:1};})()");
        const shot = await call('Page.captureScreenshot', {format:'png',captureBeyondViewport:true,clip:rectangle});
        fs.writeFileSync(`${basename}-${label}.png`,Buffer.from(shot.data,'base64'));
      }
      await call('Emulation.setEmulatedMedia',{features:[]});
    }
    await evaluate("document.querySelectorAll('#official-results details').forEach(element => element.open = true)");
    for (const width of [320, 390, 768, 1440]) {
      await call('Emulation.setDeviceMetricsOverride', {width, height: 900, deviceScaleFactor: 1, mobile: false});
      assert.equal(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'), true, `page overflow at ${width}px`);
      if (width <= 720) {
        const labels = await evaluate("[...document.querySelectorAll('#official-results .official-runtime-label')].map(label => ({text:label.textContent,width:label.getBoundingClientRect().width}))");
        assert.equal(labels.length, 30);
        assert.ok(labels.every(label => label.width > 0));
      }
    }
    record('complete result table and expanded details preserve narrow layouts');
    await click('official-run-all');
    await until(() => evaluate("!!document.querySelector('#official-results td[data-state=running]')"), 'official rerun with an active Worker');
    assert.equal(await evaluate('window.officialBenchReport'), null, 'rerun clears previous report');
    assert.equal(await evaluate("document.getElementById('c-bench-run').disabled && document.getElementById('quickjs-run').disabled"), true);
    await click('official-stop');
    await until(() => evaluate("document.getElementById('official-status').dataset.state === 'cancelled' && window.officialBenchReport?.status === 'cancelled'"), 'official cancellation');
    assert.equal(await evaluate('window.officialBenchReport.status'), 'cancelled');
    record('Stop preserves explicit incomplete statuses and releases all lab controls');
    broken = 'official-benchmarks.json';
    await navigate();
    await until(() => evaluate("document.getElementById('official-status').dataset.state === 'fail'"), 'missing inventory error');
    assert.equal((await state()).runDisabled, true);
    broken = null; await click('official-reload');
    await until(async () => !(await state()).runDisabled, 'inventory retry');
    record('missing inventory cannot start a partial suite and is recoverable');
    for (const name of artifacts) assert.equal(hash(name), artifactHashes[name], `${name} changed during verification`);
  });
  console.log(`PASS official V8 Chromium (${checks.length} checks)`);
} catch (error) {
  console.error(error.stack || error); process.exitCode = 1; checks.push({error: error.message});
} finally {
  fs.rmSync(downloads, {recursive: true, force: true});
  if (extra.length) {
    try { fs.writeFileSync(extra[1], JSON.stringify({browser: userAgent, artifactHashes, checks, report}, null, 2) + '\n', {flag: 'wx'}); }
    catch (error) { console.error(error.message); process.exitCode = 1; }
  }
}
