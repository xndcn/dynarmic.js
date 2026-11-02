#!/usr/bin/env node
// Run the ordinary Pages comparison in independent Chromium processes.
// Collection preserves complete page reports; analysis uses browser sessions
// as the outer repetition unit, rather than counting inner calls as repeats.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {withChromium} from '../../tests/browser/chromium.mjs';
const P = createRequire(import.meta.url)('../browser/c-bench-protocol.js');
const options = {browser:'/usr/bin/google-chrome', dist:'build-em/examples/browser/dist', sessions:5};
for (let i = 2; i < process.argv.length; i += 2) {
  const key = process.argv[i].slice(2), value = process.argv[i + 1];
  assert(['browser','dist','sessions','output'].includes(key) && value, 'Invalid argument');
  options[key] = key === 'sessions' ? Number(value) : value;
}
assert(options.output && Number.isSafeInteger(options.sessions) && options.sessions >= 3 && options.sessions <= 20);
const output = path.resolve(options.output), dist = path.resolve(options.dist);
assert(!fs.existsSync(output), 'Output exists; retain it and use a new directory');
const digest = name => {const b=fs.readFileSync(name);return {bytes:b.length,sha256:createHash('sha256').update(b).digest('hex')};};
const read = name => JSON.parse(fs.readFileSync(path.join(dist,name),'utf8'));
const build = P.validateManifest(read('c-bench-manifest.json')), reference = P.validateReference(read('c-bench-reference.json'));
const host = read('wasm-build-manifest.json');
assert.equal(host.build.type,'Release'); assert.equal(host.build.profile,false); assert.equal(host.configuration.regionDiagnostics,false);
const names = [...build.artifacts.map(a=>a.path),'c-bench-manifest.json','wasm-build-manifest.json','arm_jit.js','arm_jit.wasm',
  'c-bench-app.js','c-bench-worker.js','c-bench-protocol.js','index.html','styles.css'];
const files = () => Object.fromEntries(names.map(name=>[name,digest(path.join(dist,name))]));
const artifacts = files();
for (const a of build.artifacts) assert.deepEqual(artifacts[a.path],{bytes:a.bytes,sha256:a.sha256},a.path);
for(const [kind,ext] of [['js','.js'],['wasm','.wasm']])assert.deepEqual(artifacts['arm_jit'+ext],{bytes:host.artifacts[kind].bytes,sha256:host.artifacts[kind].sha256});
fs.mkdirSync(output,{recursive:true});
const write = (name,data) => fs.writeFileSync(path.join(output,name),JSON.stringify(data,null,2)+'\n',{flag:'wx'});
const collectorPaths = [fileURLToPath(import.meta.url), fileURLToPath(new URL('../../tests/browser/chromium.mjs',import.meta.url)),
  fileURLToPath(new URL('../browser/c-bench-protocol.js',import.meta.url))];
const scriptHashes = () => Object.fromEntries(collectorPaths.map(f=>[f,digest(f)]));
const collector = scriptHashes();
write('experiment.json',{schema:'dynarmic.js/c-bench-performance-v1',createdAt:new Date().toISOString(),sessions:options.sessions,
  profile:'full',dist,browser:path.resolve(options.browser),artifacts,collector,build,reference,host,
  environment:{node:process.version,os:os.release(),platform:process.platform,arch:process.arch,cpu:os.cpus()[0]?.model,
    cpuCount:os.cpus().length,memoryBytes:os.totalmem(),affinity:process.platform==='linux'?fs.readFileSync('/proc/self/status','utf8').match(/^Cpus_allowed_list:\s*(.*)$/m)?.[1]:null},
  methodology:{outerUnit:'One fresh Chromium process/profile running the unchanged full page comparison',
    innerUnit:'Three balanced-order rounds; each case/runtime uses a fresh Worker with first + two warmup + three measured calls',
    summary:'Within each session pair round medians before ratio aggregation; then summarize session-level medians',
    scope:'Fixed-work scalar kernels, same Clang/O2, complete execution paths; two warmups do not establish steady state'}});
let userAgent, failures=0;
for(let session=0;session<options.sessions;session++) {
  assert.deepEqual(files(),artifacts,'Frozen page files changed'); assert.deepEqual(scriptHashes(),collector,'Collector changed');
  const item={session,status:'fail',startedAt:new Date().toISOString(),loadAverageBefore:os.loadavg()};
  try {
    console.log('RUN session',session+1,'/',options.sessions);
    await withChromium({browser:options.browser,root:dist},async({browserPid,navigate,evaluate,until})=>{
      item.browserPid=browserPid;
      await navigate('?cbench=full');
      item.userAgent=await evaluate('navigator.userAgent'); userAgent ||= item.userAgent; assert.equal(item.userAgent,userAgent);
      const report=await until(async()=>{
        const status=await evaluate('window.cBenchReport?.status');
        return status && status!=='running' ? await evaluate('window.cBenchReport') : null;
      },'full C comparison',1205000);
      item.report=report;
      assert.equal(report.schema,'dynarmic.js/c-bench-comparison-v1'); assert.equal(report.profile,'full');
      assert.equal(report.status,'pass',JSON.stringify(report.observations.filter(x=>x.status!=='pass')));
      assert.equal(report.verified,true); assert.equal(report.performanceValid,true);
      assert.equal(report.environment.visibilityChanged,false); assert.equal(report.methodology.performanceEligible,true);
      assert.deepEqual(report.build,build); assert.deepEqual(report.reference,reference); assert.deepEqual(report.dynarmicBuild,host);
      assert.deepEqual(report.selectedCases,P.CASE_IDS); assert.equal(report.observations.length,45);
      const keys=new Set();
      for(const x of report.observations) {
        assert.equal(x.status,'pass'); P.validateResult(x.result,x.task,reference);
        const key=`${x.task.round}/${x.task.caseId}/${x.task.runtime}`; assert(!keys.has(key)); keys.add(key);
        assert(P.statistics(x.result.samplesMs).median>=P.RESOLUTION_MS);
        if(x.task.runtime==='v86')assert(x.result.stats.compiledSlots>0,'v86 must use its JIT');
      }
      for(const id of P.CASE_IDS)for(const runtime of P.RUNTIMES) {
        const items=report.observations.filter(x=>x.task.caseId===id&&x.task.runtime===runtime);
        assert.deepEqual(items.map(x=>x.task.round).sort(),[0,1,2]);assert.deepEqual(items.map(x=>x.task.position).sort(),[0,1,2]);
      }
      const summary=P.summarize(report.observations.map(x=>x.result));assert(summary.complete);
      for(const id of P.CASE_IDS) {
        for(const runtime of P.RUNTIMES)assert.deepEqual(report.summary.cases[id].runtimes[runtime].timing,summary.cases[id].runtimes[runtime].warmMs);
        assert.deepEqual(report.summary.cases[id].v86OverArm,summary.cases[id].ratios.v86OverDynarmic);
        assert.deepEqual(report.summary.cases[id].armOverDirect,summary.cases[id].ratios.dynarmicOverWasm);
      }
    });
    assert.deepEqual(files(),artifacts); assert.deepEqual(scriptHashes(),collector);
    item.status='pass';
  }catch(error){item.error=error.stack||String(error);failures++;}
  item.finishedAt=new Date().toISOString();item.loadAverageAfter=os.loadavg();
  write(`session-${session}.json`,item);
  console.log(item.status.toUpperCase(),'session',session+1,item.error||'45 observations / 270 matching checksums');
}
write('completion.json',{sessions:options.sessions,failed:failures,verified:failures===0,finishedAt:new Date().toISOString()});
if(failures)process.exitCode=1;
