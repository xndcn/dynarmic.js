// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
// Generate real backend modules for the independent BigInt vector reference.

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
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path directory{argv[1]};
    std::filesystem::create_directories(directory);
    auto emit = [&](const char* name, unsigned width, auto operation) {
        const A32::LocationDescriptor location{0x1000, {}, {}};
        IR::Block block{location};
        A32::IREmitter ir{block, location, A32::ArchVersion::v8};
        const auto a = ir.GetVector(A32::ExtReg::Q1);
        const auto b = ir.GetVector(A32::ExtReg::Q2);
        ir.SetVector(A32::ExtReg::Q3, operation(ir, width, a, b));
        ir.SetTerm(IR::Term::ReturnToDispatch{});
        const auto result = Backend::Wasm::EmitWASM(std::move(block), {false, true});
        const auto filename = directory / (std::string{name} + std::to_string(width) + ".wasm");
        std::ofstream out{filename, std::ios::binary};
        out.write(reinterpret_cast<const char*>(result.module_bytes.data()),
                  static_cast<std::streamsize>(result.module_bytes.size()));
        if (!out) throw std::runtime_error("cannot write vector test module");
    };
    emit("eq", 128, [](auto& ir, auto w, const auto& a, const auto& b) { return ir.VectorEqual(w, a, b); });
    emit("sext", 64, [](auto& ir, auto w, const auto& a, const auto&) { return ir.VectorSignExtend(w, a); });
    for (unsigned width : {8, 16, 32, 64}) {
#define EMIT(NAME, METHOD) emit(NAME, width, [](auto& ir, auto w, const auto& a, const auto& b) { return ir.METHOD(w, a, b); })
        EMIT("sqadd", VectorSignedSaturatedAdd);
        EMIT("sqsub", VectorSignedSaturatedSub);
        EMIT("uqadd", VectorUnsignedSaturatedAdd);
        EMIT("uqsub", VectorUnsignedSaturatedSub);
        EMIT("shl_u", VectorLogicalVShift);
        EMIT("shl_s", VectorArithmeticVShift);
        EMIT("rshl_u", VectorRoundingShiftLeftUnsigned);
        EMIT("rshl_s", VectorRoundingShiftLeftSigned);
        EMIT("qshl_u", VectorUnsignedSaturatedShiftLeft);
        EMIT("qshl_s", VectorSignedSaturatedShiftLeft);
        EMIT("suqadd", VectorSignedSaturatedAccumulateUnsigned);
        EMIT("usqadd", VectorUnsignedSaturatedAccumulateSigned);
        if (width == 16 || width == 32) {
            EMIT("qdmull", VectorSignedSaturatedDoublingMultiplyLong);
            EMIT("qdmulh", VectorSignedSaturatedDoublingMultiplyHigh);
            EMIT("qrdmulh", VectorSignedSaturatedDoublingMultiplyHighRounding);
        }
#undef EMIT
        emit("sqabs", width, [](auto& ir, auto w, const auto& a, const auto&) {
            return ir.VectorSignedSaturatedAbs(w, a);
        });
        emit("sqneg", width, [](auto& ir, auto w, const auto& a, const auto&) {
            return ir.VectorSignedSaturatedNeg(w, a);
        });
    }
    std::printf("{\"ext\":%zu,\"fpsr\":%zu}\n",
                offsetof(Backend::Wasm::A32JitState, ext_regs),
                offsetof(Backend::Wasm::A32JitState, fpsr));
}
