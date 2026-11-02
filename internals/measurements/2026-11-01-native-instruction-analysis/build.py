#!/usr/bin/env python3
"""Generate isolated diagnostic hosts; production native sources/binaries stay fixed."""
from pathlib import Path
import json,subprocess,shlex,shutil,hashlib
ROOT=Path(__file__).resolve().parents[3];HERE=Path(__file__).resolve().parent;CACHE=ROOT/'build-native-instruction-analysis';BASE=ROOT/'examples/c-bench/native';BUILD=ROOT/'build-c-bench-native/host';BCORE=ROOT/'build-c-bench-native/bcore-df7aee68589424364e74d6cee4def5269721e3e6'
CACHE.mkdir(exist_ok=True)
shutil.copyfile(HERE/'pmu.h',CACHE/'pmu.h')
shutil.copyfile(HERE/'rotate.h',CACHE/'rotate.h')
s=(BASE/'common.h').read_text().replace('struct Timings {','struct Timings {\n    Pmu pmu;')
s=s.replace('#pragma once','#pragma once\n#include "pmu.h"')
s=s.replace('marks[count++] = now_ms();','if(phase==0)pmu.start();\n        marks[count++] = now_ms();\n        if(phase==1)pmu.stop(count/2-1);')
s=s.replace('std::printf("]}\\n");','pmu.print();')
(CACHE/'common.h').write_text(s)
s=(BASE/'dynarmic.cpp').read_text().replace('need(request.mode == "default", "ARM mode");','need(request.mode == "default" || request.mode == "no-cycles", "ARM mode");')
s=s.replace('config.callbacks=&callbacks; config.page_table=&pages;','config.callbacks=&callbacks; config.page_table=&pages; config.enable_cycle_counting=request.mode!="no-cycles";')
s=s.replace('timings.report(request,ram.data(),prepare);','timings.report(request,ram.data(),prepare);\n        if (const char* dir=std::getenv("NATIVE_CAPTURE")) {\n            std::fflush(stdout);\n            std::freopen((std::string(dir)+"/dynarmic.asm").c_str(),"w",stdout);\n            engine.DumpDisassembly();\n        }')
(CACHE/'dynarmic.cpp').write_text(s)
s=(BASE/'bcore.cpp').read_text().replace('BenchMemory() : FlatMemory(0, RAM_SIZE, &cpu) { enable_page_table(); }','BenchMemory(bool raw) : FlatMemory(0, RAM_SIZE, &cpu) { if(!raw)enable_page_table(); }')
s=s.replace('llvm=request.mode=="llvm"','llvm=request.mode.rfind("llvm",0)==0')
s=s.replace('BenchMemory memory;','BenchMemory memory(request.mode=="llvm-raw");')
s=s.replace('core.set_verify(false); core.set_steady_skip(2);','core.set_verify(false); core.set_steady_skip(request.mode=="llvm-no-skip"?0:2);\n        core.set_loop_blocks(request.mode!="llvm-no-loop");')
s=s.replace('uint64_t cycles=0;', 'uint64_t cycles=0; bool patched=false;')
s=s.replace('cycles+=cpu.chain_packets+cpu.packets;', 'if(request.mode=="llvm-rotate" && !patched && timings.count>=2){need(request.case_index==0 && timings.count<=6,"rotate probe phase");rotate_probe(core);patched=true;std::fprintf(stderr,"rotate patch installed at marker %u\\n",timings.count); }\n            cycles+=cpu.chain_packets+cpu.packets;')
s=s.replace('need(cpu.exit_code==0,', 'if(request.mode=="llvm-rotate")need(patched,"rotation was not patched");\n        need(cpu.exit_code==0,')
s=s.replace('core.init(1,2)','core.init(request.mode=="llvm-o2"?2:1,2)')
# Optional profiling/code observer only in a separately built executable.
s=s.replace('#include "flat_memory.h"','#include "flat_memory.h"\n#include "rotate.h"\n#ifdef DIAG_CAPTURE\n#include "capture.h"\n#endif')
s=s.replace('static Timings timings;','static Timings timings;\n#ifdef DIAG_CAPTURE\nstatic Capture capture;\n#endif')
s=s.replace('if(addr == RAM_SIZE) timings.mark(value);','if(addr == RAM_SIZE) {\n#ifdef DIAG_CAPTURE\n            capture.marker=timings.count;\n#endif\n            timings.mark(value);\n        }')
s=s.replace('Core core(&cpu,&memory);','Core core(&cpu,&memory);\n#ifdef DIAG_CAPTURE\n        core.setEventSink(&capture);\n#endif')
s=s.replace('timings.report(request,memory.raw(),prepare);','timings.report(request,memory.raw(),prepare);\n#ifdef DIAG_CAPTURE\n        capture.save(core);\n#endif')
(CACHE/'bcore.cpp').write_text(s)
commands=[]
for kind in ['dynarmic','bcore']:
 includes=['-I',CACHE]
 if kind=='dynarmic':includes+=['-I',ROOT/'externals/dynarmic/src','-I',ROOT/'externals/dynarmic/externals/mcl/include','-I',ROOT/'externals/dynarmic/externals/fmt/include']
 else:includes+=['-I',BCORE/'include','-I',BCORE/'tools']
 command=list(map(str,['c++','-O3','-DNDEBUG','-std=c++20',*includes,'-c',CACHE/(kind+'.cpp'),'-o',CACHE/(kind+'.o')]))
 subprocess.run(command,check=True);commands.append(command)
 link=shlex.split((BUILD/f'CMakeFiles/c-bench-{kind}.dir/link.txt').read_text())
 link=[str(CACHE/(kind+'.o')) if x.endswith(f'{kind}.cpp.o') else str(CACHE/kind) if x==f'c-bench-{kind}' else x for x in link]
 subprocess.run(link,cwd=BUILD,check=True);commands.append(link)
for name in ['c-bench-arm.bin','c-bench-bfin.bin','c-bench-reference.json']:shutil.copyfile(ROOT/'build-c-bench-native'/name,CACHE/name)
shutil.copyfile(ROOT/'build-bcore-analysis/dist/omit.bin',CACHE/'bfin-omit.bin')
def rec(p):return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
(CACHE/'build.json').write_text(json.dumps({'commands':commands,'sources':[rec(CACHE/n) for n in ['common.h','pmu.h','rotate.h','bcore.cpp','dynarmic.cpp']],'baseManifest':json.loads((ROOT/'build-c-bench-native/manifest.json').read_text()),'artifacts':[rec(CACHE/n) for n in ['bcore','dynarmic','bfin-omit.bin','c-bench-bfin.bin','c-bench-arm.bin','c-bench-reference.json']]},indent=2)+'\n')
