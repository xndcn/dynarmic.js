import json,re,subprocess
from pathlib import Path
here=Path(__file__).resolve().parent
out=here/'disassembly';out.mkdir(exist_ok=True)
summary=[]
for mode,flag in [('liftoff','--liftoff-only'),('turbofan','--no-liftoff')]:
    for variant,name in [('baseline','tail-exits'),('tail-only','tail-exits'),('baseline','normal-exits'),('tail-only','normal-exits'),('baseline','flags'),('i32','flags'),('tee','flags'),('baseline','constant-memory'),('constant','constant-memory'),('tee','constant-memory')]:
        p=subprocess.run(['taskset','-c','2','node',flag,'--print-wasm-code',str(here/'probe.mjs'),str(here/'modules'/variant),name],capture_output=True,text=True,timeout=60)
        assert p.returncode==0,p.stderr
        file=out/f'{mode}-{variant}-{name}.log';file.write_text(p.stdout+p.stderr)
        sizes=re.findall(r'Instructions \(size = (\d+)\)',p.stdout)
        assert sizes
        summary.append({'mode':mode,'variant':variant,'name':name,'instructionBytes':int(sizes[0]),'log':str(file.relative_to(here))})
(here/'machine-code.json').write_text(json.dumps(summary,indent=2)+'\n')
for row in summary:print(row)
