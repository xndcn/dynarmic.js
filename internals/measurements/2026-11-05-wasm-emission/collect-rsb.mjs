// Matched board call/return workload; artifacts must be frozen before timing.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {hashFile, readBuildInfo, systemInfo} from '../../../tests/bench/environment.mjs';
import {parseHostOutput} from '../../../tests/bench/protocol.mjs';

const [before, after, output, count = '25'] = process.argv.slice(2);
const rounds = Number(count), dirs = [before, after].map(dir => path.resolve(dir));
assert(output && !fs.existsSync(output));
assert(Number.isInteger(rounds) && rounds >= 5 && rounds <= 100);
assert(!process.env.DYN_DUMP_DIR && !process.env.NODE_OPTIONS);
const builds = dirs.map(dir => readBuildInfo(path.dirname(dir)).build);
assert.deepEqual(builds[0], builds[1]);
assert.equal(builds[0].profile, false); assert.equal(builds[0].regionDiagnostics, false);
const median = xs => {const a = [...xs].sort((a,b)=>a-b); return (a[Math.floor(a.length/2)]+a[Math.floor((a.length-1)/2)])/2;};
const identity = () => dirs.map(dir => Object.fromEntries(['.js','.wasm'].map(ext =>
  [ext,hashFile(path.join(dir,'jit_callbench'+ext))])));
const report = {schema:'dynarmic.js/board-rsb-comparison-v1',timestamp:new Date().toISOString(),
  ...systemInfo(),build:builds[0],artifacts:identity(),collectorSha256:hashFile(new URL(import.meta.url)),
  workloadSha256:hashFile('tests/hosts/jit_callbench.cpp'),
  methodology:{rounds,freshProcesses:true,alternatingOrder:true,warmSamples:8,
    certifiedSteadyState:false,board:true,cycleCounting:true,regionMultiEntry:true,
    instructions:5242883,validation:'r0 equals 1048576, SVC halt, unchanged instruction stream and zero warm compilation'},trials:[]};
try {
  for(let round=0; round<rounds; ++round) for(const variant of round%2 ? [1,0] : [0,1]) {
    const argv = [path.join(dirs[variant],'jit_callbench.js'),'board'];
    const result = spawnSync(process.execPath,argv,{encoding:'utf8',timeout:60000});
    const trial = {round,variant:variant ? 'after' : 'before',argv,status:result.status,
      signal:result.signal,stdout:result.stdout,stderr:result.stderr};
    report.trials.push(trial);
    assert.equal(result.status,0,result.error?.message || result.stderr);
    trial.samples = parseHostOutput(result.stdout);
    assert(trial.samples.every(s=>s.instructions===5242883));
    trial.warmMs = median(trial.samples.slice(1).map(s=>s.ms));
    console.log(`${round} ${trial.variant} warm=${trial.warmMs.toFixed(3)}`);
  }
  assert.deepEqual(identity(),report.artifacts,'artifacts changed');
  for(const variant of ['before','after']) {
    const trials = report.trials.filter(t=>t.variant===variant);
    report[variant] = {warmMs:median(trials.map(t=>t.warmMs)),
      coldMs:median(trials.map(t=>t.samples[0].ms)),bytes:median(trials.map(t=>t.samples[0].compile.bytes))};
  }
  report.pairedTimeRatios = Array.from({length:rounds},(_,i)=>
    report.trials.find(t=>t.round===i&&t.variant==='after').warmMs / report.trials.find(t=>t.round===i&&t.variant==='before').warmMs);
  report.pairedTimeRatio = median(report.pairedTimeRatios);
  report.status='complete';
} catch(error) {report.status='failed';report.error=String(error);throw error;}
finally {fs.writeFileSync(output,JSON.stringify(report,null,2)+'\n',{flag:'wx'});}
