// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

// Emit real IR modules for the independent bit-pattern reference in
// fp_edge_test.mjs. General-purpose registers carry scalar FP bit patterns.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A64/a64_ir_emitter.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
        return 1;
    }
    const std::filesystem::path dir{argv[1]};
    for (const u32 width : {32u, 64u}) {
        for (u32 mode = 0; mode < 4; ++mode) {
            for (const bool is_max : {false, true}) {
                for (const bool vector : {false, true}) {
                    for (const bool controlled : {false, true}) {
                        if (!vector && !controlled) continue;
                        const u32 fpcr = mode << 24;
                        const A64::LocationDescriptor location{0x1000, FP::FPCR{fpcr}};
                        IR::Block block{location};
                        A64::IREmitter ir{block, location};
                        if (vector) {
                            const auto a = ir.GetQ(A64::Vec::V0);
                            const auto b = ir.GetQ(A64::Vec::V1);
                            const auto result = is_max ? ir.FPVectorMaxNumeric(width, a, b, controlled)
                                                       : ir.FPVectorMinNumeric(width, a, b, controlled);
                            ir.SetQ(A64::Vec::V2, result);
                        } else if (width == 32) {
                            const auto a = ir.GetW(A64::Reg::R0);
                            const auto b = ir.GetW(A64::Reg::R1);
                            ir.SetW(A64::Reg::R2, IR::U32{is_max ? ir.FPMaxNumeric(a, b) : ir.FPMinNumeric(a, b)});
                        } else {
                            const auto a = ir.GetX(A64::Reg::R0);
                            const auto b = ir.GetX(A64::Reg::R1);
                            ir.SetX(A64::Reg::R2, IR::U64{is_max ? ir.FPMaxNumeric(a, b) : ir.FPMinNumeric(a, b)});
                        }
                        ir.SetTerm(IR::Term::ReturnToDispatch{});
                        A64::UserConfig user_config;
                        Backend::Wasm::EmitConfig config{false, true};
                        config.frontend = Backend::Wasm::Frontend::A64;
                        config.a64_conf = &user_config;
                        const auto emitted = Backend::Wasm::EmitWASM(std::move(block), config);
                        const auto name = std::to_string(width) + "-" + std::to_string(mode) + "-" +
                                          std::to_string(is_max) + "-" + std::to_string(vector) + "-" +
                                          std::to_string(controlled) + ".wasm";
                        std::ofstream out{dir / name, std::ios::binary};
                        out.write(reinterpret_cast<const char*>(emitted.module_bytes.data()),
                                  static_cast<std::streamsize>(emitted.module_bytes.size()));
                        if (!out) return 1;
                    }
                }
            }
        }
    }
    std::printf("{\"registers\":%zu,\"vectors\":%zu,\"fpsr\":%zu}\n",
                offsetof(Backend::Wasm::A64JitState, reg),
                offsetof(Backend::Wasm::A64JitState, vec),
                offsetof(Backend::Wasm::A64JitState, fpsr));
}
