// Translate one guest block and write its WASM module, for inspection or for
// running in Node via dispatch.mjs. The IR is printed to stderr.
//
// usage: block_dump [--raw] [--thumb | --a64] <program> <addr_hex> <out.wasm>
//
//   <program>  a built-in program (see kPrograms) loaded at 0x1000, or a flat
//              binary given as <file>[@<base_hex>] (default base 0x1000)
//   --raw      frontend translation only (A32::Translate). By default the full
//              A32AddressSpace::GenerateIR pipeline runs (optimization passes,
//              conditional-branch folding) like the JIT does.
//   --thumb    translate in Thumb state
//   --a64      the code is AArch64 (A64AddressSpace::GenerateIR / A64::Translate)
//
// Example (the `loop` program runs blocks 0x1000, 0x1008, 0x1010 and 0x1014):
//   for a in 1000 1008 1010 1014; do block_dump loop $a b$a.wasm 2>b$a.ir; done
//   node dispatch.mjs 1000=b1000.wasm 1008=b1008.wasm 1010=b1010.wasm 1014=b1014.wasm 1000 2000 \
//        --expect halt=0,r0=0,r1=16,r15=0x2000,n=0,z=1

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dynarmic/backend/wasm/a32_address_space.h"
#include "dynarmic/backend/wasm/a64_address_space.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A32/arch_version.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"

using namespace Dynarmic;

namespace {

constexpr A32::VAddr kDefaultBase = 0x1000;

struct Program {
    const char* name;
    const char* description;
    std::vector<u32> code;
};

const Program kPrograms[] = {
    {"loop", "r0=5; r1=1; do { r1 += r0 } while (--r0); bx lr  -> r0=0 r1=16",
     {
         0xE3A00005,  // mov  r0, #5
         0xE3A01001,  // mov  r1, #1
         0xE0811000,  // add  r1, r1, r0        ; loop: (0x1008)
         0xE2500001,  // subs r0, r0, #1
         0x1AFFFFFC,  // bne  loop
         0xE12FFF1E,  // bx   lr
     }},
    {"bench", "jit_bench memory loop: 2000 x (256 loads + stores), then svc #0",
     {
         0xE3A04E7D,  // mov  r4, #2000         ; outer count
         0xE3A00000,  // mov  r0, #0            ; outer: (0x1004) sum
         0xE3A01901,  // mov  r1, #0x4000       ; buffer pointer
         0xE3A02C01,  // mov  r2, #256          ; inner count
         0xE5913000,  // ldr  r3, [r1]          ; inner: (0x1010)
         0xE2811004,  // add  r1, r1, #4
         0xE0800003,  // add  r0, r0, r3
         0xE5810400,  // str  r0, [r1, #0x400]
         0xE2522001,  // subs r2, r2, #1
         0x1AFFFFF9,  // bne  inner
         0xE2544001,  // subs r4, r4, #1
         0x1AFFFFF4,  // bne  outer
         0xEF000000,  // svc  #0
     }},
    {"call", "jit_callbench: 1M x (bl func; func: add r0, #1; bx lr), then svc #0",
     {
         0xE3A04601,  // mov  r4, #0x100000
         0xE3A00000,  // mov  r0, #0
         0xEB000002,  // bl   func              ; loop: (0x1008)
         0xE2544001,  // subs r4, r4, #1
         0x1AFFFFFC,  // bne  loop
         0xEF000000,  // svc  #0
         0xE2800001,  // add  r0, r0, #1        ; func: (0x1018)
         0xE12FFF1E,  // bx   lr
     }},
    {"qadd", "saturating / parallel arithmetic on r0, r1",
     {
         0xE1012050,  // qadd    r2, r0, r1
         0xE1213050,  // qsub    r3, r0, r1
         0xE1414050,  // qdadd   r4, r0, r1
         0xE6305F11,  // shadd16 r5, r0, r1
         0xE1A06006,  // mov     r6, r6
         0xE12FFF1E,  // bx      lr
     }},
};

class Callbacks final : public A32::UserCallbacks {
public:
    Callbacks(std::vector<u8> image, A32::VAddr base)
            : image{std::move(image)}, base{base} {}

    std::optional<u32> MemoryReadCode(A32::VAddr vaddr) override {
        if (vaddr < base || vaddr - base + 4 > image.size()) {
            return std::nullopt;
        }
        const u8* p = image.data() + (vaddr - base);
        return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
    }

    u8 MemoryRead8(A32::VAddr) override { return 0; }
    u16 MemoryRead16(A32::VAddr) override { return 0; }
    u32 MemoryRead32(A32::VAddr) override { return 0; }
    u64 MemoryRead64(A32::VAddr) override { return 0; }
    void MemoryWrite8(A32::VAddr, u8) override {}
    void MemoryWrite16(A32::VAddr, u16) override {}
    void MemoryWrite32(A32::VAddr, u32) override {}
    void MemoryWrite64(A32::VAddr, u64) override {}
    bool IsReadOnlyMemory(A32::VAddr) override { return false; }
    void InterpreterFallback(A32::VAddr, size_t) override {}
    void CallSVC(u32) override {}
    void ExceptionRaised(A32::VAddr, A32::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }

private:
    std::vector<u8> image;
    A32::VAddr base;
};

// Code fetch for A64 guests from the same image.
class Callbacks64 final : public A64::UserCallbacks {
public:
    explicit Callbacks64(Callbacks& a32)
            : a32{a32} {}

    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        return vaddr >> 32 ? std::nullopt : a32.MemoryReadCode(static_cast<u32>(vaddr));
    }
    u8 MemoryRead8(u64) override { return 0; }
    u16 MemoryRead16(u64) override { return 0; }
    u32 MemoryRead32(u64) override { return 0; }
    u64 MemoryRead64(u64) override { return 0; }
    A64::Vector MemoryRead128(u64) override { return {}; }
    void MemoryWrite8(u64, u8) override {}
    void MemoryWrite16(u64, u16) override {}
    void MemoryWrite32(u64, u32) override {}
    void MemoryWrite64(u64, u64) override {}
    void MemoryWrite128(u64, A64::Vector) override {}
    void InterpreterFallback(u64, size_t) override {}
    void CallSVC(u32) override {}
    void ExceptionRaised(u64, A64::Exception) override {}
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1 << 20; }
    u64 GetCNTPCT() override { return 0; }

private:
    Callbacks& a32;
};

int Usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s [--raw] [--thumb | --a64] <program|file[@base_hex]> <addr_hex> <out.wasm>\nbuilt-in programs:\n", argv0);
    for (const Program& p : kPrograms) {
        std::fprintf(stderr, "  %-6s %s\n", p.name, p.description);
    }
    return 2;
}

// Resolves <program> to a code image and its load address.
bool LoadProgram(const std::string& spec, std::vector<u8>& image, A32::VAddr& base) {
    for (const Program& p : kPrograms) {
        if (spec == p.name) {
            image.resize(p.code.size() * sizeof(u32));
            std::memcpy(image.data(), p.code.data(), image.size());
            base = kDefaultBase;
            return true;
        }
    }

    std::string path = spec;
    base = kDefaultBase;
    if (const auto at = spec.rfind('@'); at != std::string::npos) {
        path = spec.substr(0, at);
        base = static_cast<A32::VAddr>(std::strtoul(spec.c_str() + at + 1, nullptr, 16));
    }
    std::ifstream f{path, std::ios::binary};
    if (!f) {
        std::fprintf(stderr, "%s: not a built-in program and not a readable file\n", spec.c_str());
        return false;
    }
    image.assign(std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{});
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    bool raw = false;
    bool thumb = false;
    bool a64 = false;
    std::vector<const char*> args;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--raw") == 0) {
            raw = true;
        } else if (std::strcmp(argv[i], "--thumb") == 0) {
            thumb = true;
        } else if (std::strcmp(argv[i], "--a64") == 0) {
            a64 = true;
        } else {
            args.push_back(argv[i]);
        }
    }
    if (args.size() != 3) {
        return Usage(argv[0]);
    }

    std::vector<u8> image;
    A32::VAddr base;
    if (!LoadProgram(args[0], image, base)) {
        return 1;
    }
    const u32 addr = static_cast<u32>(std::strtoul(args[1], nullptr, 16));
    const A32::LocationDescriptor location{addr, A32::PSR{thumb ? 0x20u : 0u}, A32::FPSCR{}};

    static Callbacks callbacks{std::move(image), base};
    A32::UserConfig conf;
    conf.callbacks = &callbacks;
    static Callbacks64 callbacks64{callbacks};
    A64::UserConfig conf64;
    conf64.callbacks = &callbacks64;

    if (a64) {
        const A64::LocationDescriptor location64{addr, FP::FPCR{}};
        IR::Block block = raw ? A64::Translate(location64, [](u64 vaddr) { return callbacks64.MemoryReadCode(vaddr); }, {})
                              : Backend::Wasm::A64AddressSpace{conf64}.GenerateIR(location64);
        std::fprintf(stderr, "%s\n", IR::DumpBlock(block).c_str());
        Backend::Wasm::EmitConfig emit_conf{false, true};
        emit_conf.frontend = Backend::Wasm::Frontend::A64;
        emit_conf.a64_conf = &conf64;
        const Backend::Wasm::EmittedBlockInfo ebi = Backend::Wasm::EmitWASM(std::move(block), emit_conf);
        std::ofstream out{args[2], std::ios::binary};
        out.write(reinterpret_cast<const char*>(ebi.module_bytes.data()),
                  static_cast<std::streamsize>(ebi.module_bytes.size()));
        std::printf("wrote %zu bytes to %s\n", ebi.module_bytes.size(), args[2]);
        return 0;
    }

    IR::Block block = [&] {
        if (raw) {
            return A32::Translate(location, &callbacks, A32::TranslationOptions{A32::ArchVersion::v8});
        }
        Backend::Wasm::A32AddressSpace address_space{conf};
        return address_space.GenerateIR(location);
    }();

    std::fprintf(stderr, "%s\n", IR::DumpBlock(block).c_str());

    const Backend::Wasm::EmitConfig emit_conf{false, true};
    const Backend::Wasm::EmittedBlockInfo ebi = Backend::Wasm::EmitWASM(std::move(block), emit_conf);

    std::ofstream out{args[2], std::ios::binary};
    out.write(reinterpret_cast<const char*>(ebi.module_bytes.data()),
              static_cast<std::streamsize>(ebi.module_bytes.size()));
    std::printf("wrote %zu bytes to %s\n", ebi.module_bytes.size(), args[2]);
    return 0;
}
