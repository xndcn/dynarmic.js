// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
// Constant-address memory modules, checked with an independent byte oracle.
#include <cstdio>
#include <fstream>
#include <string>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/interface/A32/arch_version.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

using namespace Dynarmic;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    for (const unsigned width : {1u, 2u, 4u, 8u}) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            for (unsigned crossing = 0; crossing < 4; ++crossing) {
                const A32::LocationDescriptor location{0x1000, {}, {}};
                IR::Block block{location};
                A32::IREmitter ir{block, location, A32::ArchVersion::v8};
                const auto source = ir.Imm32((crossing & 1) ? 0x1fff : 0x1001);
                const auto destination = ir.Imm32((crossing & 2) ? 0x3fff : 0x3001);
                constexpr auto acc = IR::AccType::NORMAL;
                switch (width) {
                case 1: ir.WriteMemory8(destination, ir.ReadMemory8(source, acc), acc); break;
                case 2: ir.WriteMemory16(destination, ir.ReadMemory16(source, acc), acc); break;
                case 4: ir.WriteMemory32(destination, ir.ReadMemory32(source, acc), acc); break;
                case 8: ir.WriteMemory64(destination, ir.ReadMemory64(source, acc), acc); break;
                }
                ir.SetTerm(IR::Term::ReturnToDispatch{});
                Backend::Wasm::EmitConfig config{false, true};
                Backend::Wasm::BoundaryState boundary{};
                config.boundary_state = &boundary;
                config.page_table_base = 0x2000;
                config.absolute_offset_page_table = (flags & 1) != 0;
                config.page_table_pointer_mask_bits = (flags & 2) ? 2 : 0;
                const auto emitted = Backend::Wasm::EmitWASM(std::move(block), config);
                const auto name = "/memory_" + std::to_string(width) + "_" +
                                  std::to_string(flags) + "_" + std::to_string(crossing) + ".wasm";
                std::ofstream out{std::string{argv[1]} + name, std::ios::binary};
                out.write(reinterpret_cast<const char*>(emitted.module_bytes.data()),
                          static_cast<std::streamsize>(emitted.module_bytes.size()));
                if (!out.good()) return 1;
            }
        }
    }
    std::puts("{}");
}
