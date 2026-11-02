#!/usr/bin/env python3
import json,statistics,sys,hashlib
from pathlib import Path
root=Path(sys.argv[1]);out=Path(sys.argv[2]);out.mkdir(exist_ok=False,parents=True)
spec=json.loads((root/'experiment.json').read_text())
refbytes=(root/'reference.json').read_bytes()
assert hashlib.sha256(refbytes).hexdigest()==spec['frozen']['c-bench-reference.json']
reference=json.loads(refbytes)['profiles']['quick']
combined=spec['selection']=='combined'
records=[json.loads(p.read_text()) for p in sorted(root.glob('session-*.json'))]
assert len(records)==3 and all(r['status']=='pass' for r in records)
def median(v):return statistics.median(v)
def key(t):return t['runtime']+'/'+str(t.get('mode','-'))+'/'+t['guest']
def stat(v):return {'samples':v,'median':median(v),'min':min(v),'max':max(v)}
lookups=[]
for r in records:
 lookup={}
 for o in r['observations']:
  assert o['ok'] and len(o['checksums'])==6 and o['checksums']==[reference['expected'][o['task']['caseId']]]*6
  assert o['iterations']==reference['iterations'][o['task']['caseId']]
  assert len(o['samples'])==6 and all(x>=0 for x in o['samples'])
  if o['packetCounts']:assert len(set(o['packetCounts']))==1
  k=(o['task']['caseId'],key(o['task']));assert k not in lookup;lookup[k]=o
 assert len(lookup)==(15 if combined else 36 if spec['selection']=='integer' else 41);lookups.append(lookup)
assert all(set(x)==set(lookups[0]) for x in lookups)
summary={'sessions':3,'observations':45 if combined else 108,'checkedCalls':270 if combined else 648,'scope':'integer, branch and memory only','cases':{}}
for case in (['branch','stream','pointer'] if combined else ['integer','branch','stream','pointer']):
 variants={}
 for c,k in lookups[0]:
  if c!=case:continue
  items=[d[c,k] for d in lookups]
  variants[k]={'warmMs':stat([median(o['samples'][3:]) for o in items]),'packets':items[0]['packetCounts'][0] if items[0]['packetCounts'] else None}
  if items[0]['activeMs']:
   variants[k]['yieldGapMs']=stat([median([s-a for s,a in zip(o['samples'][3:],o['activeMs'][3:])]) for o in items])
 pairs=[('cpOverLlvm','bcore/0/original','bcore/1/original'),('loopBlocksGain','bcore/2/original','bcore/1/original'),
 ('pageOverRaw','bcore/1/original','bcore/3/original'),('countingCost','bcore/0/original','bcore/4/original'),
 ('omitGainCp','bcore/0/original','bcore/0/omit'),('omitGainLlvm','bcore/1/original','bcore/1/omit'),
 ('llvmOverArm','bcore/1/original','arm/-/original'),
 ('combinedGain','bcore/1/original','bcore/3/omit'),('omitGainRaw','bcore/3/original','bcore/3/omit'),
 ('pageCostOmit','bcore/1/omit','bcore/3/omit'),('remainingVsArm','bcore/3/omit','arm/-/original')]
 ratios={}
 for name,num,den in pairs:
  if num not in variants or den not in variants:continue
  ratios[name]=stat([median(d[case,num]['samples'][3:])/median(d[case,den]['samples'][3:]) for d in lookups])
 summary['cases'][case]={'variants':variants,'ratios':ratios}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for case,v in summary['cases'].items():
 print(case,{k:round(x['warmMs']['median'],3) for k,x in v['variants'].items()})
 print({k:round(x['median'],3) for k,x in v['ratios'].items()})
