import json,re,subprocess
from pathlib import Path
here=Path(__file__).resolve().parent
out=here/'disassembly';assert not out.exists();out.mkdir()
summary=[]
for mode,flag in [('liftoff','--liftoff-only'),('turbofan','--no-liftoff')]:
    specs=[]
    for name in ['flags','constant-memory']:
        for policy in ['inline','shared','tail']:
            opts=['base','i32','tee'] if name=='flags' else ['base','constant','tee']
            if policy=='shared':opts.append('all')
            specs.extend((policy+'-'+opt,name) for opt in opts)
    specs.extend((policy+'-base',name) for policy in ['inline','shared','tail'] for name in ['normal-exits','tail-exits'])
    for variant,name in specs:
        cmd=['node',flag,'--print-wasm-code',str(here/'probe.mjs'),str(here/'modules'/variant),name,'--check']
        p=subprocess.run(cmd,capture_output=True,text=True,timeout=60)
        assert p.returncode==0,p.stderr
        file=out/f'{mode}-{variant}-{name}.log';file.write_text(p.stdout+p.stderr)
        sizes=re.findall(r'Instructions \(size = (\d+)\)',p.stdout)
        assert sizes
        summary.append({'mode':mode,'variant':variant,'name':name,'instructionBytes':int(sizes[0]),'log':str(file.relative_to(here))})
(here/'machine-code.json').write_text(json.dumps(summary,indent=2)+'\n')
