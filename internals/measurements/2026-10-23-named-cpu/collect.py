import json, pathlib, subprocess, sys, hashlib
out=pathlib.Path(sys.argv[1]).resolve()
out.mkdir(exist_ok=False)
build=pathlib.Path(sys.argv[2]).resolve()
workload=pathlib.Path(sys.argv[3]).resolve()
root=pathlib.Path.cwd()
manifest={'commands': [], 'artifacts': {}, 'note':'Diagnostic samples, not performance scores. Full-process collection includes startup and teardown.'}
def hashfile(p): return hashlib.sha256(p.read_bytes()).hexdigest()
specs=[(n,[str(build/'tests'/f'{host}.js'),*args]) for n,host,args in [('a32','jit_bench',['2']),('thumb','jit_thumb_bench',['2']),('a64','jit_a64_bench',['2']),('population','jit_cachepopbench',['0','4096'])]]
for arch in ['a32','a64']:
 specs.append((f'coremark-{arch}',[str(root/'tests/bench/coremark/sample.mjs'),str(build/'examples/browser/dist/arm_jit.js'),str(workload/f'coremark_{arch}.bin'),arch,'2']))
for _,args in specs:
 for a in args:
  p=pathlib.Path(a)
  if p.is_file():
   manifest['artifacts'][str(p)]=hashfile(p)
   if p.suffix=='.js': manifest['artifacts'][str(p.with_suffix('.wasm'))]=hashfile(p.with_suffix('.wasm'))
for round in range(5):
 for index in range(len(specs)):
  name,args=specs[(index+round)%len(specs)]
  name=f'{name}-{round}'
  cmd=['node','--cpu-prof','--cpu-prof-interval=100',f'--cpu-prof-dir={out}',f'--cpu-prof-name={name}.cpuprofile',*args]
  result=subprocess.run(cmd,capture_output=True,text=True,timeout=120)
  (out/f'{name}.stdout').write_text(result.stdout)
  (out/f'{name}.stderr').write_text(result.stderr)
  manifest['commands'].append({'argv':cmd,'status':result.returncode})
  (out/'commands.json').write_text(json.dumps(manifest,indent=2))
  result.check_returncode()
  summary=subprocess.run(['node','tests/bench/cpu_profile.mjs',str(out/f'{name}.cpuprofile')],capture_output=True,text=True)
  (out/f'{name}.summary.json').write_text(summary.stdout)
  summary.check_returncode()
for p,h in manifest['artifacts'].items(): assert hashfile(pathlib.Path(p))==h, p
