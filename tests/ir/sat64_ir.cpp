// IR-level test for the 64-bit-lane saturating vector add/sub emitters (the
// A32 frontend rejects VQADD.64, so no guest program can reach them).
// Builds: Q3 = sqadd64(Q1,Q2); Q4 = sqsub64; Q5 = uqadd64; Q6 = uqsub64.
#include <cstdio>
#include <fstream>
#include <utility>
#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_ir_emitter.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/a32_types.h"
#include "dynarmic/interface/A32/arch_version.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"
using namespace Dynarmic;
int main(int, char** argv) {
    IR::Block block{A32::LocationDescriptor{0x1000, {}, {}}};
    A32::IREmitter ir{block, A32::LocationDescriptor{0x1000, {}, {}}, A32::ArchVersion::v8};
    const auto a = ir.GetVector(A32::ExtReg::Q1);
    const auto b = ir.GetVector(A32::ExtReg::Q2);
    ir.SetVector(A32::ExtReg::Q3, ir.VectorSignedSaturatedAdd(64, a, b));
    ir.SetVector(A32::ExtReg::Q4, ir.VectorSignedSaturatedSub(64, a, b));
    ir.SetVector(A32::ExtReg::Q5, ir.VectorUnsignedSaturatedAdd(64, a, b));
    ir.SetVector(A32::ExtReg::Q6, ir.VectorUnsignedSaturatedSub(64, a, b));
    ir.SetTerm(IR::Term::ReturnToDispatch{});
    const Backend::Wasm::EmitConfig conf{false, true};
    const auto ebi = Backend::Wasm::EmitWASM(std::move(block), conf);
    std::ofstream out{argv[1], std::ios::binary};
    out.write(reinterpret_cast<const char*>(ebi.module_bytes.data()), static_cast<std::streamsize>(ebi.module_bytes.size()));
    std::printf("%zu %zu\n", offsetof(Backend::Wasm::A32JitState, ext_regs), offsetof(Backend::Wasm::A32JitState, fpsr));
}
