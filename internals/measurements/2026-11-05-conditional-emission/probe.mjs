import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {performance} from 'node:perf_hooks';
const [directory, name] = process.argv.slice(2);
const manifest = JSON.parse(fs.readFileSync(path.join(directory, 'manifest.json')));
const memory = new WebAssembly.Memory({initial: 8});
const dv = new DataView(memory.buffer);
const table = new WebAssembly.Table({element: 'anyfunc', initial: 2});
const unexpected = () => { throw Error('unexpected callback'); };
const imports = {e: {m: memory, t: table, read32: unexpected, write32: unexpected}};
const bytes = fs.readFileSync(path.join(directory, `${name}.wasm`));
assert(WebAssembly.validate(bytes), name);
const start = performance.now();
const module = new WebAssembly.Module(bytes);
const compileMs = performance.now() - start;
const f = new WebAssembly.Instance(module, imports).exports.f;
const target = new WebAssembly.Instance(new WebAssembly.Module(fs.readFileSync(path.join(directory, 'tail-target.wasm'))), imports).exports.f;
table.set(1, target);
const state = 0x100, ectx = 0x1000;
const reg = n => state + manifest.regs + 4*n;
dv.setUint32(state, name === 'tail-exits' ? 0x40000000 : 0, true);
dv.setBigInt64(ectx + manifest.cycles, 1000000000n, true);
dv.setUint32(manifest.boundary, name === 'normal-exits' ? 2 : 0, true);
for (let i=0;i<64;++i) dv.setUint32(manifest.slots+4*i, 1, true);
for (const page of [16,17]) dv.setUint32(manifest.pages+4*page, page*4096, true);
dv.setUint32(0x10000, 3, true);
const operand = name.endsWith('memory') ? 0x10000 : 0xf0000001;
dv.setUint32(reg(1), operand, true);
function run(i) {
  dv.setUint32(reg(0), 0, true);
  dv.setUint32(reg(15), 0x1000 + ((i&15)*4), true);
  return f(state,ectx);
}
// Independent scalar reference. Values stay within exact Number arithmetic.
let expected=0, nzcv=0;
const flags = (a,b,r,sub,c) => ((r&0x80000000) | (+!r<<30) | (c<<29) |
  ((sub ? ((a^b)&(a^r)) : (~(a^b)&(a^r)))>>>31)<<28)>>>0;
if (name==='flags') for(let i=0;i<64;++i) {
  let a=expected, sum=a+operand, c=+(sum>0xffffffff);
  expected=sum>>>0;
  a=expected; const b=i+1, diff=a-b-(1-c);
  expected=diff>>>0; nzcv=flags(a,b,expected,true,+(diff>=0));
} else if(name==='no-flags') expected=Math.imul(64,operand)>>>0;
else if(name.endsWith('memory')) expected=64*3;
else if(name==='tail-exits'||name==='normal-exits') expected=1;
for(let i=0;i<16;++i) {
  assert.equal(run(i),0);
  assert.equal(dv.getUint32(reg(0),true),expected,name);
  if(name==='flags') assert.equal(dv.getUint32(state,true),nzcv,'flags');
  if(name.endsWith('memory')) assert.equal(dv.getUint32(0x11000,true),expected,'store');
}
function batch() {
  let value=0;
  for(let i=0;i<200000;++i) value|=run(i);
  return value;
}
for(let s=0;s<5;++s) assert.equal(batch(),0);
const samples=[];
for(let s=0;s<9;++s) {
  const t=performance.now();
  const value=batch();
  const ms=performance.now()-t;
  assert.equal(value,0);
  assert.equal(dv.getUint32(reg(0),true),expected);
  samples.push(ms);
}
console.log(JSON.stringify({name,bytes:bytes.length,compileMs,samples,iterations:200000,
  warmup:1000000, result:expected, node:process.version,v8:process.versions.v8}));
