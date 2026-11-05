"""Targeted diagnosis of the failed default cases; never replaces that gate."""
import hashlib,json,os,random,statistics,subprocess
from pathlib import Path
here=Path(__file__).resolve().parent
hosts=[Path('/tmp/dynarmic-conditional-'+side+'-20261105/tests') for side in ['before','after']]
specs=[('memory-thunk','jit_bench.js',['0']),('a64-memory-thunk','jit_a64_bench.js',['0']),
       ('thumb-memory-page-table','jit_thumb_bench.js',['1']),('thumb-memory-flat-window','jit_thumb_bench.js',['2']),
       ('board-memory-1','a32_region_alias_test.js',['1','1','loop','memory'])]
output=here/'host-tier-diagnostic.json';assert not output.exists()
identity=lambda:[{str(p.name):hashlib.sha256(p.read_bytes()).hexdigest() for p in (d/name for name in original['artifacts'][0])} for d in hosts]
original=json.loads((here/'host-comparison.json').read_text())
assert identity()==original['artifacts']
report={'purpose':'diagnosis of four default gate failures and uncertain board memory, not requalification',
        'rounds':10,'cpu':2,'artifacts':identity(),'loadBefore':os.getloadavg(),'trials':[],'summary':[],'status':'running'}
try:
    for mode,flag in [('liftoff','--liftoff-only'),('turbofan','--no-liftoff')]:
        for name,file,args in specs:
            ratios=[]
            for n in range(report['rounds']):
                pair={}
                for side in ([0,1] if n%2==0 else [1,0]):
                    cmd=['taskset','-c','2','node',flag,str(hosts[side]/file),*args]
                    p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
                    row={'mode':mode,'name':name,'round':n,'side':side,'command':cmd,'status':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
                    report['trials'].append(row)
                    assert p.returncode==0 and not any(s in p.stdout for s in ['WRONG','FAIL']),(p.stdout,p.stderr)
                    prefix='REGION_SAMPLE ' if file.startswith('a32_region') else 'BENCHMARK '
                    rows=[json.loads(l[len(prefix):]) for l in p.stdout.splitlines() if l.startswith(prefix)]
                    assert len(rows)==9
                    if prefix.startswith('REGION'):
                        assert all(r['blocks']==r['bytes']==0 for r in rows[1:])
                    else:
                        assert '\nPASS\n' in p.stdout
                        assert all(r['compile']['blocks']==r['compile']['bytes']==0 for r in rows[1:])
                    row['samples']=rows;pair[side]=statistics.median(r['ms'] for r in rows[1:])
                ratios.append(pair[1]/pair[0])
            rng=random.Random(0xa18df34)
            boot=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(10000))
            s={'mode':mode,'name':name,'ratio':statistics.median(ratios),'ci95':[boot[250],boot[9749]],'ratios':ratios}
            report['summary'].append(s);output.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(s),flush=True)
    assert identity()==report['artifacts'];report['status']='complete'
finally:
    report['loadAfter']=os.getloadavg();output.write_text(json.dumps(report,indent=2)+'\n')
