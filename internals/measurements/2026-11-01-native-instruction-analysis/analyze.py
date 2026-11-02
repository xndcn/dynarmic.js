#!/usr/bin/env python3
from pathlib import Path
import json,statistics,math
ROOT=Path(__file__).resolve().parents[3];CACHE=ROOT/'build-native-instruction-analysis';OUT=Path(__file__).resolve().parent
spec=json.loads((CACHE/'results/experiment.json').read_text());done=json.loads((CACHE/'results/completion.json').read_text());assert done['verified']
records=[json.loads((CACHE/f'results/session-{i}.json').read_text()) for i in range(3)]
stat=lambda v:dict(samples=v,median=statistics.median(v),min=min(v),max=max(v))
def key(t):return t['runtime']+'/'+t['mode']+('/omit' if t['guest']=='bfin-omit.bin' else '')
look=[]
for r in records:
 assert r['status']=='pass' and len(r['observations'])==35;d={}
 for e in r['observations']:
  t=e['task'];v=e['result'];c=t['case'];n=v['iterations'];assert e['exitCode']==0 and v['checksums']==[spec['reference']['expected'][c]]*6 and n==spec['reference']['iterations'][c]
  assert len(v['pmu'])==6 and len(v['samplesMs'])==6
  metrics={'ms':statistics.median(v['samplesMs'][3:])}
  for name in ['cycles','instructions','branches','branchMisses']:
   values=[]
   for p in v['pmu'][3:]:
    assert p['enabled']>=p['running']>0 and p['running']/p['enabled']>.98
    values.append(p[name]*p['enabled']/p['running']/n)
   metrics[name+'PerIteration']=statistics.median(values)
  metrics['ipc']=metrics['instructionsPerIteration']/metrics['cyclesPerIteration']
  d[c,key(t)]=metrics
 look.append(d)
summary={'sessions':3,'observations':105,'checkedCalls':630,'environment':spec['environment'],'cases':{}}
for c in ['integer','branch','stream','pointer']:
 item={'variants':{},'ratios':{}}
 for case,k in look[0]:
  if case!=c:continue
  item['variants'][k]={m:stat([s[c,k][m] for s in look]) for m in look[0][c,k]}
 pairs=[('defaultOverDyn','bcore/default','dynarmic/default'),('llvmOverDyn','bcore/llvm','dynarmic/default'),('o1OverO2','bcore/llvm','bcore/llvm-o2'),('loopGain','bcore/llvm-no-loop','bcore/llvm'),('skipCost','bcore/llvm','bcore/llvm-no-skip'),('pageCost','bcore/llvm','bcore/llvm-raw'),('dynCycleCost','dynarmic/default','dynarmic/no-cycles'),('rotateGain','bcore/llvm','bcore/llvm-rotate'),('omitGain','bcore/llvm','bcore/llvm/omit')]
 for name,num,den in pairs:
  if num not in item['variants'] or den not in item['variants']:continue
  item['ratios'][name]={m:stat([s[c,num][m]/s[c,den][m] for s in look]) for m in ['ms','cyclesPerIteration','instructionsPerIteration']}
 summary['cases'][c]=item
(OUT/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for c,item in summary['cases'].items():
 print(c)
 for k,v in item['variants'].items():print(k,{m:round(s['median'],4) for m,s in v.items()})
 print('cycle ratios',{k:round(v['cyclesPerIteration']['median'],4) for k,v in item['ratios'].items()})
