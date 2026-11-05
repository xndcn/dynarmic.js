// Emit deterministic synthetic modules for independent emission ablations.
// Usage: emission_probe <output-directory> [region]; timing driver lives with evidence.
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/interface/A32/arch_version.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;
namespace {
struct Emitter : A32::IREmitter { using A32::IREmitter::IREmitter; using IR::IREmitter::Inst; };
A32::LocationDescriptor Location(u32 pc) { return {pc, {}, {}}; }
constexpr u32 boundary = 0x3000, pages = 0x4000, slots = 0x5000;
}
int main(int argc, char** argv) {
    if (argc != 2 && !(argc == 3 && std::strcmp(argv[2], "region") == 0)) return 2;
    const bool region_ops = argc == 3;
    const std::filesystem::path out{argv[1]};
    std::filesystem::create_directories(out);
    std::ofstream manifest{out / "manifest.json"};
    manifest << "{\"regs\":" << offsetof(A32JitState, regs)
             << ",\"cycles\":" << offsetof(ExecutionContext, cycles_remaining)
             << ",\"boundary\":" << boundary << ",\"pages\":" << pages
             << ",\"slots\":" << slots << ",\"cases\":[";
    bool first = true;
    for (const std::string name : {"flags", "no-flags", "constant-memory", "dynamic-memory", "tail-exits", "normal-exits", "tail-target"}) {
        EmitConfig config{false, true};
        config.boundary_state = reinterpret_cast<const BoundaryState*>(static_cast<uptr>(boundary));
        std::vector<IR::Block> blocks;
        const bool region = name == "tail-exits" || name == "normal-exits";
        const bool op_region = region_ops && name != "tail-target" && !region;
        const u32 count = region ? 16 : op_region ? 2 : 1;
        if (region || op_region) {
            config.enable_region_multi_entry = true;
            config.enable_cycle_counting = true;
            config.link_slots_base = slots;
        }
        for (u32 i = 0; i < count; ++i) {
            blocks.emplace_back(Location(0x1000 + i * 4));
            auto& block = blocks.back();
            Emitter ir{block, Location(0x1000 + i * 4), A32::ArchVersion::v8};
            if (op_region && i == 1) {
                ir.SetTerm(IR::Term::ReturnToDispatch{});
                continue;
            }
            if (region) {
                ir.SetRegister(A32::Reg::R0, ir.Add(ir.GetRegister(A32::Reg::R0), ir.Imm32(1)));
                block.CycleCount() = 1;
                const IR::Terminal next = i + 1 == count ? IR::Terminal{IR::Term::ReturnToDispatch{}}
                                                        : IR::Terminal{IR::Term::LinkBlockFast{Location(0x1004 + i * 4)}};
                if (name == "tail-exits")
                    ir.SetTerm(IR::Term::If{IR::Cond::EQ, IR::Term::LinkBlockFast{Location(0x8000)}, next});
                else ir.SetTerm(next);
            } else {
                auto result = ir.GetRegister(A32::Reg::R0);
                const auto operand = ir.GetRegister(A32::Reg::R1);
                for (u32 n = 0; n < 64; ++n) {
                    if (name == "flags") {
                        result = IR::U32{ir.AddWithCarry(result, operand, ir.Imm1(false))};
                        const auto flags = ir.Inst<IR::NZCV>(IR::Opcode::GetNZCVFromOp, result);
                        ir.SetCpsrNZCV(flags);
                        const auto c = ir.Inst<IR::U1>(IR::Opcode::GetCarryFromOp, result);
                        result = IR::U32{ir.SubWithCarry(result, ir.Imm32(n + 1), c)};
                        ir.SetCpsrNZCV(ir.Inst<IR::NZCV>(IR::Opcode::GetNZCVFromOp, result));
                    } else if (name == "constant-memory" || name == "dynamic-memory") {
                        const auto address = name == "constant-memory" ? ir.Imm32(0x10000) : operand;
                        result = ir.Add(result, ir.ReadMemory32(address, IR::AccType::NORMAL));
                        ir.WriteMemory32(name == "constant-memory" ? ir.Imm32(0x11000) : IR::U32{ir.Add(operand, ir.Imm32(0x1000))}, result, IR::AccType::NORMAL);
                    } else if (name != "tail-target") result = ir.Add(result, operand);
                }
                ir.SetRegister(A32::Reg::R0, result);
                if (op_region) {
                    block.CycleCount() = 1;
                    ir.SetTerm(IR::Term::LinkBlockFast{Location(0x1004)});
                } else ir.SetTerm(IR::Term::ReturnToDispatch{});
                if (name.ends_with("memory")) config.page_table_base = pages;
            }
        }
        const auto emitted = EmitWASM(std::move(blocks), config, 0);
        std::ofstream file{out / (name + ".wasm"), std::ios::binary};
        file.write(reinterpret_cast<const char*>(emitted.module_bytes.data()), emitted.module_bytes.size());
        if (!file) throw std::runtime_error("write failed");
        manifest << (first ? "" : ",") << "{\"name\":\"" << name << "\",\"bytes\":" << emitted.module_bytes.size() << '}';
        first = false;
    }
    manifest << "]}\n";
}
