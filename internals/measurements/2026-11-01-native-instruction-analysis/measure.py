#!/usr/bin/env python3
import os,json,subprocess,time,hashlib,statistics,platform
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];CACHE=ROOT/'build-native-instruction-analysis';OUT=CACHE/'results'
OUT.mkdir(exist_ok=False)
ref=json.loads((CACHE/'c-bench-reference.json').read_text())['profiles']['full']
cases=['integer','branch','stream','pointer']
tasks=[]
for case in cases:
 for mode in ['default','no-cycles']:tasks.append(dict(case=case,runtime='dynarmic',mode=mode,guest='c-bench-arm.bin'))
 for mode in ['default','llvm','llvm-o2','llvm-no-loop','llvm-no-skip','llvm-raw']:tasks.append(dict(case=case,runtime='bcore',mode=mode,guest='c-bench-bfin.bin'))
 if case=='branch':
  for mode in ['llvm','default']:tasks.append(dict(case=case,runtime='bcore',mode=mode,guest='bfin-omit.bin'))
 if case=='integer':tasks.append(dict(case=case,runtime='bcore',mode='llvm-rotate',guest='c-bench-bfin.bin'))
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
files=[CACHE/n for n in ['bcore','dynarmic','c-bench-bfin.bin','c-bench-arm.bin','bfin-omit.bin','c-bench-reference.json','build.json']]
frozen={str(p):digest(p) for p in files}
write=lambda name,value:(OUT/name).write_text(json.dumps(value,indent=2)+'\n')
write('experiment.json',dict(sessions=3,tasks=tasks,profile='full',reference=ref,artifacts=frozen,build=json.loads((CACHE/'build.json').read_text()),
 collector=digest(Path(__file__)),environment=dict(platform=platform.platform(),affinity=sorted(os.sched_getaffinity(0)),cpu=next(l.split(':',1)[1].strip() for l in Path('/proc/cpuinfo').read_text().splitlines() if l.startswith('model name'))),
 methodology='Fresh process per task; first+2warmup+3measured. PMU scope: execution thread user space only, no compiler thread, interval bracketed by guest markers. Fixed four P-core threads; 3 independent collection sessions with rotated ordering. Diagnostic release host; native binary artifacts stay frozen.'))
for session in range(3):
 assert {str(p):digest(p) for p in files}==frozen
 r=dict(session=session,status='running',loadBefore=os.getloadavg(),observations=[])
 order=tasks[session*11:]+tasks[:session*11]
 for task in order:
  c=task['case'];cmd=list(map(str,[CACHE/task['runtime'],CACHE/task['guest'],cases.index(c),ref['iterations'][c],ref['expected'][c],task['mode']]))
  p=subprocess.run(cmd,env=dict(os.environ,NATIVE_PMU='1'),capture_output=True,text=True,timeout=120)
  e=dict(task=task,command=cmd,exitCode=p.returncode,stderr=p.stderr,stdout=p.stdout)
  r['observations'].append(e);write(f'session-{session}.json',r)
  assert p.returncode==0,p.stderr
  d=json.loads(p.stdout);assert d['ok'] and d['checksums']==[ref['expected'][c]]*6
  assert d['iterations']==ref['iterations'][c] and len(d['pmu'])==6
  for h in d['pmu']:
   assert h['running']>0 and h['running']/h['enabled']>.98,(task,h)
  e['result']=d
  print(session,c,task['runtime'],task['mode'],task['guest'],round(statistics.median(d['samplesMs'][3:]),3),flush=True)
 r.update(status='pass',loadAfter=os.getloadavg());write(f'session-{session}.json',r)
 assert {str(p):digest(p) for p in files}==frozen
write('completion.json',dict(verified=True,sessions=3,observations=len(tasks)*3,checkedCalls=len(tasks)*18))
