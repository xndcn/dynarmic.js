// Use the unmodified upstream harness from the pinned extras, replacing only
// one benchmark body and the clock so protocol failure checks finish quickly.
import assert from 'node:assert/strict';
import {existsSync, readFileSync} from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import vm from 'node:vm';
const dist=process.env.DYNARMIC_JS_BROWSER_DIST||'build-em/examples/browser/dist';
const upstream=path.join(dist,'official-v8.js');
const adapter=readFileSync(new URL('../quickjs/official-v8-adapter.js',import.meta.url),'utf8');
function harness() {
  const context=vm.createContext({console});vm.runInContext(readFileSync(upstream,'utf8'),context);vm.runInContext(adapter,context);
  return context;
}
const unavailable=!existsSync(upstream)&&'Build browser_page before testing the official adapter';
test('official adapter preserves upstream warmup, minimum iterations, setup, teardown and actual elapsed batches',{skip:unavailable},()=>{
  const context=harness();
  const value=vm.runInContext(`(() => {
    let calls=0, setup=0, teardown=0, clock=0;
    const suite=BenchmarkSuite.suites.find(s=>s.name==='Richards'), benchmark=suite.benchmarks[0];
    benchmark.run=()=>{calls++;};benchmark.Setup=()=>{setup++;};benchmark.TearDown=()=>{teardown++;};
    const result=OfficialV8Bench.run('Richards/Richards',{},()=>{clock+=1001;return clock;});
    return {calls,setup,teardown,result};
  })()`,context);
  assert.equal(value.calls,33);assert.equal(value.setup,1);assert.equal(value.teardown,1);
  assert.equal(value.result.runs[0],32);assert.equal(value.result.elapsedMs[0],32032);
  assert.equal(value.result.batches.filter(b=>b.phase==='warmup').length,1);
  assert.equal(value.result.batches.filter(b=>b.phase==='measurement').length,32);
  assert.equal(value.result.completeSuite,true);
  assert.ok(value.result.score>0);
});
test('official adapter rejects short timing overrides and restores context after upstream teardown fails',{skip:unavailable},()=>{
  const context=harness();
  assert.throws(()=>vm.runInContext("OfficialV8Bench.run('Richards/Richards',{warmupMs:250},()=>0)",context),/does not permit timing overrides/);
  const value=vm.runInContext(`(() => {
    let clock=0;const priorConsole=console;
    const suite=BenchmarkSuite.suites.find(s=>s.name==='Richards'), benchmark=suite.benchmarks[0];
    const originalList=suite.benchmarks, originalSingle=suite.RunSingleBenchmark;
    benchmark.run=()=>{};benchmark.TearDown=()=>{throw new Error('teardown correctness failed');};
    let message='';try{OfficialV8Bench.run('Richards/Richards',{},()=>{clock+=1001;return clock;});}catch(error){message=String(error);}
    return {message,consoleRestored:console===priorConsole,listRestored:suite.benchmarks===originalList,harnessRestored:suite.RunSingleBenchmark===originalSingle};
  })()`,context);
  assert.match(value.message,/teardown correctness failed/);
  assert.equal(value.consoleRestored,true);assert.equal(value.listRestored,true);assert.equal(value.harnessRestored,true);
});
test('standalone Crypto cases retain untimed dependencies and partial suites never claim a full score',{skip:unavailable},()=>{
  for(const name of ['Encrypt','Decrypt']) {
    const context=harness();
    const value=vm.runInContext(`(() => {
      let clock=0;const calls=[];const suite=BenchmarkSuite.suites.find(s=>s.name==='Crypto');
      for(const benchmark of suite.benchmarks) benchmark.run=()=>{calls.push(benchmark.name);};
      const result=OfficialV8Bench.run('Crypto/${name}',{},()=>{clock+=1001;return clock;});
      return {calls,result};
    })()`,context);
    assert.equal(value.result.completeSuite,false);assert.equal(value.result.score,null);
    assert.equal(value.calls.filter(n=>n===name).length,33);
    assert.equal(value.calls.filter(n=>n!==name).length,1);
    assert.equal(name==='Decrypt'?value.calls[0]:value.calls.at(-1),name==='Decrypt'?'Encrypt':'Decrypt');
  }
});
