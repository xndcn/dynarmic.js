// Per-case alternating complete-duration CoreMark experiment. Run from repository root.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {pathToFileURL} from 'node:url';
const [beforeDir, afterDir, workloadDir, outputDir, count='5', ...extra]=process.argv.slice(2);
assert(beforeDir && afterDir && workloadDir && outputDir && !extra.length);
const runs=Number(count);assert(Number.isInteger(runs)&&runs>=5&&runs<=100);
assert(!process.env.DYN_DUMP_DIR, 'disable code dumping');
const root=process.cwd(), out=path.resolve(outputDir), work=path.resolve(workloadDir);
fs.mkdirSync(out); // Refuse overwrite, retain partial observations on failure.
const {hashFile,readBuildInfo,sourceInfo,systemInfo}=await import(pathToFileURL(path.join(root,'tests/bench/environment.mjs')));
const {coremarkCases,coremarkMethodology,coremarkIdentity,parseCoremark,summarizeCoremark,validateCoremarkBuild,validateCoremarkReport}=await import(pathToFileURL(path.join(root,'tests/bench/coremark_protocol.mjs')));
const {compareCoremark}=await import(pathToFileURL(path.join(root,'tests/bench/coremark/compare.mjs')));
const workload=JSON.parse(fs.readFileSync(path.join(work,'manifest.json')));validateCoremarkBuild(workload);
const system=systemInfo(), dirs=[beforeDir,afterDir].map(d=>path.resolve(d));
const configs=dirs.map(d=>readBuildInfo(path.resolve(d,'../..')));
const sources=configs.map(c=>sourceInfo(c.sourceRoot));
assert.deepEqual(configs[0].build,configs[1].build,'incompatible build flags');
assert.equal(sources[0].upstream,sources[1].upstream,'upstream changed');
const artifacts=dirs.map(d=>({jsSha256:hashFile(path.join(d,'dist/arm_jit.js')),wasmSha256:hashFile(path.join(d,'dist/arm_jit.wasm'))}));
const verify=()=>{
 assert.deepEqual(systemInfo(),system,'environment changed');
 assert.deepEqual(JSON.parse(fs.readFileSync(path.join(work,'manifest.json'))),workload,'workload manifest changed');
 for(const a of Object.values(workload.artifacts))assert.equal(hashFile(path.join(work,a.file)),a.sha256,'workload artifact changed');
 dirs.forEach((d,i)=>{
  assert.equal(hashFile(path.join(d,'dist/arm_jit.js')),artifacts[i].jsSha256,'JIT JS changed');
  assert.equal(hashFile(path.join(d,'dist/arm_jit.wasm')),artifacts[i].wasmSha256,'JIT Wasm changed');
  assert.deepEqual(readBuildInfo(path.resolve(d,'../..')),configs[i],'build configuration changed');
  assert.deepEqual(sourceInfo(configs[i].sourceRoot),sources[i],'source observation changed');
 });
};
const reports=dirs.map((d,i)=>({schema:'dynarmic.js/coremark-workload-v1',timestamp:new Date().toISOString(),...system,
 git:sources[i],build:configs[i].build,artifact:artifacts[i],workload,workloadSha256:coremarkIdentity(workload),methodology:coremarkMethodology,
 collection:'Paired per case, case order rotated and build order alternated each round; every sample requires ten seconds',
 cases:coremarkCases.map(({name,architecture,memoryMode})=>({name,architecture,memoryMode,runs,samples:[]})),durationQualified:false}));
const commandLog=[];let last=null;
fs.writeFileSync(path.join(out,'process.json'),JSON.stringify({pid:process.pid,argv:process.argv,started:new Date().toISOString()},null,2));
try {
 verify();
 const n=spawnSync(path.join(work,workload.artifacts.native.file),[],{encoding:'utf8',timeout:300000,maxBuffer:4*1024*1024});
 last={case:'native',status:n.status,signal:n.signal,stdout:n.stdout,stderr:n.stderr};
 fs.writeFileSync(path.join(out,'native.json'),JSON.stringify(last,null,2));
 assert.equal(n.status,0,String(n.error||n.stderr||n.signal));
 const native={stdout:n.stdout,metrics:parseCoremark(n.stdout,workload.iterations)};
 reports.forEach(r=>r.native=native);
 for(let round=0;round<runs;round++)for(let j=0;j<coremarkCases.length;j++) {
  const spec=coremarkCases[(j+round)%coremarkCases.length];
  for(const side of round%2?[1,0]:[0,1]) {
   const argv=[path.join(root,'tests/bench/coremark/sample.mjs'),path.join(dirs[side],'dist/arm_jit.js'),path.join(work,workload.artifacts[spec.architecture].file),spec.architecture,String(spec.mode)];
   const child=spawnSync(process.execPath,argv,{encoding:'utf8',timeout:300000,maxBuffer:4*1024*1024});
   last={round,case:spec.name,side:side?'after':'before',argv,status:child.status,signal:child.signal,stdout:child.stdout,stderr:child.stderr};
   commandLog.push(last);
   fs.writeFileSync(path.join(out,'commands.json'),JSON.stringify(commandLog,null,2));
   assert.equal(child.status,0,String(child.error||child.stderr||child.signal));
   const lines=child.stdout.split('\n').filter(l=>l.startsWith('COREMARK_SAMPLE '));assert.equal(lines.length,1);
   const sample=JSON.parse(lines[0].slice(16));sample.process=round;
   sample.metrics=parseCoremark(sample.stdout,workload.iterations);assert.deepEqual(sample.metrics.crc,native.metrics.crc);
   reports[side].cases.find(c=>c.name===spec.name).samples.push(sample);
   assert(sample.metrics.durationValid,'sample did not meet ten seconds');
   fs.writeFileSync(path.join(out,'progress.json'),JSON.stringify({complete:commandLog.length,total:runs*12,last:{round,case:spec.name,side:last.side,kernelSeconds:sample.metrics.seconds}},null,2));
   console.log(`${commandLog.length}/${runs*12} ${last.side} ${spec.name}: ${sample.metrics.seconds.toFixed(3)} s`);
  }
 }
 verify();
 reports.forEach((r,i)=>{
  r.cases.forEach(c=>c.summary=summarizeCoremark(c.samples,r.build.profile));r.durationQualified=true;
  validateCoremarkReport(r,true);fs.writeFileSync(path.join(out,i?'after.json':'before.json'),JSON.stringify(r,null,2),{flag:'wx'});
 });
 const comparison=compareCoremark(...reports,true);
 fs.writeFileSync(path.join(out,'comparison.json'),JSON.stringify(comparison,null,2),{flag:'wx'});
 console.log(`Duration-qualified comparison ${comparison.pass?'PASS':'FAIL'}`);process.exitCode=comparison.pass?0:1;
} catch(error) {
 fs.writeFileSync(path.join(out,'failure.json'),JSON.stringify({error:error.message,last,reports},null,2),{flag:'wx'});
 console.error(error.message);process.exitCode=2;
}
