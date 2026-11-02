import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {withChromium} from '../tests/browser/chromium.mjs';
const root=path.resolve('build-em/examples/browser/dist');
const hash=n=>createHash('sha256').update(fs.readFileSync(path.join(root,n))).digest('hex');
const names=['quickjs-arm.bin','quickjs-wasm.js','quickjs-wasm.wasm','arm_bench_jit.js','arm_bench_jit.wasm','quickjs-manifest.json'];
const artifacts=Object.fromEntries(names.map(n=>[n,hash(n)]));
const manifest=JSON.parse(fs.readFileSync(path.join(root,'quickjs-manifest.json')));
let report,browser;
await withChromium({browser:'/usr/bin/google-chrome',root},async({evaluate,navigate,until})=>{
 await navigate();browser=await evaluate('navigator.userAgent');
 await until(()=>evaluate("!document.getElementById('official-run').disabled"),'inventory');
 assert.equal(await evaluate("document.getElementById('official-case').value"),'Richards/Richards');
 await evaluate("document.getElementById('official-run').click()");
 await until(()=>evaluate("['pass','fail','cancelled'].includes(document.getElementById('official-status').dataset.state)"),'Richards three engines',180000);
 report=await evaluate('window.officialBenchReport');
 assert.equal(report.status,'pass');
 assert.deepEqual(report.selectedCases,['Richards/Richards']);
 assert.deepEqual(report.quickjsBuild,manifest);
 const entries=report.cases['Richards/Richards'];
 assert.deepEqual(Object.keys(entries).sort(),['dynarmic','native','wasm']);
 for(const [name,e] of Object.entries(entries)){
  assert.equal(e.status,'pass');assert.equal(e.result.runtime,name);
  assert.equal(e.result.official.mode,'upstream');assert.equal(e.result.official.minimumRuns,32);
  assert(e.result.official.runs[0]>=32);assert(e.result.official.elapsedMs[0]>=1000);
 }
 assert.equal(report.dynarmicBuild.artifacts.wasm.sha256,hash('arm_bench_jit.wasm'));
 assert.equal(await evaluate("document.getElementById('official-export').disabled"),false);
});
for(const n of names)assert.equal(hash(n),artifacts[n]);
fs.writeFileSync('build-quickjs-arm-tuning/checks/final-official-chromium.json',JSON.stringify({browser,artifacts,report,verified:true},null,2)+'\n',{flag:'wx'});
console.log('PASS final official Richards on Chromium native / direct QuickJS Wasm / dynarmic; current manifest retained');
