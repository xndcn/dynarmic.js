#!/usr/bin/env node
// Sequential, resumable ARM guest experiments. This collector changes no
// benchmark or compiler inputs; build the variants before invoking it.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath, pathToFileURL} from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const CASES = ['Richards/Richards','DeltaBlue/DeltaBlue','Crypto/Encrypt','Crypto/Decrypt',
  'RayTrace/RayTrace','EarleyBoyer/Earley','EarleyBoyer/Boyer','RegExp/RegExp','Splay/Splay','NavierStokes/NavierStokes'];
export const SCREEN = ['Richards/Richards','DeltaBlue/DeltaBlue','Crypto/Encrypt','Splay/Splay','kernels/full'];
export const FIXED = ['arm_jit.js','arm_jit.wasm','arm_bench_jit.js','arm_bench_jit.wasm',
  'quickjs-wasm.js','quickjs-wasm.wasm','official-v8.js','official-v8-adapter.js','official-benchmarks.json','quickjs-workloads.js'];
const digest = filename => {const data=fs.readFileSync(filename);return {bytes:data.length,sha256:createHash('sha256').update(data).digest('hex')};};
const median = values => {const a=[...values].sort((x,y)=>x-y),i=a.length>>1;return a.length%2?a[i]:(a[i-1]+a[i])/2;};
const stats = values => ({samples:values,median:median(values),min:Math.min(...values),max:Math.max(...values),
  mean:values.reduce((a,b)=>a+b,0)/values.length});
const positive = value => typeof value==='number'&&Number.isFinite(value)&&value>0;
const geometricMean = values => Math.exp(values.reduce((sum,value)=>sum+Math.log(value),0)/values.length);
const collectorIdentity = () => Object.fromEntries(['tune-arm.mjs','official-run.mjs','run.mjs'].map(name=>[name,digest(path.join(HERE,name))]));
export function verifyCollector(expected) { assert.deepEqual(collectorIdentity(),expected,'Collector or benchmark runner changed during measurement'); }

export function parseArgs(argv) {
  const result={variants:[],cases:SCREEN,repeats:1,timeoutMs:300000,output:null,baseline:null,resume:false,dryRun:false};
  const seen=new Set();
  for(let i=0;i<argv.length;i++) {
    const key=argv[i];
    if(['--resume','--dry-run'].includes(key)){assert(!seen.has(key),'Duplicate '+key);seen.add(key);result[key==='--resume'?'resume':'dryRun']=true;continue;}
    assert(['--variant','--cases','--repeats','--timeout','--output','--baseline'].includes(key),'Unknown option '+key);
    const value=argv[++i];assert(value&&!value.startsWith('--'),'Missing value for '+key);
    if(key==='--variant') {
      const separator=value.indexOf('=');assert(separator>0&&separator<value.length-1,'Use --variant label=/path/to/dist');
      const label=value.slice(0,separator);assert(/^[a-zA-Z0-9][a-zA-Z0-9._-]*$/.test(label),'Invalid variant label');
      result.variants.push({label,dist:path.resolve(value.slice(separator+1))});continue;
    }
    assert(!seen.has(key),'Duplicate '+key);seen.add(key);
    if(key==='--cases')result.cases=value==='screen'?SCREEN:value==='official'?CASES:value==='all'?[...CASES,'kernels/full']:value.split(',');
    if(key==='--repeats')result.repeats=Number(value);
    if(key==='--timeout')result.timeoutMs=Number(value);
    if(key==='--output')result.output=path.resolve(value);
    if(key==='--baseline')result.baseline=value;
  }
  assert(result.variants.length>=2,'Supply at least two --variant directories');
  assert(new Set(result.variants.map(v=>v.label)).size===result.variants.length,'Duplicate variant label');
  assert(new Set(result.variants.map(v=>v.dist)).size===result.variants.length,'Variant directories must differ');
  assert(result.output,'--output is required');
  assert(Number.isInteger(result.repeats)&&result.repeats>=1&&result.repeats<=100,'--repeats must be 1..100');
  assert(Number.isInteger(result.timeoutMs)&&result.timeoutMs>=1000&&result.timeoutMs<=3600000,'--timeout must be 1000..3600000 ms');
  assert(result.cases.length&&new Set(result.cases).size===result.cases.length&&result.cases.every(c=>[...CASES,'kernels/full'].includes(c)),'Invalid or duplicate case selection');
  result.baseline ||= result.variants[0].label;
  assert(result.variants.some(v=>v.label===result.baseline),'Baseline must identify a supplied variant');
  return result;
}

export function makePlan(variants,cases,repeats) {
  const tasks=[];
  for(let round=0;round<repeats;round++)for(let offset=0;offset<cases.length;offset++) {
    const caseIndex=(round+offset)%cases.length;
    const shift=(round+caseIndex)%variants.length;
    const order=variants.slice(shift).concat(variants.slice(0,shift));
    for(let position=0;position<order.length;position++) {
      const label=order[position].label;
      tasks.push({id:`r${round}-c${caseIndex}-${label}`,round,caseId:cases[caseIndex],variant:label,position,order:order.map(v=>v.label)});
    }
  }
  return tasks;
}

export function inspectVariants(variants) {
  const entries=variants.map(variant=>{
    const artifacts=Object.fromEntries([...FIXED,'quickjs-arm.bin'].map(name=>[name,digest(path.join(variant.dist,name))]));
    const manifestPath=path.join(variant.dist,'quickjs-manifest.json');
    const manifest=JSON.parse(fs.readFileSync(manifestPath,'utf8'));
    assert.equal(manifest.schema,'dynarmic.js/quickjs-build-v1','Variant build manifest schema');
    for(const name of ['quickjs-arm.bin','quickjs-wasm.js','quickjs-wasm.wasm','official-v8.js','official-v8-adapter.js','quickjs-workloads.js']) {
      const recorded=manifest.artifacts.find(item=>item.path===name);assert(recorded,'Manifest omits '+name);
      assert.deepEqual(artifacts[name],{bytes:recorded.bytes,sha256:recorded.sha256},`Stale artifact ${variant.label}/${name}`);
    }
    const hosts={};
    for(const [host,filename] of [['arm_jit','wasm-build-manifest.json'],['arm_bench_jit','arm-bench-build-manifest.json']]) {
      const build=JSON.parse(fs.readFileSync(path.join(variant.dist,filename),'utf8'));
      assert.equal(build.schema,'dynarmic.js/wasm-coremark-host-build-v1');
      assert.equal(build.build.type,'Release','Use a Release host');assert.equal(build.build.profile,false,'Disable host profiling during tuning');
      assert.equal(build.configuration.regionDiagnostics,false,'Disable region diagnostics during tuning');
      for(const [kind,extension] of [['js','.js'],['wasm','.wasm']]) {
        assert.deepEqual(artifacts[host+extension],{bytes:build.artifacts[kind].bytes,sha256:build.artifacts[kind].sha256},`Stale host ${host}`);
      }
      hosts[host]=build;
    }
    const inventory=JSON.parse(fs.readFileSync(path.join(variant.dist,'official-benchmarks.json'),'utf8'));
    assert.deepEqual(inventory.cases.map(item=>item.id),CASES,'The official inventory must remain complete');
    return {...variant,artifacts,manifest,manifestIdentity:digest(manifestPath),hosts};
  });
  for(const entry of entries.slice(1))for(const name of FIXED)
    assert.deepEqual(entry.artifacts[name],entries[0].artifacts[name],`Fixed input differs: ${name} in ${entry.label}. Only the ARM guest may vary.`);
  return entries;
}

export function validateObservation(task,raw) {
  if(task.caseId==='kernels/full') {
    assert.equal(raw.schema,'dynarmic.js/quickjs-node-v1');assert.equal(raw.verified,true);assert.equal(raw.profile,'full');
    assert.deepEqual(raw.reports.map(r=>r.runtime),['node-js','quickjs-wasm','quickjs-dynarmic']);
    const arm=raw.reports.find(r=>r.runtime==='quickjs-dynarmic');
    const expected={lcg:428440788,sieve:6542,pi:3141587};
    for(const result of raw.reports)assert.deepEqual(result.outputs,expected,'Full kernel correctness');
    const metrics={};
    for(const name of ['lcg','sieve','pi']) {
      assert.equal(arm.samplesMs[name].length,3);assert(arm.samplesMs[name].every(positive),'Invalid ARM kernel samples');
      metrics[name+'Ms']=median(arm.samplesMs[name]);
    }
    assert(positive(arm.runMs));return {metrics,runMs:arm.runMs,loadMs:arm.loadMs,stats:arm.compile};
  }
  assert.equal(raw.schema,'dynarmic.js/official-v8-node-v1');assert.equal(raw.verified,true);assert.equal(raw.reports.length,1);
  const result=raw.reports[0],value=result.official;
  assert.equal(result.runtime,'dynarmic');assert.equal(result.caseId,task.caseId);assert.equal(result.status,'pass');
  assert.equal(value.caseId,task.caseId);assert.equal(value.mode,'upstream');assert.equal(value.version,'7');
  assert.equal(value.warmupMs,1000);assert.equal(value.measureMs,1000);assert.equal(value.minimumRuns,32);
  assert.equal(value.runs.length,1);assert(Number.isSafeInteger(value.runs[0])&&value.runs[0]>=32);
  assert(positive(value.elapsedMs[0])&&value.elapsedMs[0]>=1000);assert(positive(value.samplesMs[0]));
  assert(Math.abs(value.samplesMs[0]-value.elapsedMs[0]/value.runs[0])<Math.max(1,value.samplesMs[0])*1e-9,'Invalid normalized timing');
  assert(positive(result.runMs));return {metrics:{msPerIteration:value.samplesMs[0]},runMs:result.runMs,loadMs:result.loadMs,stats:result.stats};
}

export function summarize(spec,observations) {
  const tasks=spec.plan,byId=new Map(observations.map(item=>[item.task.id,item]));
  const result={schema:'dynarmic.js/arm-tuning-summary-v1',planned:tasks.length,completed:observations.length,
    passed:observations.filter(item=>item.status==='pass').length,failed:observations.filter(item=>item.status!=='pass').length,
    baseline:spec.baseline,complete:tasks.every(task=>byId.has(task.id)),variants:{}};
  for(const variant of spec.variants) {
    const cases={};
    for(const caseId of spec.cases) {
      const entries=observations.filter(item=>item.task.variant===variant.label&&item.task.caseId===caseId&&item.status==='pass');
      const metrics={};
      for(const name of Object.keys(entries[0]?.measurement.metrics||{})) {
        const values=entries.map(item=>item.measurement.metrics[name]);const paired=[];
        for(const entry of entries) {
          const base=observations.find(item=>item.task.variant===spec.baseline&&item.task.round===entry.task.round&&item.task.caseId===caseId&&item.status==='pass');
          if(base)paired.push({round:entry.task.round,baseline:base.measurement.metrics[name],candidate:entry.measurement.metrics[name],speedup:base.measurement.metrics[name]/entry.measurement.metrics[name]});
        }
        metrics[name]={...stats(values),pairs:paired,pairedSpeedup:paired.length?stats(paired.map(pair=>pair.speedup)):null};
      }
      const compile={};
      for(const key of ['blocks','bytes','translateMs','optMs','emitMs','compileMs','instMs']) {
        const values=entries.map(item=>item.measurement.stats?.[key]);
        if(values.length&&values.every(value=>typeof value==='number'&&Number.isFinite(value)&&value>=0))compile[key]=stats(values);
      }
      cases[caseId]={completedSamples:entries.length,metrics,runMs:entries.length?stats(entries.map(item=>item.measurement.runMs)):null,compile};
    }
    const suiteNames=[...new Set(CASES.map(caseId=>caseId.split('/')[0]))];
    const ratio=(caseId,metric,round)=>cases[caseId]?.metrics[metric]?.pairs.find(pair=>pair.round===round)?.speedup;
    const roundSpeedups=Array.from({length:spec.repeats},(_,round)=>{
      const suites={};
      for(const suite of suiteNames) {
        const values=CASES.filter(id=>id.startsWith(suite+'/')).map(id=>ratio(id,'msPerIteration',round));
        if(values.every(positive))suites[suite]=geometricMean(values);
      }
      const official=CASES.map(id=>ratio(id,'msPerIteration',round));
      const selected=spec.cases.flatMap(id=>(id==='kernels/full'?['lcgMs','sieveMs','piMs']:['msPerIteration']).map(metric=>ratio(id,metric,round)));
      return {round,suites,
        eightSuiteWeightedSpeedup:Object.keys(suites).length===8?geometricMean(Object.values(suites)):null,
        tenCaseEqualWeightedSpeedup:official.every(positive)?geometricMean(official):null,
        selectedMetricSpeedup:selected.every(positive)?geometricMean(selected):null};
    });
    const aggregate=key=>{const values=roundSpeedups.map(round=>round[key]);return values.every(positive)?stats(values):null;};
    const eight=aggregate('eightSuiteWeightedSpeedup'),ten=aggregate('tenCaseEqualWeightedSpeedup'),selected=aggregate('selectedMetricSpeedup');
    const suiteSpeedups={};
    for(const suite of suiteNames) {
      const values=roundSpeedups.map(round=>round.suites[suite]);
      if(values.every(positive))suiteSpeedups[suite]=median(values);
    }
    result.variants[variant.label]={guestBytes:variant.artifacts['quickjs-arm.bin'].bytes,guestSha256:variant.artifacts['quickjs-arm.bin'].sha256,
      cases,roundSpeedups,suiteSpeedups,
      eightSuiteWeightedSpeedup:eight?.median??null,eightSuiteWeightedSpeedupSummary:eight,
      tenCaseEqualWeightedSpeedup:ten?.median??null,tenCaseEqualWeightedSpeedupSummary:ten,
      geometricMeanPairedSpeedup:selected?.median??null,selectedMetricSpeedupSummary:selected};
  }
  result.aggregation='Compute case ratios, suite means and overall means within each process round, then summarize the independent round ratios';
  result.verified=result.complete&&result.failed===0;return result;
}

function atomicNew(filename,value) {
  const temporary=filename+`.tmp-${process.pid}-${Date.now()}`;
  const descriptor=fs.openSync(temporary,'wx');
  try{fs.writeFileSync(descriptor,JSON.stringify(value,null,2)+'\n');fs.fsyncSync(descriptor);}finally{fs.closeSync(descriptor);}
  try{fs.linkSync(temporary,filename);}finally{fs.unlinkSync(temporary);}
}
function processIdentity(pid) {
  try {
    process.kill(pid,0);
    let start=null;
    if(process.platform==='linux') {const stat=fs.readFileSync(`/proc/${pid}/stat`,'utf8');start=stat.slice(stat.lastIndexOf(')')+2).split(' ')[19];}
    return {pid,start};
  } catch(error){if(error.code==='ESRCH'||error.code==='ENOENT')return null;throw error;}
}
export function acquireLock(directory) {
  const filename=path.join(directory,'active.json');
  if(fs.existsSync(filename)) {
    const owner=JSON.parse(fs.readFileSync(filename,'utf8')),current=processIdentity(owner.pid);
    assert(!current||(owner.start!==null&&current.start!==owner.start),`Experiment is still active in PID ${owner.pid}; observe that process rather than restarting it`);
    fs.renameSync(filename,path.join(directory,`stale-owner-${Date.now()}-${process.pid}.json`));
  }
  const attempts=path.join(directory,'attempts');
  if(fs.existsSync(attempts))for(const name of fs.readdirSync(attempts)) {
    const attempt=path.join(attempts,name),identityFile=path.join(attempt,'process.json');
    if(!fs.existsSync(identityFile)||fs.existsSync(path.join(attempt,'completion.json')))continue;
    const owner=JSON.parse(fs.readFileSync(identityFile,'utf8'));
    if(!owner.pid)continue;
    const current=processIdentity(owner.pid);
    assert(!current||(owner.start!==null&&owner.start!==undefined&&current.start!==owner.start),
      `Measurement child PID ${owner.pid} is still active in ${name}; wait for or stop that process before resuming`);
  }
  const owner={...processIdentity(process.pid),startedAt:new Date().toISOString()};
  const handle=fs.openSync(filename,'wx');fs.writeFileSync(handle,JSON.stringify(owner)+'\n');fs.closeSync(handle);
  return ()=>{if(fs.existsSync(filename)&&JSON.parse(fs.readFileSync(filename,'utf8')).pid===process.pid)fs.unlinkSync(filename);};
}

// A crash can leave a fully or partially written temporary result before the
// no-overwrite link. Once the exclusive collector lock is held, retain those
// known files separately and leave that task missing for a fresh observation.
export function recoverResultTemps(directory,spec) {
  const expected=new Set(spec.plan.map(task=>task.id+'.json')),recovered=[];
  const results=path.join(directory,'results');
  for(const name of fs.readdirSync(results)) {
    if(expected.has(name))continue;
    const match=/^(.*\.json)\.tmp-(\d+)-(\d+)$/.exec(name);
    assert(match&&expected.has(match[1]),'Unexpected result file '+name);
    assert.equal(processIdentity(Number(match[2])),null,'Temporary result owner is still active: '+match[2]);
    const destination=fs.mkdtempSync(path.join(directory,'recovered-result-'));
    fs.renameSync(path.join(results,name),path.join(destination,name));
    recovered.push(path.relative(directory,path.join(destination,name)));
  }
  return recovered;
}

export function collectResults(directory,spec) {
  const observations=[];
  const expected=new Set(spec.plan.map(task=>task.id+'.json'));
  for(const name of fs.readdirSync(path.join(directory,'results')))assert(expected.has(name),'Unexpected result file '+name);
  for(const task of spec.plan) {
    const filename=path.join(directory,'results',task.id+'.json');if(!fs.existsSync(filename))continue;
    const entry=JSON.parse(fs.readFileSync(filename,'utf8'));assert.deepEqual(entry.task,task,'Persisted task differs from the immutable plan');
    assert(['pass','fail'].includes(entry.status),'Invalid persisted result');
    if(entry.status==='pass')assert.deepEqual(validateObservation(task,entry.raw),entry.measurement,'Persisted metrics differ from raw evidence');
    observations.push(entry);
  }
  return observations;
}
function childRun(command,args,attempt,timeout,signal) {
  return new Promise(resolve=>{
    const stdout=fs.openSync(path.join(attempt,'stdout.log'),'wx'),stderr=fs.openSync(path.join(attempt,'stderr.log'),'wx');
    let child,error=null,timedOut=false,killed=false,killTimer,timer;
    const stop=()=>{killed=true;child?.kill('SIGTERM');killTimer=setTimeout(()=>child?.kill('SIGKILL'),1500);};
    try{child=spawn(command,args,{stdio:['ignore',stdout,stderr]});}catch(failure){fs.closeSync(stdout);fs.closeSync(stderr);resolve({error:failure.message,exitCode:null,signal:null});return;}
    fs.closeSync(stdout);fs.closeSync(stderr);
    atomicNew(path.join(attempt,'process.json'),{...(child.pid?processIdentity(child.pid)||{pid:child.pid,start:null}:{pid:null,start:null}),parentPid:process.pid,command,args,startedAt:new Date().toISOString()});
    child.once('error',failure=>{error=failure.message;});
    child.once('close',(exitCode,exitSignal)=>{clearTimeout(timer);clearTimeout(killTimer);signal.removeEventListener('abort',stop);resolve({exitCode,signal:exitSignal,error,timedOut,interrupted:signal.aborted,killed});});
    signal.addEventListener('abort',stop,{once:true});
    if(signal.aborted)stop();
    timer=setTimeout(()=>{timedOut=true;stop();},timeout);
  });
}

export async function runExperiment(options,{execute=childRun}={}) {
  assert(!process.env.DYN_DUMP_DIR,'Disable DYN_DUMP_DIR during tuning');
  const variants=inspectVariants(options.variants);
  const identity={schema:'dynarmic.js/arm-tuning-v1',variants,cases:options.cases,repeats:options.repeats,timeoutMs:options.timeoutMs,baseline:options.baseline,
    collector:collectorIdentity(),
    environment:{node:process.version,platform:process.platform,arch:process.arch,osRelease:os.release(),hostname:os.hostname(),cpu:os.cpus()[0]?.model,cpuCount:os.cpus().length,totalMemoryBytes:os.totalmem(),
      nodeOptions:process.env.NODE_OPTIONS||'',execArgv:process.execArgv,
      cpuAffinity:process.platform==='linux'?fs.readFileSync('/proc/self/status','utf8').match(/^Cpus_allowed_list:\s*(.*)$/m)?.[1]||null:null},
    methodology:{unit:'One fresh Node process per variant/case/round; kernel repeats and upstream adaptive iterations are not independent process samples',
      order:'For each round rotate case order; rotate variant order by round plus case index; for two variants this alternates AB/BA',
      comparison:'Paired baseline/candidate ratios within each round and case; ratios above 1 favor the candidate',
      suiteWeighting:'Within each process round combine Crypto and EarleyBoyer cases geometrically, then weight eight official suites equally; summarize the independent round ratios; kernel trio reported separately',
      screening:'One round is preliminary. Finalists require repeated alternating process measurements and review of regressions, guest size and compilation statistics.'},
    plan:makePlan(variants,options.cases,options.repeats)};
  if(options.dryRun){console.log(JSON.stringify(identity,null,2));return identity;}
  let spec;
  if(options.resume) {
    spec=JSON.parse(fs.readFileSync(path.join(options.output,'experiment.json'),'utf8'));
    const {createdAt,...prior}=spec;assert.deepEqual(prior,identity,'Resume inputs/configuration changed; use a new output directory');
  } else {
    assert(!fs.existsSync(options.output),'Output already exists; use --resume for missing tasks or select a new directory');
    fs.mkdirSync(path.dirname(options.output),{recursive:true});fs.mkdirSync(options.output);
    spec={...identity,createdAt:new Date().toISOString()};atomicNew(path.join(options.output,'experiment.json'),spec);
    fs.mkdirSync(path.join(options.output,'results'));fs.mkdirSync(path.join(options.output,'attempts'));
  }
  const unlock=acquireLock(options.output),abort=new AbortController();
  const interrupted=()=>abort.abort(new Error('Measurement interrupted; resume only the missing tasks'));
  process.once('SIGINT',interrupted);process.once('SIGTERM',interrupted);
  try {
    recoverResultTemps(options.output,spec);
    const observations=collectResults(options.output,spec),done=new Set(observations.map(item=>item.task.id));
    for(const task of spec.plan) {
      if(done.has(task.id))continue;
      if(abort.signal.aborted)break;
      verifyCollector(spec.collector);
      assert.deepEqual(inspectVariants(options.variants),variants,'Inputs changed during measurement');
      const attempt=fs.mkdtempSync(path.join(options.output,'attempts',task.id+'-'));
      const variant=variants.find(v=>v.label===task.variant),rawPath=path.join(attempt,'raw.json');
      const args=task.caseId==='kernels/full'?[path.join(HERE,'run.mjs'),variant.dist,'full']:
        [path.join(HERE,'official-run.mjs'),variant.dist,'--runtime','dynarmic','--case',task.caseId,'--timeout',String(options.timeoutMs),'--report',rawPath];
      const startedAt=new Date().toISOString(),started=performance.now(),loadAverageBefore=os.loadavg();
      console.error(`RUN ${task.id} ${task.caseId} (${task.position+1}/${variants.length} in this pair/group)`);
      const processResult=await execute(process.execPath,args,attempt,options.timeoutMs+15000,abort.signal);
      const observation={schema:'dynarmic.js/arm-tuning-observation-v1',task,startedAt,finishedAt:new Date().toISOString(),wallMs:performance.now()-started,
        loadAverageBefore,loadAverageAfter:os.loadavg(),attempt:path.relative(options.output,attempt),process:processResult,status:'fail'};
      atomicNew(path.join(attempt,'completion.json'),{task,startedAt,finishedAt:observation.finishedAt,wallMs:observation.wallMs,process:processResult});
      if(processResult.interrupted)break;
      try {
        assert.equal(processResult.exitCode,0,processResult.error||`Child failed (${processResult.exitCode}, ${processResult.signal})`);
        observation.raw=JSON.parse(fs.readFileSync(task.caseId==='kernels/full'?path.join(attempt,'stdout.log'):rawPath,'utf8'));
        observation.measurement=validateObservation(task,observation.raw);
        verifyCollector(spec.collector);
        assert.deepEqual(inspectVariants(options.variants),variants,'Inputs changed while the task was running');
        observation.status='pass';
      }catch(error){observation.error=error.message;}
      atomicNew(path.join(options.output,'results',task.id+'.json'),observation);observations.push(observation);
      const snapshot=path.join(options.output,`summary-${String(observations.length).padStart(5,'0')}.json`);
      atomicNew(snapshot,summarize(spec,observations));
      console.error(`${observation.status.toUpperCase()} ${task.id}${observation.error?': '+observation.error:''}`);
    }
    const final=summarize(spec,collectResults(options.output,spec));
    console.log(JSON.stringify(final,null,2));
    if(abort.signal.aborted)process.exitCode=130;else if(!final.verified)process.exitCode=1;
    return final;
  } finally {process.removeListener('SIGINT',interrupted);process.removeListener('SIGTERM',interrupted);unlock();}
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href) {
  if(process.argv.includes('--help'))console.log('Usage: node examples/quickjs/tune-arm.mjs --variant baseline=/path/dist --variant candidate=/path/dist --cases screen|official|all|ID,ID --repeats 3 --output NEW_DIRECTORY [--baseline label] [--timeout 300000] [--resume] [--dry-run]');
  else try{await runExperiment(parseArgs(process.argv.slice(2)));}catch(error){console.error('Cannot collect ARM tuning experiment: '+error.message);process.exitCode=2;}
}
