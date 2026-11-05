// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
// Emit integer edge-case modules for the independent JavaScript lane oracle.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/interface/A32/arch_version.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;
namespace {
struct TestEmitter : A32::IREmitter {
    using A32::IREmitter::IREmitter;
    using IR::IREmitter::Inst;
};
struct Case {
    const char* name;
    IR::Opcode op;
    bool ge = false;
    bool overflow = false;
};
#define P(name) Case{#name, IR::Opcode::name}
#define G(name) Case{#name, IR::Opcode::name, true}
const Case cases[]{
    P(PackedSaturatedAddU8), P(PackedSaturatedAddS8),
    P(PackedSaturatedSubU8), P(PackedSaturatedSubS8),
    P(PackedSaturatedAddU16), P(PackedSaturatedAddS16),
    P(PackedSaturatedSubU16), P(PackedSaturatedSubS16),
    G(PackedAddU8), G(PackedAddS8), G(PackedSubU8), G(PackedSubS8),
    G(PackedAddU16), G(PackedAddS16), G(PackedSubU16), G(PackedSubS16),
    G(PackedAddSubU16), G(PackedAddSubS16), G(PackedSubAddU16), G(PackedSubAddS16),
    P(PackedHalvingAddU8), P(PackedHalvingAddS8),
    P(PackedHalvingSubU8), P(PackedHalvingSubS8),
    P(PackedHalvingAddU16), P(PackedHalvingAddS16),
    P(PackedHalvingSubU16), P(PackedHalvingSubS16),
    P(PackedHalvingAddSubU16), P(PackedHalvingAddSubS16),
    P(PackedHalvingSubAddU16), P(PackedHalvingSubAddS16),
    P(SignedSaturatedAddWithFlag32), P(SignedSaturatedSubWithFlag32),
    {"SignedSaturatedAddWithFlag32_live", IR::Opcode::SignedSaturatedAddWithFlag32, false, true},
    {"SignedSaturatedSubWithFlag32_live", IR::Opcode::SignedSaturatedSubWithFlag32, false, true},
    P(SignedSaturatedAdd32), P(SignedSaturatedSub32),
};
#undef P
#undef G

size_t Write(const std::filesystem::path& dir, const char* name, IR::Block&& block) {
    block.SetTerminal(IR::Term::ReturnToDispatch{});
    Backend::Wasm::BoundaryState boundary{};
    Backend::Wasm::EmitConfig config{false, true};
    config.boundary_state = &boundary;  // Exercise arithmetic with a configured board boundary.
    const auto emitted = Backend::Wasm::EmitWASM(std::move(block), config);
    std::ofstream out{dir / (std::string{name} + ".wasm"), std::ios::binary};
    out.write(reinterpret_cast<const char*>(emitted.module_bytes.data()),
              static_cast<std::streamsize>(emitted.module_bytes.size()));
    if (!out.good()) throw std::runtime_error("cannot write module");
    return emitted.module_bytes.size();
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path dir{argv[1]};
    std::filesystem::create_directories(dir);
    std::ofstream manifest{dir / "manifest.json"};
    using State = Backend::Wasm::A32JitState;
    manifest << "{\"regs\":" << offsetof(State, regs)
             << ",\"fpsr\":" << offsetof(State, fpsr)
             << ",\"q\":" << offsetof(State, cpsr_q)
             << ",\"ge\":" << offsetof(State, cpsr_ge) << ",\"modules\":[";
    const A32::LocationDescriptor location{0x1000, {}, {}};
    bool first = true;
    for (const auto& test : cases) {
        IR::Block block{location};
        TestEmitter ir{block, location, A32::ArchVersion::v8};
        const auto result = ir.Inst<IR::U32>(test.op, ir.GetRegister(A32::Reg::R0), ir.GetRegister(A32::Reg::R1));
        if (test.ge) ir.SetGEFlags(ir.Inst<IR::U32>(IR::Opcode::GetGEFromOp, result));
        if (test.overflow) ir.OrQFlag(ir.Inst<IR::U1>(IR::Opcode::GetOverflowFromOp, result));
        ir.SetRegister(A32::Reg::R2, result);
        const auto bytes = Write(dir, test.name, std::move(block));
        manifest << (first ? "" : ",") << "{\"name\":\"" << test.name << "\",\"bytes\":" << bytes << '}';
        first = false;
    }
    // Scalar arithmetic is exercised with independently varying carry inputs
    // and flag consumers, including the fast path with only overflow live.
    const Case scalar_cases[]{
        {"Add32", IR::Opcode::Add32}, {"Add64", IR::Opcode::Add64},
        {"Sub32", IR::Opcode::Sub32}, {"Sub64", IR::Opcode::Sub64},
        {"LogicalShiftLeft32", IR::Opcode::LogicalShiftLeft32},
        {"LogicalShiftLeft64", IR::Opcode::LogicalShiftLeft64},
        {"LogicalShiftRight32", IR::Opcode::LogicalShiftRight32},
        {"LogicalShiftRight64", IR::Opcode::LogicalShiftRight64},
        {"ArithmeticShiftRight32", IR::Opcode::ArithmeticShiftRight32},
        {"ArithmeticShiftRight64", IR::Opcode::ArithmeticShiftRight64},
        {"RotateRight32", IR::Opcode::RotateRight32}, {"RotateRight64", IR::Opcode::RotateRight64},
        {"UnsignedDiv32", IR::Opcode::UnsignedDiv32}, {"UnsignedDiv64", IR::Opcode::UnsignedDiv64},
        {"SignedDiv32", IR::Opcode::SignedDiv32}, {"SignedDiv64", IR::Opcode::SignedDiv64},
        {"UnsignedMultiplyHigh64", IR::Opcode::UnsignedMultiplyHigh64},
        {"SignedMultiplyHigh64", IR::Opcode::SignedMultiplyHigh64},
    };
    for (const auto& test : scalar_cases) {
        const std::string op_name{test.name};
        const bool wide = op_name.ends_with("64");
        const bool arithmetic = op_name.starts_with("Add") || op_name.starts_with("Sub");
        const bool shift = op_name.find("Shift") != std::string::npos || op_name.starts_with("Rotate");
        for (const int carry_mode : {-1, 0, 1}) {
            if (carry_mode != -1 && (!arithmetic || wide)) continue;
            for (const char* flags : {"none", "overflow", "nzcv", "all"}) {
                if (!arithmetic && std::string{flags} != "none") continue;
                IR::Block block{location};
                TestEmitter ir{block, location, A32::ArchVersion::v8};
                const auto a_lo = ir.GetRegister(A32::Reg::R0), a_hi = ir.GetRegister(A32::Reg::R1);
                const auto b_lo = ir.GetRegister(A32::Reg::R2), b_hi = ir.GetRegister(A32::Reg::R3);
                const IR::Value a = wide ? IR::Value{ir.Pack2x32To1x64(a_lo, a_hi)} : IR::Value{a_lo};
                const IR::Value b = wide ? IR::Value{ir.Pack2x32To1x64(b_lo, b_hi)} : IR::Value{b_lo};
                const auto carry = carry_mode < 0 ? ir.MostSignificantBit(ir.GetRegister(A32::Reg::R4)) : ir.Imm1(carry_mode != 0);
                IR::Value result;
                if (arithmetic) {
                    result = ir.Inst(test.op, a, b, carry);
                    if (std::string{flags} == "all") {
                        const auto c = ir.Inst<IR::U1>(IR::Opcode::GetCarryFromOp, result);
                        ir.SetRegister(A32::Reg::R8, IR::U32{ir.AddWithCarry(ir.Imm32(0), ir.Imm32(0), c)});
                    }
                    if (std::string{flags} == "overflow" || std::string{flags} == "all") ir.OrQFlag(ir.Inst<IR::U1>(IR::Opcode::GetOverflowFromOp, result));
                    if (std::string{flags} == "nzcv" || std::string{flags} == "all") ir.SetCpsrNZCV(ir.Inst<IR::NZCV>(IR::Opcode::GetNZCVFromOp, result));
                } else if (shift) {
                    const auto amount = ir.LeastSignificantByte(b_lo);
                    result = wide ? ir.Inst(test.op, a, amount) : ir.Inst(test.op, a, amount, carry);
                    if (!wide) ir.OrQFlag(ir.Inst<IR::U1>(IR::Opcode::GetCarryFromOp, result));
                } else {
                    result = ir.Inst(test.op, a, b);
                }
                if (wide) {
                    ir.SetRegister(A32::Reg::R6, ir.LeastSignificantWord(IR::U64{result}));
                    ir.SetRegister(A32::Reg::R7, ir.MostSignificantWord(IR::U64{result}).result);
                } else {
                    ir.SetRegister(A32::Reg::R6, IR::U32{result});
                }
                const auto name = op_name + "_" + flags + "_c" + std::to_string(carry_mode);
                const auto bytes = Write(dir, name.c_str(), std::move(block));
                manifest << ",{\"name\":\"" << name << "\",\"scalar\":true,\"bits\":" << (wide ? 64 : 32)
                         << ",\"carry\":" << carry_mode << ",\"bytes\":" << bytes << '}';
            }
        }
    }
    // One dependency chain per packed saturation opcode; inputs remain dynamic.
    // Used only for matching baseline/candidate generated-code measurements.
    for (size_t i = 0; i < 8; ++i) {
        const auto& test = cases[i];
        IR::Block block{location};
        TestEmitter ir{block, location, A32::ArchVersion::v8};
        auto value = ir.GetRegister(A32::Reg::R0);
        const auto b = ir.GetRegister(A32::Reg::R1);
        for (size_t n = 0; n < 64; ++n) value = ir.Inst<IR::U32>(test.op, value, b);
        ir.SetRegister(A32::Reg::R2, value);
        const auto name = std::string{"bench_"} + test.name;
        Write(dir, name.c_str(), std::move(block));
    }
    manifest << "]}\n";
    return manifest.good() ? 0 : 1;
}
