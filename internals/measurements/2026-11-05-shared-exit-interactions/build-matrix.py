"""Rebuild independent source variants, freeze guest modules, restore production."""
import hashlib,json,shutil,subprocess,sys
from pathlib import Path
here=Path(__file__).resolve().parent
root=here.parents[2]
backend=root/'src/dynarmic/backend/wasm'
files=['emit_context.h','emit_wasm_self_loop.cpp','emit_wasm_terminal.cpp','wasm_binary.h','emit_wasm_data_processing.cpp','emit_wasm_memory.cpp']
saved={p:(backend/p).read_bytes() for p in files}
base={p:subprocess.check_output(['git','show','bddb494:src/dynarmic/backend/wasm/'+p],cwd=root) for p in files}
previous=here.parent/'2026-11-05-conditional-emission'
build=root/'build-native-region-sharing'
bundle=len(sys.argv)>1 and sys.argv[1]=='bundle'
identity=json.loads((here/'matrix-identity.json').read_text()) if bundle else {'variants':{},'status':'running'}
identity['status']='running'
def put(p,data):
    if p.read_bytes()!=data:p.write_bytes(data)
def patch(file):subprocess.run(['git','apply',str(file)],cwd=root,check=True)
try:
    for policy in (['shared'] if bundle else ['inline','shared','tail']):
        for opt in (['all'] if bundle else ['base','i32','constant','tee']):
            for name,data in base.items():put(backend/name,data)
            if policy=='shared':patch(here/'shared-exit.patch')
            elif policy=='tail':patch(previous/'tail-only.patch')
            if opt=='all':
                for item in ['i32','constant','tee']:patch(previous/(item+'.patch'))
            elif opt!='base':patch(previous/(opt+'.patch'))
            variant=policy+'-'+opt
            sources={str((backend/p).relative_to(root)):hashlib.sha256((backend/p).read_bytes()).hexdigest() for p in files}
            with (here/('build-'+variant+'.log')).open('w') as log:
                subprocess.run(['cmake','--build',str(build),'--target','emission_probe','-j6'],cwd=root,stdout=log,stderr=subprocess.STDOUT,check=True)
            dest=here/'modules'/variant
            assert not dest.exists()
            subprocess.run([str(build/'tests/tools/emission_probe'),str(dest),'region'],check=True)
            identity['variants'][variant]={'source':sources,'generator':hashlib.sha256((build/'tests/tools/emission_probe').read_bytes()).hexdigest(),
                'modules':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in dest.iterdir()}}
            (here/'matrix-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
            print('built',variant,flush=True)
    identity['status']='complete'
finally:
    for name,data in saved.items():put(backend/name,data)
    (here/'matrix-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
