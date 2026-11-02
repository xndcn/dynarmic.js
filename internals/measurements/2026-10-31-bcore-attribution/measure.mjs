import fs from 'node:fs';
import path from 'node:path';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {withChromium} from '../../../tests/browser/chromium.mjs';
const [rootArg='build-bcore-analysis/dist',outputArg='build-bcore-analysis/results',sessionsArg='3',selection='integer']=process.argv.slice(2);
const root=path.resolve(rootArg),output=path.resolve(outputArg),sessions=Number(sessionsArg);
assert(!fs.existsSync(output));fs.mkdirSync(output,{recursive:true});
const hashes=()=>Object.fromEntries(fs.readdirSync(root).sort().map(n=>[n,createHash('sha256').update(fs.readFileSync(path.join(root,n))).digest('hex')]));
const frozen=hashes();fs.writeFileSync(path.join(output,'experiment.json'),JSON.stringify({frozen,sessions,selection,startedAt:new Date().toISOString()},null,2));
const allSpecs=selection==='smoke'?[{runtime:'bcore',mode:0,guest:'original',caseId:'integer'},{runtime:'bcore',mode:1,guest:'original',caseId:'integer'},{runtime:'arm',guest:'soft',caseId:'float'}]
 :selection==='combined'?['branch','stream','pointer'].flatMap(caseId=>[...[1,3].flatMap(mode=>['original','omit'].map(guest=>({runtime:'bcore',mode,guest,caseId}))),{runtime:'arm',guest:'original',caseId}])
 :selection==='capture'?[0,1].flatMap(mode=>['integer','stream','branch'].map(caseId=>({runtime:'bcore',mode,caseId,guest:'original',capture:true})))
 :[...['integer','branch','stream','pointer'].flatMap(caseId=>[
 ...[0,1,2,3,4].map(mode=>({runtime:'bcore',mode,guest:'original',caseId})),
 ...[0,1].map(mode=>({runtime:'bcore',mode,guest:'omit',caseId})),
 {runtime:'arm',guest:'original',caseId},{runtime:'wasm',guest:'original',caseId}]),
 ...[{runtime:'arm',guest:'original'},{runtime:'arm',guest:'soft'},
 {runtime:'bcore',guest:'original',mode:1},{runtime:'bcore',guest:'omit',mode:1},{runtime:'wasm',guest:'original'}].map(x=>({...x,caseId:'float'}))];
const specs=selection==='integer'?allSpecs.filter(t=>t.caseId!=='float'):allSpecs;
fs.copyFileSync(path.join(root,'c-bench-reference.json'),path.join(output,'reference.json'));
for(let session=0;session<sessions;session++){
 assert.deepEqual(hashes(),frozen);
 const report={session,observations:[],status:'running',startedAt:new Date().toISOString()};
 const save=()=>fs.writeFileSync(path.join(output,`session-${session}.json`),JSON.stringify(report,null,2));
 await withChromium({browser:'/home/xiong/.cache/ms-playwright/chromium-1228/chrome-linux64/chrome',root},async({navigate,evaluate,until})=>{
  await navigate();report.userAgent=await evaluate('navigator.userAgent');
  const tasks=specs.slice(session*7%specs.length).concat(specs.slice(0,session*7%specs.length));
  for(const task of tasks){
   console.log(session,task.caseId,task.runtime,task.mode??'',task.guest);
   await evaluate(`(()=>{window.answer=null;const w=new Worker('analysis-worker.js');w.onmessage=({data})=>{window.answer=data;w.terminate();};w.onerror=e=>{window.answer={ok:false,error:e.message};w.terminate();};w.postMessage(${JSON.stringify(task)});})();`);
   const result=await until(()=>evaluate('window.answer'),'attribution observation',180000);
   assert.equal(result.ok,true,result.error);
   if(task.capture){const dir=path.join(output,`${task.caseId}-${task.mode}`);fs.mkdirSync(dir);for(let i=0;i<result.modules.length;i++)fs.writeFileSync(path.join(dir,`${i}.wasm`),Buffer.from(result.modules[i].bytes));result.modules=result.modules.map(m=>({exports:m.exports,bytes:m.bytes.length}));}
   report.observations.push(result);save();
  }
 });
 report.status='pass';report.finishedAt=new Date().toISOString();save();assert.deepEqual(hashes(),frozen);console.log('PASS session',session);
}
