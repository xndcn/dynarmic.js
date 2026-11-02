// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.

// ARMv8 crypto extensions (AESE/AESD/AESMC/AESIMC, SHA256H/H2/SU0/SU1) for
// the WASM backend. Wasm has no AES/SHA instructions, so each op stages its
// v128 operands in state->crypto_scratch, calls one host thunk, and loads the
// v128 result back.

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {
namespace {

constexpr size_t kScratchSlotBytes = 16;
constexpr size_t kResultSlot = 3;

void EmitCrypto(EmitContext& ctx, IR::Inst* inst, CryptoOp op, size_t num_args) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    FuncBody& code = ctx.code;
    const u32 dst = ctx.reg_alloc.DefineLocal(code, inst);

    for (size_t i = 0; i < num_args; i++) {
        code.LocalGet(kParamState);
        EmitPush(code, ctx.reg_alloc, args[i]);
        code.Simd(Op::V128Store);
        code.ULEB(0);  // align
        code.ULEB(ctx.state.crypto_scratch + i * kScratchSlotBytes);
    }

    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32}, {}});
    const u32 fn = ctx.Import(kImportCrypto, ty);
    code.LocalGet(kParamState);
    code.I32Const(static_cast<s32>(ctx.state.crypto_scratch));
    code.U8(Op::I32Add);
    code.LocalGet(kParamEctx);
    code.I32Const(static_cast<s32>(op));
    code.Call(fn);

    code.LocalGet(kParamState);
    code.Simd(Op::V128Load);
    code.ULEB(0);  // align
    code.ULEB(ctx.state.crypto_scratch + kResultSlot * kScratchSlotBytes);
    code.LocalSet(dst);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::AESEncryptSingleRound>(EmitContext& ctx, IR::Inst* inst) {
    EmitCrypto(ctx, inst, CryptoOp::AesEncryptRound, 1);
}

template<>
void EmitIR<IR::Opcode::AESDecryptSingleRound>(EmitContext& ctx, IR::Inst* inst) {
    EmitCrypto(ctx, inst, CryptoOp::AesDecryptRound, 1);
}

template<>
void EmitIR<IR::Opcode::AESMixColumns>(EmitContext& ctx, IR::Inst* inst) {
    EmitCrypto(ctx, inst, CryptoOp::AesMixColumns, 1);
}

template<>
void EmitIR<IR::Opcode::AESInverseMixColumns>(EmitContext& ctx, IR::Inst* inst) {
    EmitCrypto(ctx, inst, CryptoOp::AesInverseMixColumns, 1);
}

template<>
void EmitIR<IR::Opcode::SHA256Hash>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool part1 = args[3].GetImmediateU1();
    EmitCrypto(ctx, inst, part1 ? CryptoOp::Sha256HashPart1 : CryptoOp::Sha256HashPart2, 3);
}

template<>
void EmitIR<IR::Opcode::SHA256MessageSchedule0>(EmitContext& ctx, IR::Inst* inst) {
    EmitCrypto(ctx, inst, CryptoOp::Sha256MessageSchedule0, 2);
}

template<>
void EmitIR<IR::Opcode::SHA256MessageSchedule1>(EmitContext& ctx, IR::Inst* inst) {
    EmitCrypto(ctx, inst, CryptoOp::Sha256MessageSchedule1, 3);
}

}  // namespace Dynarmic::Backend::Wasm
