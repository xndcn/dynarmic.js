"""Alternating fresh-process, same-host guest ablations; diagnostic, not a gate."""
import hashlib, json, os, platform, random, statistics, subprocess
from pathlib import Path
here=Path(__file__).resolve().parent
specs={'tail-only':['tail-exits','normal-exits'], 'i32':['flags','no-flags'],
       'tee':['flags','constant-memory','normal-exits'], 'constant':['constant-memory','dynamic-memory']}
modes={'liftoff':['--liftoff-only'],'turbofan':['--no-liftoff']}
output=here/'guest-ablation.json'
assert not output.exists()
hashfile=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
identity={str(p.relative_to(here)):hashfile(p) for p in (here/'modules').rglob('*') if p.is_file()}
identity['probe.mjs']=hashfile(here/'probe.mjs')
report={'purpose':'synthetic guest-only independent ablations, not production qualification',
        'rounds':15,'cpu':2,'loadBefore':os.getloadavg(),'platform':platform.platform(),
        'artifacts':identity,'trials':[],'summary':[],'status':'running'}
def save(): output.write_text(json.dumps(report,indent=2)+'\n')
try:
    for mode,flags in modes.items():
        for variant,names in specs.items():
            for name in names:
                ratios=[]
                for round in range(report['rounds']):
                    pair={}
                    for side in (['baseline',variant] if round%2==0 else [variant,'baseline']):
                        cmd=['taskset','-c','2','node',*flags,str(here/'probe.mjs'),str(here/'modules'/side),name]
                        p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
                        row={'mode':mode,'variant':variant,'name':name,'round':round,'side':side,
                             'command':cmd,'status':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
                        report['trials'].append(row)
                        assert p.returncode==0 and not p.stderr,(p.stdout,p.stderr)
                        result=json.loads(p.stdout); row['result']=result
                        assert len(result['samples'])==9 and all(x>0 for x in result['samples'])
                        pair[side]=statistics.median(result['samples'])
                    ratios.append(pair[variant]/pair['baseline'])
                rng=random.Random(0xa18df34)
                boot=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(10000))
                result={'mode':mode,'variant':variant,'name':name,'ratio':statistics.median(ratios),
                        'ci95':[boot[250],boot[9749]],'ratios':ratios}
                report['summary'].append(result);save();print(json.dumps(result),flush=True)
    assert all(hashfile(here/k)==v for k,v in identity.items())
    report['status']='complete'
finally:
    report['loadAfter']=os.getloadavg();save()
