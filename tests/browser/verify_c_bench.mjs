#!/usr/bin/env node
// Real Chromium execution of every fixed C workload on all three paths.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {createRequire} from 'node:module';
import {withChromium, pause} from './chromium.mjs';
const P = createRequire(import.meta.url)('../../examples/browser/c-bench-protocol.js');
const [browser, directory = 'build-em/examples/browser/dist', ...extra] = process.argv.slice(2);
assert(browser && (!extra.length || extra.length === 2 && extra[0] === '--report'),
  'Usage: verify_c_bench.mjs <chrome-executable> [dist] [--report output.json]');
const root = path.resolve(directory), checks = [], reports = {};
const manifest = P.validateManifest(JSON.parse(fs.readFileSync(path.join(root, 'c-bench-manifest.json'))));
const reference = P.validateReference(JSON.parse(fs.readFileSync(path.join(root, 'c-bench-reference.json'))));
const names = [...manifest.artifacts.map(x => x.path), 'c-bench-manifest.json', 'c-bench-app.js', 'c-bench-worker.js',
  'c-bench-protocol.js', 'index.html', 'styles.css', 'arm_jit.js', 'arm_jit.wasm', 'wasm-build-manifest.json'];
const hash = name => createHash('sha256').update(fs.readFileSync(path.join(root, name))).digest('hex');
const hashes = Object.fromEntries(names.map(name => [name, hash(name)]));
const downloads = fs.mkdtempSync(path.join(os.tmpdir(), 'c-bench-export-'));
let broken = null, browserVersion;
const record = text => {checks.push(text); console.log('PASS ' + text);};
try {
  await withChromium({browser, root, serve: ({name, response}) => {
    if (name === broken) {response.writeHead(404).end(); return true;}
    return false;
  }}, async ({call, evaluate, navigate, until}) => {
    const click = id => evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);
    const state = () => evaluate("({state:document.getElementById('c-bench-status').dataset.state,text:document.getElementById('c-bench-status').textContent,disabled:document.getElementById('c-bench-run').disabled})");
    const finished = () => until(async () => {
      const s = await state(); return ['pass','fail','cancelled','invalid'].includes(s.state) && !s.disabled && s;
    }, 'C benchmark terminal state', 1205000);
    await navigate(); browserVersion = await evaluate('navigator.userAgent');
    await until(async () => !(await state()).disabled, 'C benchmark controls');
    assert.equal(await evaluate("document.getElementById('c-bench-status').getAttribute('role')"), 'status');
    assert.deepEqual(await evaluate("performance.getEntriesByType('resource').filter(e=>/c-bench.*(?:bin|wasm|elf)$/.test(e.name)).map(e=>e.name)"), []);
    record('C benchmark engines load on demand at a nested path');
    for (const profile of ['quick', 'full']) {
      await evaluate(`document.getElementById('c-bench-profile').value=${JSON.stringify(profile)};document.getElementById('c-bench-case').value='all'`);
      await click('c-bench-run');
      let progress = '';
      await until(async () => {
        const s = await state(); if (s.text !== progress) {progress = s.text; console.log(progress);}
        return s.state !== 'running' && !s.disabled;
      }, profile + ' complete comparison', 1205000);
      const s = await state(), r = await evaluate('window.cBenchReport');
      reports[profile] = r;
      assert.equal(s.state, 'pass', s.text); assert.equal(r.status, 'pass'); assert.equal(r.verified, true);
      assert.equal(r.environment.visibilityChanged, false); assert.equal(r.observations.length, 45);
      assert.deepEqual(r.selectedCases, P.CASE_IDS); assert.deepEqual(r.reference, reference); assert.deepEqual(r.build, manifest);
      for (const entry of r.observations) {
        assert.equal(entry.status, 'pass', JSON.stringify(entry));
        P.validateResult(entry.result, entry.task, reference);
        if (entry.task.runtime === 'v86') assert(entry.result.stats.compiledSlots > 0, 'v86 JIT must generate code for these workloads');
      }
      for (const caseId of P.CASE_IDS) for (const runtime of P.RUNTIMES) {
        const items = r.observations.filter(x => x.task.caseId === caseId && x.task.runtime === runtime);
        assert.deepEqual(items.map(x => x.task.round).sort(), [0,1,2]);
        assert.deepEqual(items.map(x => x.task.position).sort(), [0,1,2]);
      }
      const shared = P.summarize(r.observations.map(x => x.result)); assert.equal(shared.complete, true);
      for (const id of P.CASE_IDS) for (const runtime of P.RUNTIMES)
        assert.deepEqual(r.summary.cases[id].runtimes[runtime].timing, shared.cases[id].runtimes[runtime].warmMs);
      reports[profile] = r;
      record(profile + ': all five kernels, three runtimes and three fresh-engine rounds pass independent checksums');
    }
    await call('Browser.setDownloadBehavior', {behavior:'allow', downloadPath:downloads});
    await click('c-bench-export');
    const exported = await until(() => fs.readdirSync(downloads).find(n => n.endsWith('.json')), 'C benchmark JSON export');
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(downloads, exported))), reports.full);
    record('JSON export retains every first/warm/measured sample, input and build identity');
    for (const [label,width,dark] of [['desktop',1440,false],['mobile',390,false],['small',320,false],['dark',1440,true]]) {
      await call('Emulation.setDeviceMetricsOverride', {width,height:1000,deviceScaleFactor:1,mobile:false});
      await call('Emulation.setEmulatedMedia', {features:[{name:'prefers-color-scheme',value:dark?'dark':'light'}, {name:'prefers-reduced-motion',value:'reduce'}]});
      await pause(100);
      await evaluate("document.getElementById('c-bench').scrollIntoView({behavior:'instant'})");
      assert.equal(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'), true, `overflow at ${width}`);
      if (extra.length) {
        const clip = await evaluate("(()=>{const r=document.getElementById('c-bench').getBoundingClientRect();return{x:r.left+scrollX,y:r.top+scrollY,width:r.width,height:r.height,scale:1};})()");
        const shot = await call('Page.captureScreenshot',{format:'png',captureBeyondViewport:true,clip});
        fs.writeFileSync(extra[1].replace(/\.json$/, '') + '-' + label + '.png', Buffer.from(shot.data,'base64'));
      }
    }
    await evaluate("document.querySelectorAll('#c-bench-results details').forEach(e=>e.open=true)");
    await call('Emulation.setDeviceMetricsOverride',{width:320,height:900,deviceScaleFactor:1,mobile:false});
    assert.equal(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'), true);
    record('results and expanded provenance fit 320–1440px in light and dark themes');
    await navigate(); await click('c-bench-run');
    await until(() => evaluate('window.cBenchRunning === true'), 'C comparison active');
    assert.equal(await evaluate("['quickjs-run','official-run','official-run-all'].every(id=>document.getElementById(id).disabled)"), true);
    await click('c-bench-stop'); assert.equal((await finished()).state, 'cancelled');
    record('Stop terminates work, retains incomplete status and releases all lab controls');
    await click('c-bench-run');
    await until(() => evaluate('window.cBenchRunning === true'), 'comparison before hidden tab');
    await evaluate("Object.defineProperty(document,'hidden',{configurable:true,get:()=>true});document.dispatchEvent(new Event('visibilitychange'))");
    assert.equal((await finished()).state, 'invalid');
    const invalid = await evaluate('window.cBenchReport');
    assert.equal(invalid.verified, false); assert.equal(invalid.summary.v86OverArm, null); assert.equal(invalid.summary.armOverDirect, null);
    await evaluate('delete document.hidden');
    record('tab visibility loss invalidates timings and removes aggregate ratios');
    broken = 'c-bench-arm.bin';
    await navigate(); await evaluate("document.getElementById('c-bench-case').value='integer'"); await click('c-bench-run');
    assert.equal((await finished()).state, 'fail');
    const failed = await evaluate('window.cBenchReport'); assert(failed.observations.some(x => x.status === 'fail'));
    assert.equal(failed.summary.v86OverArm, null); broken = null;
    await click('c-bench-run'); assert.equal((await finished()).state, 'pass');
    record('missing binary is a visible failure and retry runs all three paths successfully');
    for (const name of names) assert.equal(hash(name), hashes[name], name + ' changed during verification');
  });
  console.log(`PASS C benchmark Chromium (${checks.length} checks)`);
} catch (error) {checks.push({error:error.message});console.error(error.stack || error);process.exitCode=1;}
finally {
  fs.rmSync(downloads,{recursive:true,force:true});
  if(extra.length)fs.writeFileSync(extra[1],JSON.stringify({browser:browserVersion,artifactHashes:hashes,checks,reports},null,2)+'\n',{flag:'wx'});
}
