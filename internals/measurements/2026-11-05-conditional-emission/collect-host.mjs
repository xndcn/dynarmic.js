import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {cases,methodology,median,parseHostOutput,summarize,validateReport} from '../../../tests/bench/protocol.mjs';
import {compareReports} from '../../../tests/bench/compare.mjs';
import {hashFile,readBuildInfo,sourceInfo,systemInfo} from '../../../tests/bench/environment.mjs';
const [before,after,output]=process.argv.slice(2);
assert(before&&after&&output&&!fs.existsSync(output));
assert(!process.env.NODE_OPTIONS&&!process.env.DYN_DUMP_DIR);
const dirs=[before,after].map(p=>path.resolve(p));
const builds=dirs.map(p=>readBuildInfo(path.dirname(p)));
assert.deepEqual(builds[0].build,builds[1].build);
assert.equal(builds[0].build.profile,false);
const specs=[...cases.map(c=>({...c,flags:[],standard:true})),
 ...[0,1,2].map(mode=>({name:`literal-${mode}`,file:'a32_region_alias_test.js',argv:['literal',String(mode)],flags:[]})),
 {name:'literal-1-liftoff',file:'a32_region_alias_test.js',argv:['literal','1'],flags:['--liftoff-only']},
 {name:'literal-1-turbofan',file:'a32_region_alias_test.js',argv:['literal','1'],flags:['--no-liftoff']},
 {name:'thumb-boundaries',file:'a32_region_alias_test.js',argv:['1','1','1'],flags:[]},
 {name:'board-call',file:'jit_callbench.js',argv:['board'],flags:[]},
 {name:'board-memory-1',file:'a32_region_alias_test.js',argv:['1','1','loop','memory'],flags:[]}];
const identity=()=>dirs.map(dir=>{
 const result={};
 for(const stem of new Set(specs.map(s=>s.file))) for(const file of [stem,stem.replace(/\.js$/,'.wasm')]) result[file]=hashFile(path.join(dir,file));
 return result;
});
const report={timestamp:new Date().toISOString(),...systemInfo(),build:builds[0].build,
 git:sourceInfo(builds[0].sourceRoot),rounds:15,artifacts:identity(),trials:[],summary:[],status:'running'};
const save=()=>fs.writeFileSync(output,JSON.stringify(report,null,2)+'\n');
try {
 for(let round=0;round<report.rounds;++round) {
  for(let j=0;j<specs.length;++j) {
   const spec=specs[(j+round)%specs.length];
   for(const side of round%2?[1,0]:[0,1]) {
    const cmd=[...spec.flags,path.join(dirs[side],spec.file),...spec.argv];
    const p=spawnSync(process.execPath,cmd,{encoding:'utf8',timeout:60000,maxBuffer:16*1024*1024});
    const trial={round,side,name:spec.name,cmd,status:p.status,stdout:p.stdout,stderr:p.stderr};
    report.trials.push(trial);assert.equal(p.status,0,p.stderr||p.stdout);
    if(spec.file==='a32_region_alias_test.js') {
     assert(!/WRONG|FAIL/.test(p.stdout));
     trial.samples=p.stdout.split('\n').filter(l=>l.startsWith('REGION_SAMPLE ')).map(l=>JSON.parse(l.slice(14)));
     assert.deepEqual(trial.samples.map(s=>s.sample),[0,1,2,3,4,5,6,7,8]);
     assert(trial.samples.slice(1).every(s=>s.blocks===0&&s.bytes===0));
     if(spec.name.startsWith('literal')) assert(trial.samples.every(s=>s.instructions===1572865));
    } else trial.samples=parseHostOutput(p.stdout);
    trial.warmMs=median(trial.samples.slice(1).map(s=>s.ms));
   }
  }
  save();console.log(`round ${round+1}/${report.rounds}`,{trials:report.trials.length});
 }
 assert.deepEqual(identity(),report.artifacts);
 // Keep the existing default-suite acceptance calculation intact.
 const standardReports=dirs.map((dir,side)=>({schema:'dynarmic.js/benchmark-v2',timestamp:report.timestamp,
  ...systemInfo(),git:report.git,build:report.build,methodology,
  cases:cases.map(spec=>{
   const samples=report.trials.filter(t=>t.side===side&&t.name===spec.name).flatMap(t=>t.samples.map(s=>({...s,process:t.round})));
   const root=builds[side].sourceRoot;
   const workload=fs.readFileSync(path.join(root,'tests/hosts',spec.file.replace(/\.js$/,'.cpp')));
   // Hash concatenated bytes exactly like the ordinary runner.
   const item={...spec,runs:report.rounds,samples,
    workloadSha256:null,artifact:{jsSha256:hashFile(path.join(dir,spec.file)),wasmSha256:hashFile(path.join(dir,spec.file.replace(/\.js$/,'.wasm')))},summary:summarize(samples)};
   return {item,workload,root};
  })}));
 const {createHash}=await import('node:crypto');
 for(const r of standardReports) {
  r.cases=r.cases.map(({item,workload,root})=>({...item,workloadSha256:createHash('sha256').update(workload).update(fs.readFileSync(path.join(root,'tests/hosts/benchmark.h'))).digest('hex')}));
  validateReport(r);
 }
 report.defaultReports=standardReports;report.defaultGate=compareReports(...standardReports);
 // Deterministic bootstrap over paired process medians, retaining every sample.
 let seed=0xa18df34;
 const random=()=>{seed^=seed<<13;seed^=seed>>>17;seed^=seed<<5;return (seed>>>0)/4294967296;};
 for(const spec of specs) {
  const trials=report.trials.filter(t=>t.name===spec.name);
  const ratios=Array.from({length:report.rounds},(_,i)=>trials.find(t=>t.round===i&&t.side===1).warmMs/trials.find(t=>t.round===i&&t.side===0).warmMs);
  const boot=Array.from({length:10000},()=>median(Array.from({length:ratios.length},()=>ratios[Math.floor(random()*ratios.length)]))).sort((a,b)=>a-b);
  report.summary.push({name:spec.name,ratio:median(ratios),ci95:[boot[250],boot[9749]],ratios,
   before:median(trials.filter(t=>t.side===0).map(t=>t.warmMs)),after:median(trials.filter(t=>t.side===1).map(t=>t.warmMs)),
   bytes:[0,1].map(side=>median(trials.filter(t=>t.side===side).map(t=>t.samples[0].compile?.bytes??t.samples[0].bytes))),
   coldMs:[0,1].map(side=>median(trials.filter(t=>t.side===side).map(t=>t.samples[0].ms)))});
 }
 report.status='complete';
} finally {save();}
