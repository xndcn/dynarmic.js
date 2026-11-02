// Native end-to-end test for the WASM backend emitter:
// build small IR blocks by hand, emit WASM modules, write them out for Node
// to instantiate and execute. Also checks emission-only metadata and verifies
// that the opt-in A64 fallback multi-entry selector leaves other layouts and
// frontends byte-for-byte unchanged.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/frontend/A64/a64_ir_emitter.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/interface/A32/arch_version.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;

namespace {

constexpr u64 kEntryPC = 0x1000;
// Bit 55 makes LocationDescriptor::PC() sign-extend. The selector must use the
// raw masked descriptor PC instead.
constexpr u64 kReturnPC = 0x0080000000001004;
constexpr u64 kOrdinaryPC = 0x1008;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void WriteModule(const std::string& path, const Backend::Wasm::EmittedBlockInfo& ebi) {
    std::ofstream out{path, std::ios::binary};
    out.write(reinterpret_cast<const char*>(ebi.module_bytes.data()),
              static_cast<std::streamsize>(ebi.module_bytes.size()));
    Require(out.good(), "failed to write emitted module");
}

Backend::Wasm::EmitConfig A64Config(const A64::UserConfig& user_config, bool multi_entry) {
    Backend::Wasm::EmitConfig config{false, true};
    config.frontend = Backend::Wasm::Frontend::A64;
    config.a64_conf = &user_config;
    config.enable_a64_fallback_multi_entry = multi_entry;
    return config;
}

A64::LocationDescriptor A64Location(u64 pc, bool single_step = false) {
    return A64::LocationDescriptor{pc, FP::FPCR{}, single_step};
}

IR::Block MakeA64Block(A64::LocationDescriptor location, A64::Reg marker_reg, u64 marker) {
    IR::Block block{location};
    A64::IREmitter ir{block, location};
    ir.SetX(marker_reg, ir.Imm64(marker));
    return block;
}

// 0 -> {1, 2}, 1 -> 2, 2 -> 1 is irreducible. With Z=1, block 0
// chooses 1 and blocks 1/2 return instead of taking their in-region edge.
std::vector<IR::Block> MakeA64FallbackRegion(bool single_step = false, bool return_to_ordinary = false) {
    const auto entry = A64Location(kEntryPC, single_step);
    const auto return_site = A64Location(kReturnPC, single_step);
    const auto ordinary = A64Location(kOrdinaryPC, single_step);

    auto blocks = std::vector<IR::Block>{};
    blocks.push_back(MakeA64Block(entry, A64::Reg::R0, 11));
    blocks.push_back(MakeA64Block(return_site, A64::Reg::R1, 21));
    blocks.push_back(MakeA64Block(ordinary, A64::Reg::R2, 31));

    blocks[0].SetTerminal(IR::Term::If{IR::Cond::EQ, IR::Term::LinkBlock{return_site}, IR::Term::LinkBlock{ordinary}});
    blocks[1].SetTerminal(IR::Term::If{IR::Cond::NE, IR::Term::LinkBlock{ordinary}, IR::Term::ReturnToDispatch{}});
    // This real PushRSB is what makes block 1 an EmitContext::Region return
    // site. Block 2 is an ordinary selected block and must not become an alias.
    const auto rsb_target = return_to_ordinary ? ordinary : return_site;
    blocks[2].AppendNewInst(IR::Opcode::PushRSB, {IR::Value{rsb_target.UniqueHash()}});
    blocks[2].SetTerminal(IR::Term::If{IR::Cond::NE, IR::Term::LinkBlock{return_site}, IR::Term::ReturnToDispatch{}});
    return blocks;
}

std::vector<IR::Block> MakeA64StructuredRegion() {
    const auto entry = A64Location(kEntryPC);
    const auto next = A64Location(kReturnPC);
    auto blocks = std::vector<IR::Block>{};
    blocks.push_back(MakeA64Block(entry, A64::Reg::R0, 11));
    blocks.push_back(MakeA64Block(next, A64::Reg::R1, 21));
    blocks[0].AppendNewInst(IR::Opcode::PushRSB, {IR::Value{next.UniqueHash()}});
    blocks[0].SetTerminal(IR::Term::LinkBlock{next});
    blocks[1].SetTerminal(IR::Term::ReturnToDispatch{});
    return blocks;
}

std::vector<IR::Block> MakeA64SingleBlock() {
    auto blocks = std::vector<IR::Block>{};
    blocks.push_back(MakeA64Block(A64Location(kEntryPC), A64::Reg::R0, 11));
    blocks[0].SetTerminal(IR::Term::ReturnToDispatch{});
    return blocks;
}

IR::Block MakeA32Block(A32::LocationDescriptor location, A32::Reg marker_reg, u32 marker) {
    IR::Block block{location};
    A32::IREmitter ir{block, location, A32::ArchVersion::v8};
    ir.SetRegister(marker_reg, ir.Imm32(marker));
    return block;
}

std::vector<IR::Block> MakeA32BoundaryRegion() {
    const A32::LocationDescriptor entry{0x1000, {}, {}};
    const A32::LocationDescriptor next{0x1004, {}, {}};
    const A32::LocationDescriptor outside{0x1008, {}, {}};
    auto blocks = std::vector<IR::Block>{};
    blocks.push_back(MakeA32Block(entry, A32::Reg::R0, 11));
    blocks.push_back(MakeA32Block(next, A32::Reg::R1, 21));
    blocks[0].CycleCount() = 1;
    blocks[1].CycleCount() = 1;
    blocks[0].SetTerminal(IR::Term::LinkBlock{next});
    blocks[1].SetTerminal(IR::Term::LinkBlock{outside});
    return blocks;
}

constexpr u32 kBoundaryAddress = 0x500;

size_t CountSequence(const std::vector<u8>& bytes, const std::vector<u8>& sequence) {
    size_t count = 0;
    auto it = bytes.begin();
    while ((it = std::search(it, bytes.end(), sequence.begin(), sequence.end())) != bytes.end()) {
        ++count;
        ++it;
    }
    return count;
}

std::vector<u8> BoundaryLoad(u32 offset) {
    std::vector<u8> bytes{Backend::Wasm::Op::I32Const};
    Backend::Wasm::EncodeSLEB(bytes, kBoundaryAddress);
    bytes.push_back(Backend::Wasm::Op::I32Load);
    Backend::Wasm::EncodeULEB(bytes, 2);
    Backend::Wasm::EncodeULEB(bytes, offset);
    return bytes;
}

std::vector<u8> ContextLoad(u8 op, u32 offset, u32 align) {
    std::vector<u8> bytes{Backend::Wasm::Op::LocalGet};
    Backend::Wasm::EncodeULEB(bytes, Backend::Wasm::kParamEctx);
    bytes.push_back(op);
    Backend::Wasm::EncodeULEB(bytes, align);
    Backend::Wasm::EncodeULEB(bytes, offset);
    return bytes;
}

Backend::Wasm::EmitConfig SpecializedBoundaryConfig(const Backend::Wasm::BoundaryHookSnapshot& snapshot) {
    Backend::Wasm::EmitConfig config{true, true};
    config.boundary_state = reinterpret_cast<const Backend::Wasm::BoundaryState*>(kBoundaryAddress);
    config.boundary_hook_snapshot = snapshot;
    return config;
}

Backend::Wasm::EmittedBlockInfo EmitSpecializedBoundary(const Backend::Wasm::BoundaryHookSnapshot& snapshot,
                                                        size_t expected_hook_loads) {
    const auto emitted = Backend::Wasm::EmitWASM(MakeA32BoundaryRegion(), SpecializedBoundaryConfig(snapshot));
    const auto& bytes = emitted.module_bytes;
    Require(CountSequence(bytes, BoundaryLoad(offsetof(Backend::Wasm::BoundaryState, flags))) == 1,
            "specialization must retain the in-region board-flag guard");
    Require(CountSequence(bytes, ContextLoad(Backend::Wasm::Op::I64Load,
                                             offsetof(Backend::Wasm::ExecutionContext, cycles_remaining), 3)) >= 1,
            "specialization must retain the in-region cycle guard");
    Require(CountSequence(bytes, ContextLoad(Backend::Wasm::Op::I32Load,
                                             offsetof(Backend::Wasm::ExecutionContext, halt_reason), 2)) == 1,
            "specialization must retain the in-region halt guard");
    Require(CountSequence(bytes, BoundaryLoad(offsetof(Backend::Wasm::BoundaryState, instruction_hooks))) == expected_hook_loads,
            "unexpected instruction_hooks load count");
    Require(CountSequence(bytes, BoundaryLoad(offsetof(Backend::Wasm::BoundaryState, hook_slots))) == expected_hook_loads,
            "unexpected hook_slots load count");
    return emitted;
}

IR::Block MakeA32ChainBlock(bool target) {
    const A32::LocationDescriptor entry{target ? u32{0x1004} : u32{0x1000}, {}, {}};
    const A32::LocationDescriptor next{0x1004, {}, {}};
    auto block = MakeA32Block(entry, target ? A32::Reg::R1 : A32::Reg::R0, target ? 21 : 11);
    block.CycleCount() = 1;
    block.SetTerminal(target ? IR::Terminal{IR::Term::ReturnToDispatch{}} : IR::Terminal{IR::Term::LinkBlock{next}});
    return block;
}

IR::Block MakeA32BoundaryIndirectBlock(bool rsb) {
    const A32::LocationDescriptor entry{0x1000, {}, {}};
    const A32::LocationDescriptor target{0x1004, {}, {}};
    auto block = MakeA32Block(entry, A32::Reg::R0, 11);
    block.CycleCount() = 1;
    if (rsb) {
        block.AppendNewInst(IR::Opcode::PushRSB, {IR::Value{target.UniqueHash()}});
    }
    A32::IREmitter ir{block, entry, A32::ArchVersion::v8};
    ir.ALUWritePC(ir.Imm32(target.PC()));
    block.SetTerminal(rsb ? IR::Terminal{IR::Term::PopRSBHint{}} : IR::Terminal{IR::Term::FastDispatchHint{}});
    return block;
}

std::vector<IR::Block> MakeA32FallbackRegion() {
    const A32::LocationDescriptor entry{0x1000, {}, {}};
    const A32::LocationDescriptor return_site{0x1004, {}, {}};
    const A32::LocationDescriptor ordinary{0x1008, {}, {}};
    auto blocks = std::vector<IR::Block>{};
    blocks.push_back(MakeA32Block(entry, A32::Reg::R0, 11));
    blocks.push_back(MakeA32Block(return_site, A32::Reg::R1, 21));
    blocks.push_back(MakeA32Block(ordinary, A32::Reg::R2, 31));
    blocks[0].SetTerminal(IR::Term::If{IR::Cond::EQ, IR::Term::LinkBlock{return_site}, IR::Term::LinkBlock{ordinary}});
    blocks[1].SetTerminal(IR::Term::If{IR::Cond::NE, IR::Term::LinkBlock{ordinary}, IR::Term::ReturnToDispatch{}});
    blocks[2].AppendNewInst(IR::Opcode::PushRSB, {IR::Value{return_site.UniqueHash()}});
    blocks[2].SetTerminal(IR::Term::If{IR::Cond::NE, IR::Term::LinkBlock{return_site}, IR::Term::ReturnToDispatch{}});
    return blocks;
}

void RequireUnchanged(const Backend::Wasm::EmittedBlockInfo& without,
                      const Backend::Wasm::EmittedBlockInfo& with,
                      const char* message) {
    Require(without.module_bytes == with.module_bytes, message);
    Require(with.fallback_aliases.empty(), "aliases are fallback-only and A64-multi-entry-only");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.wasm>\n", argv[0]);
        return 1;
    }

    IR::Block block{A32::LocationDescriptor{0x1000, {}, {}}};

    A32::IREmitter ir{block, A32::LocationDescriptor{0x1000, {}, {}}, A32::ArchVersion::v8};
    // r0 = 5; r1 = r0 + 3; r2 = r1 << 4; cpsr_nz = nz of r2
    ir.SetRegister(A32::Reg::R0, ir.Imm32(5));
    const IR::U32 sum = ir.Add(ir.GetRegister(A32::Reg::R0), ir.Imm32(3));
    ir.SetRegister(A32::Reg::R1, sum);
    const auto shifted = ir.LogicalShiftLeft(ir.GetRegister(A32::Reg::R1), ir.Imm8(4), ir.Imm1(0));
    ir.SetRegister(A32::Reg::R2, shifted.result);
    ir.SetCpsrNZ(ir.NZFrom(shifted.result));
    ir.SetTerm(IR::Term::ReturnToDispatch{});

    std::fprintf(stderr, "%s\n", IR::DumpBlock(block).c_str());

    const Backend::Wasm::EmitConfig conf{false, true};
    const Backend::Wasm::EmittedBlockInfo ebi = Backend::Wasm::EmitWASM(std::move(block), conf);
    WriteModule(argv[1], ebi);

    const A64::UserConfig a64_user_config{};
    const auto fallback_without = Backend::Wasm::EmitWASM(MakeA64FallbackRegion(), A64Config(a64_user_config, false));
    Require(fallback_without.fallback_aliases.empty(), "opt-out fallback must not expose aliases");
    const auto fallback = Backend::Wasm::EmitWASM(MakeA64FallbackRegion(), A64Config(a64_user_config, true));
    Require(fallback.fallback_aliases.size() == 1 &&
                fallback.fallback_aliases[0] == A64Location(kReturnPC).UniqueHash(),
            "fallback aliases must contain exactly the actual non-entry return site");
    const auto entry_return = Backend::Wasm::EmitWASM(MakeA64FallbackRegion(false, true), A64Config(a64_user_config, true), 2);
    Require(entry_return.fallback_aliases.empty(), "the original entry must be excluded even when it is a return site");

    const auto structured_without = Backend::Wasm::EmitWASM(MakeA64StructuredRegion(), A64Config(a64_user_config, false));
    const auto structured = Backend::Wasm::EmitWASM(MakeA64StructuredRegion(), A64Config(a64_user_config, true));
    RequireUnchanged(structured_without, structured, "structured A64 bytes changed");

    const auto single_without = Backend::Wasm::EmitWASM(MakeA64SingleBlock(), A64Config(a64_user_config, false));
    const auto single = Backend::Wasm::EmitWASM(MakeA64SingleBlock(), A64Config(a64_user_config, true));
    RequireUnchanged(single_without, single, "single-block A64 bytes changed");

    const auto step_without = Backend::Wasm::EmitWASM(MakeA64FallbackRegion(true), A64Config(a64_user_config, false));
    const auto step = Backend::Wasm::EmitWASM(MakeA64FallbackRegion(true), A64Config(a64_user_config, true));
    RequireUnchanged(step_without, step, "single-step A64 bytes changed");

    Backend::Wasm::EmitConfig a32_config{false, true};
    const auto a32_without = Backend::Wasm::EmitWASM(MakeA32FallbackRegion(), a32_config);
    a32_config.enable_a64_fallback_multi_entry = true;
    const auto a32 = Backend::Wasm::EmitWASM(MakeA32FallbackRegion(), a32_config);
    RequireUnchanged(a32_without, a32, "A32 fallback bytes changed");

    const std::string output_path{argv[1]};
    const size_t slash = output_path.find_last_of('/');
    const std::string directory = slash == std::string::npos ? "." : output_path.substr(0, slash);
    WriteModule(directory + "/a64_fallback.wasm", fallback);

    Backend::Wasm::EmitConfig boundary_config{true, true};
    boundary_config.boundary_state = reinterpret_cast<const Backend::Wasm::BoundaryState*>(kBoundaryAddress);
    const auto boundary = Backend::Wasm::EmitWASM(MakeA32BoundaryRegion(), boundary_config);
    WriteModule(directory + "/a32_boundary.wasm", boundary);

    Backend::Wasm::BoundaryHookSnapshot hook_snapshot{};
    std::array<u8, 65536> hook_slots{};
    hook_snapshot.hook_slots = hook_slots.data();
    const auto specialized = EmitSpecializedBoundary(hook_snapshot, 0);
    WriteModule(directory + "/a32_boundary_specialized.wasm", specialized);

    hook_snapshot.instruction_hooks = 1;
    const auto filtered_empty = EmitSpecializedBoundary(hook_snapshot, 0);
    Require(filtered_empty.module_bytes == specialized.module_bytes,
            "the conservative filter bit alone must not retain hook guards");

    hook_snapshot.instruction_hooks = 0;
    hook_slots[(0x1000 >> 1) & 0xffff] = 1 << 1;
    const auto source_hook = EmitSpecializedBoundary(hook_snapshot, 0);
    Require(source_hook.module_bytes == specialized.module_bytes,
            "a source-PC hook must not affect guards for other PCs");
    hook_slots[(0x1000 >> 1) & 0xffff] = 0;

    hook_slots[(0x1004 >> 1) & 0xffff] = 1 << 1;
    const auto local_code = EmitSpecializedBoundary(hook_snapshot, 1);
    WriteModule(directory + "/a32_boundary_specialized_local.wasm", local_code);
    hook_slots[(0x1004 >> 1) & 0xffff] = 1 << 2;
    EmitSpecializedBoundary(hook_snapshot, 1);
    hook_slots[(0x1008 >> 1) & 0xffff] = 1 << 2;
    EmitSpecializedBoundary(hook_snapshot, 1);
    hook_slots.fill(0);

    hook_snapshot.instruction_hooks = 1 << 1;
    const auto global_code = EmitSpecializedBoundary(hook_snapshot, 1);
    WriteModule(directory + "/a32_boundary_specialized_global_code.wasm", global_code);
    hook_snapshot.instruction_hooks = 1 << 2;
    const auto global_block = EmitSpecializedBoundary(hook_snapshot, 1);
    WriteModule(directory + "/a32_boundary_specialized_global_block.wasm", global_block);
    hook_snapshot.instruction_hooks = 0;

    auto specialized_chain_config = SpecializedBoundaryConfig(hook_snapshot);
    specialized_chain_config.link_slots_base = 0x10700;
    WriteModule(directory + "/a32_chain_specialized_first.wasm",
                Backend::Wasm::EmitWASM(MakeA32ChainBlock(false), specialized_chain_config));
    WriteModule(directory + "/a32_chain_specialized_target.wasm",
                Backend::Wasm::EmitWASM(MakeA32ChainBlock(true), SpecializedBoundaryConfig(hook_snapshot)));

    auto chain_config = boundary_config;
    chain_config.link_slots_base = 0x10700;
    WriteModule(directory + "/a32_chain_first.wasm", Backend::Wasm::EmitWASM(MakeA32ChainBlock(false), chain_config));
    WriteModule(directory + "/a32_chain_target.wasm", Backend::Wasm::EmitWASM(MakeA32ChainBlock(true), boundary_config));

    auto indirect_config = chain_config;
    indirect_config.fast_dispatch_base = 0x10800;
    WriteModule(directory + "/a32_boundary_rsb.wasm",
                Backend::Wasm::EmitWASM(MakeA32BoundaryIndirectBlock(true), indirect_config));
    auto fast_only_config = boundary_config;
    fast_only_config.fast_dispatch_base = 0x10800;
    WriteModule(directory + "/a32_boundary_fast_dispatch.wasm",
                Backend::Wasm::EmitWASM(MakeA32BoundaryIndirectBlock(false), fast_only_config));
    auto specialized_fast_config = SpecializedBoundaryConfig(hook_snapshot);
    specialized_fast_config.fast_dispatch_base = 0x10800;
    WriteModule(directory + "/a32_boundary_fast_dispatch_specialized.wasm",
                Backend::Wasm::EmitWASM(MakeA32BoundaryIndirectBlock(false), specialized_fast_config));

    auto rsb_only_config = boundary_config;
    rsb_only_config.fast_dispatch_base = 0x10800;
    WriteModule(directory + "/a32_boundary_rsb_only.wasm",
                Backend::Wasm::EmitWASM(MakeA32BoundaryIndirectBlock(true), rsb_only_config));

    for (unsigned count = 0; count <= 3; ++count) {
        IR::Block host_block{A32::LocationDescriptor{0x1000, {}, {}}};
        A32::IREmitter host_ir{host_block, A32::LocationDescriptor{0x1000, {}, {}}, A32::ArchVersion::v8};
        const auto argument = host_ir.Imm64(0x123456789abcdef0ULL);
        switch (count) {
        case 0: host_ir.CallHostFunction(reinterpret_cast<void(*)(void)>(1)); break;
        case 1: host_ir.CallHostFunction(reinterpret_cast<void(*)(u64)>(1), argument); break;
        case 2: host_ir.CallHostFunction(reinterpret_cast<void(*)(u64,u64)>(1), argument, host_ir.Imm64(2)); break;
        case 3: host_ir.CallHostFunction(reinterpret_cast<void(*)(u64,u64,u64)>(1), argument, host_ir.Imm64(2), host_ir.Imm64(3)); break;
        }
        host_ir.SetTerm(IR::Term::ReturnToDispatch{});
        WriteModule(directory + "/host_call_" + std::to_string(count) + ".wasm", Backend::Wasm::EmitWASM(std::move(host_block), conf));
    }
    for (const unsigned width : {1u, 2u, 4u, 8u}) {
        for (const bool absolute : {false, true}) {
            IR::Block memory_block{A32::LocationDescriptor{0x1000, {}, {}}};
            A32::IREmitter mem_ir{memory_block, A32::LocationDescriptor{0x1000, {}, {}}, A32::ArchVersion::v8};
            const auto address = mem_ir.GetRegister(A32::Reg::R0);
            const auto destination = mem_ir.GetRegister(A32::Reg::R1);
            const IR::AccType acc = IR::AccType::NORMAL;
            switch (width) {
            case 1: mem_ir.WriteMemory8(destination, mem_ir.ReadMemory8(address, acc), acc); break;
            case 2: mem_ir.WriteMemory16(destination, mem_ir.ReadMemory16(address, acc), acc); break;
            case 4: mem_ir.WriteMemory32(destination, mem_ir.ReadMemory32(address, acc), acc); break;
            case 8: mem_ir.WriteMemory64(destination, mem_ir.ReadMemory64(address, acc), acc); break;
            }
            mem_ir.SetTerm(IR::Term::ReturnToDispatch{});
            auto config = conf;
            config.page_table_base = 0x1000;
            config.absolute_offset_page_table = absolute;
            WriteModule(directory + "/a32_page_" + std::to_string(width) + (absolute ? "_absolute" : "") + ".wasm",
                        Backend::Wasm::EmitWASM(std::move(memory_block), config));
        }
    }
    std::printf("wrote %zu bytes to %s\n", ebi.module_bytes.size(), argv[1]);
    return 0;
}
