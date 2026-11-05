import hashlib,json,subprocess
from pathlib import Path
here=Path(__file__).resolve().parent
checks=[]
for variant in sorted((here/'modules').iterdir()):
    for name in ['flags','no-flags','constant-memory','dynamic-memory','normal-exits','tail-exits']:
        for flag in ['--liftoff-only','--no-liftoff']:
            p=subprocess.run(['node',flag,str(here/'probe.mjs'),str(variant),name,'--check'],capture_output=True,text=True,timeout=60)
            checks.append({'variant':variant.name,'name':name,'flag':flag,'status':p.returncode,'stdout':p.stdout,'stderr':p.stderr})
            assert p.returncode==0 and not p.stderr,(variant,name,p.stdout,p.stderr)
(here/'functional-checks.json').write_text(json.dumps({'checks':checks,'count':len(checks),'status':'complete'},indent=2)+'\n')
print('functional processes passed',len(checks))
