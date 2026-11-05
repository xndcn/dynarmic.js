"""Check configured emission paths against frozen implementations and oracles."""
import hashlib,json,subprocess,tarfile
from pathlib import Path
here=Path(__file__).resolve().parent
root=here.parents[2]
generator=root/'build-native-region-sharing/tests/tools/emission_probe'
previous=here.parent/'2026-11-05-conditional-emission'
shared=here.parent/'2026-11-05-shared-exit-interactions'
# These fixed-address modules distinguish codegen from host address relocation.
expected={('single',0):(previous,'baseline'),('single',2):(previous,'constant'),
 ('region',0):(shared,'inline-base'),('region',1):(shared,'shared-base'),
 ('region',2):(shared,'inline-constant'),('region',3):(shared,'shared-constant')}
output=here/'options-checks.json';assert not output.exists()
report={'generatorSha256':hashlib.sha256(generator.read_bytes()).hexdigest(),'identity':[],'functional':[],'status':'running'}
try:
 for mode in ['single','region']:
  for mask in range(4):
   dest=here/'modules'/f'{mode}-{mask}';assert not dest.exists()
   subprocess.run([str(generator),str(dest),mode,str(mask)],check=True)
   if (mode,mask) in expected:
    folder,variant=expected[(mode,mask)]
    with tarfile.open(folder/'evidence.tar.gz') as t:
     for p in dest.iterdir():
      old=t.extractfile('modules/'+variant+'/'+p.name).read()
      assert p.read_bytes()==old,(mode,mask,p.name)
      report['identity'].append({'mode':mode,'mask':mask,'file':p.name,'frozenVariant':variant,'sha256':hashlib.sha256(old).hexdigest()})
   for name in ['flags','no-flags','constant-memory','dynamic-memory','normal-exits','tail-exits']:
    for flag in ['--liftoff-only','--no-liftoff']:
     cmd=['node',flag,str(shared/'probe.mjs'),str(dest),name,'--check']
     p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
     report['functional'].append({'mode':mode,'mask':mask,'name':name,'flag':flag,'status':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
     assert p.returncode==0 and not p.stderr,(mode,mask,name,p.stdout,p.stderr)
   print(mode,mask,'passed',flush=True)
 report['status']='complete'
finally:output.write_text(json.dumps(report,indent=2)+'\n')
