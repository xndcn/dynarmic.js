#pragma once
#include "core.h"
#include "bcore_profile.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
struct Capture final : BcoreEventSink {
    unsigned marker=0, serial=0;
    std::map<uint32_t,BcoreCompileMetrics> last;
    std::string dir=std::getenv("NATIVE_CAPTURE")?std::getenv("NATIVE_CAPTURE"):"capture";
    std::ofstream events;
    Capture(){std::filesystem::create_directories(dir);events.open(dir+"/events.jsonl");}
    bool wantsCompileMetrics() const override{return true;}
    bool wantsIrDump(uint32_t) const override{return std::getenv("NATIVE_IR") != nullptr;}
    void onIrDump(uint32_t pc,const char* stage,const std::string& ir) override {
        std::ofstream f(dir+"/ir-"+std::to_string(pc)+"-"+stage+"-"+std::to_string(serial++)+".ll");f<<ir;
    }
    void onCompileMetrics(uint32_t pc,const BcoreCompileMetrics& m) override {
        last[pc]=m;events<<"{\"pc\":"<<pc<<",\"marker\":"<<marker<<",\"tier\":"<<m.tier<<",\"background\":"<<m.background<<",\"cp\":"<<m.copy_patch<<",\"bytes\":"<<m.object_bytes<<",\"irInstructions\":"<<m.ir_instructions<<"}\n";
    }
    void save(bcore::Core& core){
        std::ofstream map(dir+"/blocks.jsonl");
        core.forEachCompiledBlock([&](const BcoreBlockInfo& b){
            if(!b.host_size)return;
            std::string name="pc-"+std::to_string(b.guest_pc)+"-"+std::to_string(b.variant)+"-"+std::to_string(reinterpret_cast<uintptr_t>(b.host_addr))+".bin";
            std::ofstream f(dir+"/"+name,std::ios::binary);f.write(static_cast<const char*>(b.host_addr),b.host_size);
            map<<"{\"file\":\""<<name<<"\",\"pc\":"<<b.guest_pc<<",\"address\":"<<reinterpret_cast<uintptr_t>(b.host_addr)<<",\"size\":"<<b.host_size<<",\"variant\":"<<b.variant<<"}\n";
        });
        const auto s=core.stats();std::ofstream f(dir+"/stats.json");
        f<<"{\"blocksPromoted\":"<<s.blocks_promoted<<",\"blocksTranslated\":"<<s.blocks_translated<<",\"cpBlocks\":"<<s.copy_patch_blocks<<",\"cpFallbacks\":"<<s.copy_patch_fallbacks<<",\"loopBlocks\":"<<s.loop_blocks<<",\"loopsProven\":"<<s.loops_proven<<",\"loopsSteady\":"<<s.loops_steady<<"}\n";
    }
};
