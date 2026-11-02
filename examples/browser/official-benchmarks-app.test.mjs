// Protocol and lifecycle failures without substituting for real case execution.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
const source = readFileSync(new URL('official-benchmarks-app.js', import.meta.url), 'utf8');
const tick = () => new Promise(resolve => setImmediate(resolve));
const groups = [['Richards',['Richards']],['DeltaBlue',['DeltaBlue']],['Crypto',['Encrypt','Decrypt']],
  ['RayTrace',['RayTrace']],['EarleyBoyer',['Earley','Boyer']],['RegExp',['RegExp']],['Splay',['Splay']],['NavierStokes',['NavierStokes']]];
const ids = groups.flatMap(([suite, names]) => names.map(name => suite + '/' + name));
const inventory = {schema:'dynarmic.js/official-benchmarks-v1',
  suites:groups.map(([id, benchmarks]) => ({id,title:id,benchmarks})), cases:ids.map(id => ({id})),
  methodology:{mode:'upstream',warmupMs:1000,measurementBatchMs:1000,minimumRuns:32}};
const reply = (runtime,caseId) => ({ok:true,runtime,caseId,loadMs:1,runMs:2100,stats:{},
  memoryBytes:runtime==='native'?null:402653184,quickjsMemoryLimitBytes:runtime==='native'?null:192*1048576,
  official:{schema:'dynarmic.js/official-v8-case-v1',caseId,suite:caseId.split('/')[0],name:caseId.split('/')[1],status:'pass',mode:'upstream',version:'7',
    warmupMs:1000,measureMs:1000,minimumRuns:32,samplesMs:[31.25],runs:[32],elapsedMs:[1000],
    summary:{minMs:31.25,maxMs:31.25,medianMs:31.25,meanMs:31.25,stddevMs:0},referenceUs:1000,
    completeSuite:!['Crypto','EarleyBoyer'].includes(caseId.split('/')[0]),
    suiteCases:ids.filter(id=>id.startsWith(caseId.split('/')[0]+'/')),
    score:['Crypto','EarleyBoyer'].includes(caseId.split('/')[0])?null:3.2,
    batches:[{phase:'warmup',runs:null,elapsedMs:1000},{phase:'measurement',runs:32,elapsedMs:1000}],logs:[],
    validation:{upstreamAssertions:true,setupAndTearDown:true}}});
function harness({fetch, onPost}={}) {
  const elements=new Map(),timers=new Map(),listeners=new Map(),workers=[];let timerId=0;
  const element=()=>({textContent:'',hidden:false,disabled:false,value:'',dataset:{},style:{},children:[],
    append(...values){this.children.push(...values);},replaceChildren(...values){this.children=values;},setAttribute(key,value){this[key]=value;}});
  const get=id=>{if(!elements.has(id))elements.set(id,element());return elements.get(id);};
  const document={hidden:false,getElementById:get,createElement:element,
    addEventListener(type,fn){if(!listeners.has(type))listeners.set(type,new Set());listeners.get(type).add(fn);},
    removeEventListener(type,fn){listeners.get(type)?.delete(fn);},
    dispatchEvent(event){for(const fn of listeners.get(event.type)||[])fn(event);}};
  get('stop').disabled=true;get('official-stop').disabled=true;get('official-case').value=ids[0];
  const defaultFetch=async name=>({ok:true,json:async()=>name==='official-benchmarks.json'?structuredClone(inventory):
    name==='quickjs-manifest.json'?{schema:'dynarmic.js/quickjs-build-v1',artifacts:['quickjs-arm.bin','quickjs-wasm.js','quickjs-wasm.wasm','official-v8.js','official-v8-adapter.js'].map(path=>({path,sha256:'a'.repeat(64),bytes:10}))}:
    {schema:'dynarmic.js/wasm-coremark-host-build-v1',source:{revision:'test'},build:{type:'Release'},configuration:{guestRamBytes:256*1048576},
      artifacts:{js:{sha256:'a'.repeat(64),bytes:10},wasm:{sha256:'b'.repeat(64),bytes:20}}}});
  class Worker {
    constructor(){this.terminated=false;workers.push(this);}
    postMessage(message){this.message=message;this.deliver=data=>this.onmessage?.({data});
      if(onPost)return onPost(this,message);queueMicrotask(()=>this.deliver(reply(message.runtime,message.caseId)));}
    terminate(){this.terminated=true;}
  }
  const context=vm.createContext({document,Worker,fetch:fetch||defaultFetch,URL,URLSearchParams,AbortController,AbortSignal,
    location:new URL('http://localhost:8000/nested/'),navigator:{userAgent:'test',hardwareConcurrency:4},
    CustomEvent:class{constructor(type,options){this.type=type;this.detail=options.detail;}},
    setTimeout(fn,ms){timers.set(++timerId,{fn,ms});return timerId;},clearTimeout(id){timers.delete(id);}});
  vm.runInContext('globalThis.window=globalThis',context);vm.runInContext(source,context);
  return {context,get,document,workers,timers,defaultFetch,
    run:()=>get('official-run').onclick(),all:()=>get('official-run-all').onclick(),stop:()=>get('official-stop').onclick(),
    timeout:()=>[...timers.values()].find(t=>t.ms===600000).fn()};
}
function ready(h,state) {
  assert.equal(h.get('official-status').dataset.state,state,h.get('official-status').textContent);
  assert.equal(h.get('official-stop').disabled,true);assert.equal(h.get('official-run').disabled,false);
  assert.equal(h.context.officialBenchRunning,false);assert.equal(h.timers.size,0);
  assert.ok(h.workers.every(w=>w.terminated));
}

test('official inventory must retain every unique case and can recover after a fetch failure',async()=>{
  for(const bad of [{...inventory,cases:inventory.cases.slice(1)},
    {...inventory,cases:[...inventory.cases.slice(0,9),inventory.cases[0]]},
    {...inventory,suites:inventory.suites.slice(1)}]) {
    const h=harness({fetch:async()=>({ok:true,json:async()=>bad})});await tick();
    assert.equal(h.get('official-status').dataset.state,'fail');assert.equal(h.get('official-run').disabled,true);assert.equal(h.workers.length,0);
  }
  let failing=true;let h;
  h=harness({fetch:name=>failing?Promise.resolve({ok:false,status:404}):h.defaultFetch(name)});await tick();
  assert.equal(h.get('official-run').disabled,true);failing=false;await h.get('official-reload').onclick();
  assert.equal(h.get('official-run').disabled,false);
});

test('official Run all retains all 30 measurements and isolates a failed runtime from later cases',async()=>{
  const h=harness({onPost(worker,{runtime,caseId}){queueMicrotask(()=>worker.deliver(runtime==='wasm'&&caseId===ids[0]
    ?{ok:false,error:'runtime fault'}:reply(runtime,caseId)));}});await tick();await h.all();ready(h,'fail');
  const report=h.context.officialBenchReport;
  assert.deepEqual([...report.selectedCases],ids);assert.equal(Object.keys(report.cases).length,10);
  assert.equal(report.cases[ids[0]].wasm.status,'fail');
  assert.equal(Object.values(report.cases).flatMap(Object.values).filter(e=>e.status==='pass').length,29);
  assert.equal(h.get('official-export').disabled,false,'failure details remain downloadable');
});

test('malformed official timings, identities and memory configuration cannot count as passing cases',async()=>{
  for(const corrupt of [r=>({...r,runtime:'other'}),r=>({...r,runMs:NaN}),r=>({...r,stats:{blocks:Infinity}}),
    r=>({...r,quickjsMemoryLimitBytes:1}),r=>({...r,official:{...r.official,mode:'bounded'}}),
    r=>({...r,official:{...r.official,runs:[1]}}),r=>({...r,official:{...r.official,elapsedMs:[10]}}),
    r=>({...r,official:{...r.official,samplesMs:[0]}}),r=>({...r,stats:[]}),
    r=>({...r,official:{...r.official,referenceUs:Infinity}}),r=>({...r,official:{...r.official,score:300}}),
    r=>({...r,official:{...r.official,batches:[{phase:'warmup',runs:null,elapsedMs:1000}]}}),
    r=>({...r,official:{...r.official,batches:[{phase:'warmup',runs:null,elapsedMs:1000},{phase:'measurement',runs:31,elapsedMs:1000}]}})]) {
    const h=harness({onPost(worker,{runtime,caseId}){queueMicrotask(()=>worker.deliver(corrupt(reply(runtime,caseId))));}});
    await tick();await h.run();ready(h,'fail');
    assert.equal(Object.values(h.context.officialBenchReport.cases[ids[0]]).filter(e=>e.status==='pass').length,0);
  }
});

test('official per-runtime deadline stops the Worker and continues the remaining runtimes',async()=>{
  const h=harness({onPost(worker,{runtime,caseId}){if(runtime!=='dynarmic')queueMicrotask(()=>worker.deliver(reply(runtime,caseId)));}});
  await tick();const run=h.run();await tick();h.timeout();await run;ready(h,'fail');
  assert.match(h.context.officialBenchReport.cases[ids[0]].dynarmic.error,/ten-minute/);
  assert.equal(h.context.officialBenchReport.methodology.timeoutPerRuntimeMs,600000);
  assert.equal(h.workers[0].terminated,true);
  assert.equal(h.context.officialBenchReport.cases[ids[0]].wasm.status,'pass');
  assert.equal(h.context.officialBenchReport.cases[ids[0]].native.status,'pass');
});

test('official engine provenance rejects missing hashes or a smaller guest host before execution',async()=>{
  for(const mutate of [value=>({...value,artifacts:{wasm:{sha256:'broken'}}}),value=>({...value,configuration:{guestRamBytes:16*1048576}})]) {
    let h;h=harness({fetch:async name=>{
      if(name==='official-benchmarks.json')return {ok:true,json:async()=>structuredClone(inventory)};
      const response=await h.defaultFetch(name);
      return name==='arm-bench-build-manifest.json'?{ok:true,json:async()=>mutate(await response.json())}:response;
    }});await tick();await h.run();ready(h,'fail');assert.equal(h.workers.length,0);
  }
});

test('official Stop marks every queued case cancelled and ignores a stale Worker after a retry',async()=>{
  let hang=true;
  const h=harness({onPost(worker,{runtime,caseId}){if(!hang)queueMicrotask(()=>worker.deliver(reply(runtime,caseId)));}});
  await tick();const first=h.all();await tick();const stale=h.workers[0];h.stop();await first;ready(h,'cancelled');
  assert.equal(Object.values(h.context.officialBenchReport.cases).flatMap(Object.values).filter(e=>e.status==='cancelled').length,30);
  hang=false;await h.run();ready(h,'pass');
  stale.deliver({type:'progress',runtime:'dynarmic',caseId:ids[0],phase:'executing'});ready(h,'pass');
});

test('official work respects the existing labs and clears the previous export before startup fails',async()=>{
  const h=harness();await tick();h.context.quickjsRunning=true;await h.all();assert.equal(h.workers.length,0);
  h.context.quickjsRunning=false;h.context.cBenchRunning=true;await h.all();assert.equal(h.workers.length,0);
  h.context.cBenchRunning=false;await h.run();ready(h,'pass');
  h.context.fetch=async()=>({ok:false,status:404});await h.run();ready(h,'fail');
  assert.equal(h.context.officialBenchReport,null);assert.equal(h.get('official-export').disabled,true);
});
