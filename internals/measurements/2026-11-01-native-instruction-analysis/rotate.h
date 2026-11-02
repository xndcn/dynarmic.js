#pragma once
// Case-specific diagnostic only, not a general JIT optimization. At guest
// 0x100ec the known original kernel computes y=x^(x>>13), then rotates y by 7.
// LLVM selected SHLD y,x,7. Since y>>25 == x>>25, ROL y,7 is identical.
// The following CMP overwrites the native flags before any use. All guest
// sticky-flag computations are retained. Patch only between core.run calls,
// after call 1, with no background compiler, and require the exact byte shape.
#include "core.h"
#include "bcore_profile.h"
inline void rotate_probe(bcore::Core& core) {
    unsigned changes=0;
    core.forEachCompiledBlock([&](const BcoreBlockInfo& info){
        if(info.guest_pc!=0x100ec)return;
        auto* bytes=const_cast<uint8_t*>(static_cast<const uint8_t*>(info.host_addr));
        const uint8_t old[]={0x41,0x0f,0xa4,0xde,0x07,0x41,0x39,0xc5};
        const uint8_t setup[]={0x41,0xc1,0xee,0x0d,0x41,0x31,0xde};
        const uint8_t replacement[]={0x41,0xc1,0xc6,0x07,0x90};
        for(size_t i=64;i<512;++i)if(!std::memcmp(bytes+i,old,sizeof old)){
            bool prior=false;for(size_t j=i-64;j<i;++j)prior|=!std::memcmp(bytes+j,setup,sizeof setup);
            need(prior,"rotate probe input shape changed");
            std::memcpy(bytes+i,replacement,sizeof replacement);
            __builtin___clear_cache(reinterpret_cast<char*>(bytes+i),reinterpret_cast<char*>(bytes+i+sizeof replacement));
            ++changes;
        }
    });
    need(changes==1,"rotate probe did not replace exactly one SHLD");
}
