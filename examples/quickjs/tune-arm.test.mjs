import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {CASES,FIXED,parseArgs,makePlan,inspectVariants,validateObservation,summarize,acquireLock,runExperiment,recoverResultTemps,verifyCollector} from './tune-arm.mjs';
import {analyze} from './tune-arm-analyze.mjs';

function fixture() {
  const root=fs.mkdtempSync(path.join(os.tmpdir(),'quickjs-tuning-test-'));
  const variants=['baseline','candidate'].map((label,index)=>({label,dist:path.join(root,label)}));
  for(const {label,dist} of variants) {
    fs.mkdirSync(dist);
    for(const name of FIXED)fs.writeFileSync(path.join(dist,name),'fixed '+name);
    fs.writeFileSync(path.join(dist,'official-benchmarks.json'),JSON.stringify({cases:CASES.map(id=>({id}))}));
    fs.writeFileSync(path.join(dist,'quickjs-arm.bin'),label);
    const record=name=>{const b=fs.readFileSync(path.join(dist,name));return {path:name,bytes:b.length,sha256:createHash('sha256').update(b).digest('hex')};};
    fs.writeFileSync(path.join(dist,'quickjs-manifest.json'),JSON.stringify({schema:'dynarmic.js/quickjs-build-v1',configuration:{label},artifacts:[...FIXED,'quickjs-arm.bin'].map(record)}));
    for(const [host,file] of [['arm_jit','wasm-build-manifest.json'],['arm_bench_jit','arm-bench-build-manifest.json']])
      fs.writeFileSync(path.join(dist,file),JSON.stringify({schema:'dynarmic.js/wasm-coremark-host-build-v1',build:{type:'Release',profile:false},configuration:{regionDiagnostics:false},artifacts:{js:record(host+'.js'),wasm:record(host+'.wasm')}}));
  }
  return {root,variants,cleanup:()=>fs.rmSync(root,{recursive:true,force:true})};
}
const raw=(caseId,ms=2)=>({schema:'dynarmic.js/official-v8-node-v1',verified:true,reports:[{runtime:'dynarmic',caseId,status:'pass',runMs:2200,loadMs:10,stats:{blocks:2,bytes:40,compileMs:3},official:{caseId,status:'pass',mode:'upstream',version:'7',warmupMs:1000,measureMs:1000,minimumRuns:32,runs:[1000],elapsedMs:[ms*1000],samplesMs:[ms]}}]});
const args=(variants,output,extra=[])=>parseArgs([...variants.flatMap(v=>['--variant',v.label+'='+v.dist]),'--cases',CASES[0],'--output',output,...extra]);

test('tuning options reject duplicate labels, unsupported cases and output-less experiments',()=>{
  assert.throws(()=>parseArgs(['--variant','a=/tmp/a','--variant','a=/tmp/b','--output','/tmp/out']),/Duplicate variant/);
  assert.throws(()=>parseArgs(['--variant','a=/tmp/a','--variant','b=/tmp/b','--output','/tmp/out','--cases','madeup']),/case selection/);
  assert.throws(()=>parseArgs(['--variant','a=/tmp/a','--variant','b=/tmp/b']),/output/);
});
test('plan covers every requested task once and alternates both variants within each repeated case',()=>{
  const variants=[{label:'a'},{label:'b'}],plan=makePlan(variants,CASES,3);
  assert.equal(plan.length,60);assert.equal(new Set(plan.map(t=>t.id)).size,60);
  for(const caseId of CASES) {
    const first=plan.find(t=>t.caseId===caseId&&t.round===0),second=plan.find(t=>t.caseId===caseId&&t.round===1);
    assert.deepEqual(second.order,[...first.order].reverse());
  }
});
test('host, direct Wasm and source differences are rejected while the ARM guest and manifest may vary',()=>{
  const f=fixture();try {
    const input=inspectVariants(f.variants);assert.notEqual(input[0].artifacts['quickjs-arm.bin'].sha256,input[1].artifacts['quickjs-arm.bin'].sha256);
    fs.appendFileSync(path.join(f.variants[1].dist,'official-v8.js'),'changed');
    assert.throws(()=>inspectVariants(f.variants),/Stale artifact/);
    const filename=path.join(f.variants[1].dist,'quickjs-manifest.json'),manifest=JSON.parse(fs.readFileSync(filename,'utf8'));
    const bytes=fs.readFileSync(path.join(f.variants[1].dist,'official-v8.js'));
    Object.assign(manifest.artifacts.find(item=>item.path==='official-v8.js'),{bytes:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')});
    fs.writeFileSync(filename,JSON.stringify(manifest));assert.throws(()=>inspectVariants(f.variants),/Fixed input differs/);
  }finally{f.cleanup();}
});

test('failed observations remain terminal on resume and original evidence is never replaced',async()=>{
  const f=fixture(),output=path.join(f.root,'results');let calls=0;
  const oldLog=console.log,oldError=console.error;console.log=()=>{};console.error=()=>{};
  const execute=async(_node,argv,attempt)=>{
    calls++;fs.writeFileSync(path.join(attempt,'stdout.log'),'');fs.writeFileSync(path.join(attempt,'stderr.log'),'intentional test failure');
    return {exitCode:1,signal:null,interrupted:false};
  };
  try{
    const options=args(f.variants,output);const first=await runExperiment(options,{execute});
    assert.equal(first.complete,true);assert.equal(first.verified,false);assert.equal(first.failed,2);
    const before=fs.readFileSync(path.join(output,'results','r0-c0-baseline.json'));
    await runExperiment({...options,resume:true},{execute});assert.equal(calls,2);
    assert.deepEqual(fs.readFileSync(path.join(output,'results','r0-c0-baseline.json')),before);
  }finally{process.exitCode=0;console.log=oldLog;console.error=oldError;f.cleanup();}
});
test('raw official iteration counts are retained and inconsistent normalized times fail',()=>{
  const task={caseId:CASES[0]},good=raw(task.caseId);
  assert.equal(validateObservation(task,good).metrics.msPerIteration,2);
  good.reports[0].official.runs[0]=100;
  assert.throws(()=>validateObservation(task,good),/normalized timing/);
});
test('paired analysis uses process rounds and equal suite weights rather than counting iterations as repeats',()=>{
  const variants=[{label:'base',artifacts:{'quickjs-arm.bin':{bytes:10,sha256:'a'}}},{label:'candidate',artifacts:{'quickjs-arm.bin':{bytes:12,sha256:'b'}}}];
  const plan=makePlan(variants,CASES,3),spec={variants,plan,cases:CASES,repeats:3,baseline:'base'};
  const observations=plan.map(task=>({task,status:'pass',measurement:{metrics:{msPerIteration:task.variant==='base'?2:1},runMs:2000,stats:{compileMs:1}}}));
  const summary=summarize(spec,observations);assert.equal(summary.verified,true);
  assert.equal(summary.variants.candidate.eightSuiteWeightedSpeedup,2);
  assert.equal(summary.variants.candidate.cases[CASES[0]].metrics.msPerIteration.pairs.length,3);
  assert.equal(summarize(spec,observations.slice(1)).verified,false);
  assert.equal(summarize(spec,observations.slice(1)).variants.candidate.eightSuiteWeightedSpeedup,null);
});
test('live parent or orphan child blocks resume instead of starting a competing measurement',()=>{
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'quickjs-lock-test-'));try {
    fs.mkdirSync(path.join(dir,'attempts'));const release=acquireLock(dir);
    assert.throws(()=>acquireLock(dir),/still active/);release();
    const attempt=path.join(dir,'attempts','orphan');fs.mkdirSync(attempt);
    fs.writeFileSync(path.join(attempt,'process.json'),JSON.stringify({pid:process.pid,start:null}));
    assert.throws(()=>acquireLock(dir),/child PID.*still active/);
  }finally{fs.rmSync(dir,{recursive:true,force:true});}
});
test('interrupted experiments preserve finished observations and resume only missing tasks without overwriting evidence',async()=>{
  const f=fixture(),output=path.join(f.root,'results');let called=0;
  const oldLog=console.log,oldError=console.error;console.log=()=>{};console.error=()=>{};
  const execute=async(_node,argv,attempt)=>{
    called++;fs.writeFileSync(path.join(attempt,'stdout.log'),'');fs.writeFileSync(path.join(attempt,'stderr.log'),'');
    if(called===2){process.emit('SIGINT');return {exitCode:null,signal:'SIGTERM',interrupted:true};}
    const caseId=argv[argv.indexOf('--case')+1];fs.writeFileSync(argv[argv.indexOf('--report')+1],JSON.stringify(raw(caseId)));
    return {exitCode:0,signal:null,interrupted:false};
  };
  try {
    const options=args(f.variants,output);const first=await runExperiment(options,{execute});
    assert.equal(first.completed,1);assert.equal(first.complete,false);
    const original=fs.readFileSync(path.join(output,'results','r0-c0-baseline.json'));
    const completed=await runExperiment({...options,resume:true},{execute});
    assert.equal(completed.verified,true);assert.equal(called,3);
    const analysis=analyze(output);assert.equal(analysis.summary.verified,true);
    assert.match(analysis.markdown,/fresh Node process/);assert.equal(analysis.csv.trim().split('\n').length,3);
    assert.deepEqual(fs.readFileSync(path.join(output,'results','r0-c0-baseline.json')),original);
    await runExperiment({...options,resume:true},{execute});assert.equal(called,3);
    await assert.rejects(runExperiment(options,{execute}),/already exists/);
    await assert.rejects(runExperiment({...options,resume:true,repeats:2},{execute}),/Resume inputs/);
  }finally{process.exitCode=0;console.log=oldLog;console.error=oldError;f.cleanup();}
});


test('paired suite aggregates preserve each process round before taking the median',()=>{
  const variants=['base','candidate'].map(label=>({label,artifacts:{'quickjs-arm.bin':{bytes:1,sha256:label}}}));
  const plan=makePlan(variants,CASES,3),spec={variants,plan,cases:CASES,repeats:3,baseline:'base'};
  const observations=plan.map(task=>{
    let ratio=1;
    if(task.caseId==='Crypto/Encrypt')ratio=[1,10,10][task.round];
    if(task.caseId==='Crypto/Decrypt')ratio=[10,1,10][task.round];
    return {task,status:'pass',measurement:{metrics:{msPerIteration:task.variant==='base'?10:10/ratio},runMs:2000,stats:{}}};
  });
  const candidate=summarize(spec,observations).variants.candidate;
  assert.ok(Math.abs(candidate.suiteSpeedups.Crypto-Math.sqrt(10))<1e-12);
  assert.ok(Math.abs(candidate.eightSuiteWeightedSpeedup-Math.pow(Math.sqrt(10),1/8))<1e-12);
  assert.equal(candidate.roundSpeedups.length,3);
  assert.equal(candidate.eightSuiteWeightedSpeedupSummary.samples.length,3);
  assert.ok(candidate.eightSuiteWeightedSpeedupSummary.max>candidate.eightSuiteWeightedSpeedupSummary.median);
  assert.ok(Math.abs(candidate.tenCaseEqualWeightedSpeedup-Math.pow(10,1/10))<1e-12);
});

test('dead-writer result temporaries are preserved separately while completed and unknown files stay untouched',()=>{
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'quickjs-recovery-test-'));
  try {
    fs.mkdirSync(path.join(directory,'results'));fs.mkdirSync(path.join(directory,'attempts'));
    const spec={plan:[{id:'r0-c0-base'},{id:'r0-c0-next'}]},release=acquireLock(directory);
    fs.writeFileSync(path.join(directory,'results','r0-c0-base.json'),'completed');
    const temporary='r0-c0-next.json.tmp-2147483647-1';
    fs.writeFileSync(path.join(directory,'results',temporary),'incomplete original bytes');
    const recovered=recoverResultTemps(directory,spec);
    assert.equal(recovered.length,1);assert.equal(fs.readFileSync(path.join(directory,recovered[0]),'utf8'),'incomplete original bytes');
    assert.equal(fs.readFileSync(path.join(directory,'results','r0-c0-base.json'),'utf8'),'completed');
    assert.equal(fs.existsSync(path.join(directory,'results','r0-c0-next.json')),false);
    const live='r0-c0-next.json.tmp-'+process.pid+'-2';fs.writeFileSync(path.join(directory,'results',live),'live');
    assert.throws(()=>recoverResultTemps(directory,spec),/still active/);
    assert.equal(fs.readFileSync(path.join(directory,'results',live),'utf8'),'live');
    fs.unlinkSync(path.join(directory,'results',live));
    fs.writeFileSync(path.join(directory,'results','unknown.json'),'retain');
    assert.throws(()=>recoverResultTemps(directory,spec),/Unexpected result file/);
    assert.equal(fs.readFileSync(path.join(directory,'results','unknown.json'),'utf8'),'retain');release();
  }finally{fs.rmSync(directory,{recursive:true,force:true});}
});

test('per-task collector validation rejects changed benchmark runner fingerprints',()=>{
  assert.throws(()=>verifyCollector({'official-run.mjs':{sha256:'changed',bytes:1}}),/Collector or benchmark runner changed/);
});
