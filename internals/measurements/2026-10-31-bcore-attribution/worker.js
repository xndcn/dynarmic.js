'use strict';
self.onmessage = async ({data: task}) => {
 try {
  const get=async name=>{const r=await fetch(name);if(!r.ok)throw new Error(name+': '+r.status);return r.arrayBuffer();};
  const ref=JSON.parse(new TextDecoder().decode(await get('c-bench-reference.json')));
  const caseIndex=ref.cases.find(c=>c.id===task.caseId).index, config=ref.profiles.quick;
  const iterations=config.iterations[task.caseId],seed=config.seed,expected=config.expected[task.caseId];
  const inp=new Uint32Array([0x43424e31,caseIndex,iterations,seed]);
  let samples=[],checksums=[],packetCounts=[],activeMs=[],stats={},modules=[];
  if(task.capture){const Original=WebAssembly.Module;WebAssembly.Module=new Proxy(Original,{construct(target,args){
   const m=new target(...args);modules.push({exports:Original.exports(m),bytes:Array.from(new Uint8Array(args[0].buffer||args[0],args[0].byteOffset||0,args[0].byteLength))});return m;}});}
  if(task.runtime==='bcore'){
   importScripts('diag.js');const m=await createDiagBcore({wasmBinary:new Uint8Array(await get('diag.wasm'))});
   if(m._bcore_init(task.mode))throw new Error('init');
   const ram=m._bcore_ram();m.HEAPU8.set(new Uint8Array(await get(task.guest+'.bin')),ram+0x10000);m.HEAPU8.set(new Uint8Array(inp.buffer),ram+0xf000);
   const steps=[],channel=new MessageChannel();let state=0;
   while(!state){const start=performance.now();state=m._bcore_step(8);steps.push([start,performance.now()]);if(state<0)throw new Error('step '+state);
    if(!state)await new Promise(resolve=>{channel.port1.onmessage=resolve;channel.port2.postMessage(0);});}
   channel.port1.close();channel.port2.close();
   const marks=Array.from(new Float64Array(m.HEAPU8.buffer,m._bcore_marks(),m._bcore_mark_count()));if(marks.length!==12)throw new Error('marks');
   const packets=Array.from(new BigUint64Array(m.HEAPU8.buffer,m._bcore_packets(),12),Number);
   for(let i=0;i<6;i++){const a=marks[2*i],b=marks[2*i+1];samples.push(b-a);packetCounts.push(packets[2*i+1]-packets[2*i]);activeMs.push(steps.reduce((sum,[s,e])=>sum+Math.max(0,Math.min(e,b)-Math.max(s,a)),0));}
   const out=new Uint32Array(m.HEAPU8.buffer,ram+0xf000,32);if(out[4]!==2||out[5]!==6)throw new Error('mailbox');checksums=Array.from(out.slice(6,12));
   stats={submitted:m._bcore_remote_stat(0),installed:m._bcore_remote_stat(2),stepCalls:steps.length};
  }else if(task.runtime==='arm'){
   importScripts('arm_jit.js');const m=await createArmJit({wasmBinary:new Uint8Array(await get('arm_jit.wasm'))});const ram=m._arm_ram();
   m.HEAPU8.fill(0,ram,ram+16777216);m.HEAPU8.set(new Uint8Array(await get(task.guest==='soft'?'arm-soft.bin':'c-bench-arm.bin')),ram+0x10000);m.HEAPU8.set(new Uint8Array(inp.buffer),ram+0xf000);
   if(m._arm_init(2,0)!==0)throw new Error('ARM init');if(m._arm_run(0x10000)!==0x01000000||m._arm_reg(0)!==0)throw new Error('ARM exit');
   const marks=Array.from(new Float64Array(m.HEAPU8.buffer,m._arm_marks(),m._arm_mark_count()));
   if(marks.length!==12)throw new Error('ARM marks');samples=Array.from({length:6},(_,i)=>marks[2*i+1]-marks[2*i]);checksums=Array.from(new Uint32Array(m.HEAPU8.buffer,ram+0xf000+24,6));
  }else{
   const e=(await WebAssembly.instantiate(await get('c-bench.wasm'),{})).instance.exports;
   for(let i=0;i<6;i++){if(e.bench_init(caseIndex,iterations,seed))throw new Error('direct input');const start=performance.now();checksums.push(e.bench_run()>>>0);samples.push(performance.now()-start);}
  }
  if(checksums.length!==6||checksums.some(x=>x!==expected))throw new Error('checksum '+JSON.stringify(checksums));
  postMessage({ok:true,task,iterations,expected,checksums,samples,packetCounts,activeMs,stats,modules});
 }catch(e){postMessage({ok:false,task,error:String(e.stack||e)});}
};
