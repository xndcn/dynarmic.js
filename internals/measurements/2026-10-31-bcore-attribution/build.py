#!/usr/bin/env python3
"""Build isolated attribution controls using the already verified bcore archives."""
import argparse, hashlib, importlib.util, json, os, shutil, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
HERE=ROOT/'examples/c-bench'
p=argparse.ArgumentParser();p.add_argument('--cache',type=Path,default=ROOT/'build-bcore-analysis');p.add_argument('--bcore-build',type=Path,default=ROOT/'build-c-bench-bcore/host');p.add_argument('--emsdk',type=Path,default=Path.home()/'.cache/bcore-wasm/emsdk-4.0.9');p.add_argument('--bfin-gcc',type=Path,default=Path.home()/'toolchains/bfin-elf/bin/bfin-elf-gcc');a=p.parse_args()
cache=a.cache.resolve();dist=cache/'dist';dist.mkdir(parents=True,exist_ok=True)
base=ROOT/'build-em/examples/browser/dist';build=a.bcore_build.resolve();sdk=a.emsdk.resolve();gcc=a.bfin_gcc.resolve()
spec=importlib.util.spec_from_file_location('b',HERE/'build-bcore.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
source=build.parent/('source-'+b.REVISION)
commands=[]
def run(cmd,cwd=None):
 cmd=list(map(str,cmd));commands.append(dict(argv=cmd,cwd=str(cwd or ROOT)));subprocess.run(cmd,cwd=cwd,check=True,env=dict(os.environ,EM_CONFIG=str(sdk/'.emscripten')))
def rec(f):
 data=f.read_bytes();return {'path':str(f),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
# Keep all production page/build files untouched. Compile the same host with
# initialization-only controls and passive guest-packet accounting.
s=(HERE/'bcore-host.cpp').read_text()
s=s.replace('static uint64_t cycles;','static uint64_t cycles;\nstatic uint64_t packet_marks[12];')
s=s.replace('else marks[mark_count++] = emscripten_get_now();','else { marks[mark_count] = emscripten_get_now(); packet_marks[mark_count++] = cycles + cpu.chain_packets + (cpu.packets ? cpu.packets - 1 : 0); }')
s=s.replace('int bcore_init()','int bcore_init(int mode)')
s=s.replace('memory->enable_page_table();','if (mode != 3) memory->enable_page_table();')
s=s.replace('core->set_copy_patch(true);','core->set_copy_patch(mode == 0 || mode == 4);\n    core->set_loop_blocks(mode != 2);')
s=s.replace('core->set_tiering(100000, 16);','core->set_tiering(mode == 0 ? 100000 : mode == 4 ? UINT32_MAX : 0, 16);\n    if (mode == 0) core->set_remote_tier([](uint32_t, std::vector<uint8_t>) { return true; });')
s=s.replace('core->set_cold_threshold(10)','core->set_cold_threshold(mode == 0 || mode == 4 ? 10 : 0)')
s=s.replace('EMSCRIPTEN_KEEPALIVE uint8_t* bcore_ram()', 'EMSCRIPTEN_KEEPALIVE uint64_t* bcore_packets() { return packet_marks; }\nEMSCRIPTEN_KEEPALIVE uint8_t* bcore_ram()')
(cache/'host.cpp').write_text(s)
em=sdk/'upstream/emscripten/em++'
run([em,'-O3','-DNDEBUG','-std=c++17','-fwasm-exceptions','-fno-rtti','-I',source/'include','-I',source/'tools','-c',cache/'host.cpp','-o',cache/'host.o'])
run([em,cache/'host.o','@'+str(build/'CMakeFiles/c-bench-bcore.dir/linkLibs.rsp'),'-fwasm-exceptions','-O1','-sALLOW_MEMORY_GROWTH=1','-sINITIAL_MEMORY=128MB','-sMAXIMUM_MEMORY=4GB','-sALLOW_TABLE_GROWTH=1','-sSTACK_SIZE=8MB','-sENVIRONMENT=worker','-sMODULARIZE=1','-sEXPORT_NAME=createDiagBcore','-sINVOKE_RUN=0','-sEXIT_RUNTIME=0','-sEXPORTED_RUNTIME_METHODS=HEAPU8','-o',dist/'diag.js'],cwd=build)
for label,extra in [('original',[]),('omit',['-fomit-frame-pointer','-momit-leaf-frame-pointer']),('o3',['-O3','-fomit-frame-pointer','-momit-leaf-frame-pointer'])]:
 elf=cache/(label+'.elf')
 run([gcc,*b.FLAGS,*extra,'-I',HERE,HERE/'start-bfin.S',HERE/'guest.c',HERE/'kernels.c','-Wl,-T,'+str(HERE/'link-bfin.ld'),'-Wl,--gc-sections','-o',elf,'-lgcc'])
 run([gcc.with_name('bfin-elf-objcopy'),'-O','binary',elf,dist/(label+'.bin')])
 run([gcc,*b.FLAGS,*extra,'-S',HERE/'kernels.c','-o',cache/(label+'.s')])
 (cache/(label+'-disassembly.txt')).write_bytes(subprocess.check_output([gcc.with_name('bfin-elf-objdump'),'-d',elf]))
assert (dist/'original.bin').read_bytes()==(base/'c-bench-bfin.bin').read_bytes(), 'Control changed the original guest'
# Same Clang frontend and scalar flags, only ARM float ABI changes to software.
m=json.loads((base/'c-bench-manifest.json').read_text());softflags=[f.replace('-mfloat-abi=softfp','-mfloat-abi=soft') for f in m['targets']['arm']['effectiveFlags']]
libgcc=Path(subprocess.check_output(['arm-none-eabi-gcc','-mcpu=cortex-a8','-marm','-mfloat-abi=soft','-print-libgcc-file-name']).decode().strip())
elf=cache/'arm-soft.elf'
run(['python3','-m','ziglang','cc',*softflags,HERE/'kernels.c',HERE/'guest.c',HERE/'start-arm.S',*m['targets']['arm']['linkFlags'],libgcc,'-o',elf])
run(['python3','-m','ziglang','objcopy','-O','binary',elf,dist/'arm-soft.bin'])
run(['python3','-m','ziglang','cc',*softflags,'-S',HERE/'kernels.c','-o',cache/'arm-soft.s'])
for name in ['arm_jit.js','arm_jit.wasm','c-bench-arm.bin','c-bench.wasm','c-bench-reference.json']:
 shutil.copyfile(base/name,dist/name)
manifest={'schema':'bcore-attribution-v1','modes':{'0':'copy-patch counted; jobs queued, never delivered; no compiler Worker','1':'untiered LLVM O1/codegen2, page table, loop blocks','2':'same LLVM with loop blocks off','3':'same LLVM with raw bounded RAM instead of page table','4':'copy-patch uncounted, UINT32_MAX'},'commands':commands,'baseBuild':json.loads((base/'c-bench-bcore-manifest.json').read_text()),'inputs':[rec(HERE/'kernels.c'),rec(HERE/'guest.c'),rec(HERE/'bcore-host.cpp'),rec(cache/'host.cpp'),rec(libgcc)],'libraries':[rec(f) for f in sorted((build/'upstream').glob('lib*.a'))],'artifacts':[rec(f) for f in sorted(dist.iterdir()) if f.is_file()]}
(cache/'build.json').write_text(json.dumps(manifest,indent=2)+'\n')
