"""Same IR and JS host: exit policy x individual optimization interactions."""
import datetime,hashlib,json,os,platform,random,statistics,subprocess
from pathlib import Path
here=Path(__file__).resolve().parent
specs=[]
for policy in ['inline','shared','tail']:
    for opt,name in [('i32','flags'),('tee','flags'),('tee','constant-memory'),('constant','constant-memory')]:
        specs.append((f'{policy}:{opt}:{name}',policy+'-base',policy+'-'+opt,name))
for policy in ['shared','tail']:
    for name in ['flags','constant-memory','normal-exits','tail-exits']:
        specs.append((f'policy:{policy}:{name}','inline-base',policy+'-base',name))
for name in ['flags','constant-memory']:
    specs.append((f'shared:all:{name}','shared-base','shared-all',name))
    specs.append((f'total:shared-all:{name}','inline-base','shared-all',name))
specs.extend([('shared:tee-to-all:flags','shared-tee','shared-all','flags'),
              ('shared:constant-to-all:constant-memory','shared-constant','shared-all','constant-memory')])
output=here/'matrix-results.json';assert not output.exists()
assert json.loads((here/'matrix-identity.json').read_text())['status']=='complete'
hashfile=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
identity={str(p.relative_to(here)):hashfile(p) for p in (here/'modules').rglob('*') if p.is_file()}
identity['probe.mjs']=hashfile(here/'probe.mjs')
report={'timestamp':datetime.datetime.now(datetime.timezone.utc).isoformat(),
 'purpose':'synthetic same-host exit/optimization interaction; not firmware or default-suite acceptance',
 'rounds':15,'cpu':2,'loadBefore':os.getloadavg(),'platform':platform.platform(),
 'artifacts':identity,'specs':specs,'trials':[],'summary':[],'status':'running'}
def save():output.write_text(json.dumps(report,indent=2)+'\n')
try:
    for mode,flag in [('liftoff','--liftoff-only'),('turbofan','--no-liftoff')]:
        for round in range(report['rounds']):
            for j in range(len(specs)):
                key,before,after,name=specs[(j+round)%len(specs)]
                for side in ([before,after] if round%2==0 else [after,before]):
                    cmd=['taskset','-c','2','node',flag,str(here/'probe.mjs'),str(here/'modules'/side),name]
                    p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
                    row={'mode':mode,'spec':key,'round':round,'side':side,'command':cmd,'status':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
                    report['trials'].append(row)
                    assert p.returncode==0 and not p.stderr,(p.stdout,p.stderr)
                    result=json.loads(p.stdout);assert result['name']==name and len(result['samples'])==9
                    assert all(t>0 for t in result['samples'])
                    row['result']=result;row['warmMs']=statistics.median(result['samples'])
            save();print(mode,'round',round+1,'trials',len(report['trials']),flush=True)
        for key,before,after,name in specs:
            ts=[t for t in report['trials'] if t['mode']==mode and t['spec']==key]
            ratios=[next(t['warmMs'] for t in ts if t['round']==i and t['side']==after)/next(t['warmMs'] for t in ts if t['round']==i and t['side']==before) for i in range(report['rounds'])]
            rng=random.Random(0xa18df34)
            boot=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(10000))
            s={'mode':mode,'spec':key,'before':before,'after':after,'name':name,'ratio':statistics.median(ratios),'ci95':[boot[250],boot[9749]],'ratios':ratios,
               'warmMs':[statistics.median(t['warmMs'] for t in ts if t['side']==side) for side in [before,after]],
               'bytes':[next(t['result']['bytes'] for t in ts if t['side']==side) for side in [before,after]]}
            report['summary'].append(s)
        save();print(mode,'complete',flush=True)
    assert all(hashfile(here/k)==digest for k,digest in identity.items())
    report['status']='complete'
finally:
    report['loadAfter']=os.getloadavg();save()
