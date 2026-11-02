// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

#include <cstdio>
#include <fstream>
#include <string>
#include <utility>

#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A64/a64_ir_emitter.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    A64::UserConfig user_config;
    for (const unsigned width : {1u, 2u, 4u, 8u, 16u}) {
        for (unsigned flags = 0; flags < 8; ++flags) {
            const A64::LocationDescriptor location{0x1000, FP::FPCR{}};
            IR::Block block{location};
            A64::IREmitter ir{block, location};
            const auto source = ir.GetX(A64::Reg::R0);
            const auto destination = ir.GetX(A64::Reg::R1);
            constexpr auto acc = IR::AccType::NORMAL;
            switch (width) {
            case 1: ir.WriteMemory8(destination, ir.ReadMemory8(source, acc), acc); break;
            case 2: ir.WriteMemory16(destination, ir.ReadMemory16(source, acc), acc); break;
            case 4: ir.WriteMemory32(destination, ir.ReadMemory32(source, acc), acc); break;
            case 8: ir.WriteMemory64(destination, ir.ReadMemory64(source, acc), acc); break;
            case 16: ir.WriteMemory128(destination, ir.ReadMemory128(source, acc), acc); break;
            }
            ir.SetTerm(IR::Term::ReturnToDispatch{});
            Backend::Wasm::EmitConfig config{false, true};
            config.frontend = Backend::Wasm::Frontend::A64;
            config.a64_conf = &user_config;
            config.page_table_base = 0x2000;
            config.page_table_address_space_bits = 16;
            config.absolute_offset_page_table = (flags & 1) != 0;
            config.silently_mirror_page_table = (flags & 2) != 0;
            config.page_table_pointer_mask_bits = (flags & 4) ? 2 : 0;
            const auto emitted = Backend::Wasm::EmitWASM(std::move(block), config);
            std::ofstream out{std::string{argv[1]} + "/memory_" + std::to_string(width) + "_" + std::to_string(flags) + ".wasm", std::ios::binary};
            out.write(reinterpret_cast<const char*>(emitted.module_bytes.data()),
                      static_cast<std::streamsize>(emitted.module_bytes.size()));
            if (!out.good()) return 1;
        }
    }
    std::printf("{\"reg\":%zu,\"scratch\":%zu}\n",
                offsetof(Backend::Wasm::A64JitState, reg),
                offsetof(Backend::Wasm::A64JitState, mem_scratch));
}
