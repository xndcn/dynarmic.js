// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// A32 coprocessor ops. The coprocessor's Compile* decision is made at emit
// time (like the x64 backend): a Callback becomes an indirect host call
// through the coproc thunk, a u32* / array<u32*, 2> becomes an inline
// linear-memory access (wasm host pointers are linear addresses), and
// monostate raises UndefinedInstruction via the raise thunk.

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

A32::Coprocessor* GetCoproc(const EmitConfig& conf, size_t num) {
    if (!conf.coprocessors) {
        return nullptr;
    }
    return (*conf.coprocessors)[num].get();
}

// Emits a call to the raise thunk reporting an undefined instruction at this
// block's PC (the x64 backend ASSERT_FALSEs here; raising keeps us running).
void EmitCoprocException(EmitContext& ctx) {
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I64}, {}});
    const u32 fn = ctx.Import(kImportExceptionRaised, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.I32Const(static_cast<s32>(A32::LocationDescriptor{ctx.block->Location()}.PC()));
    ctx.code.I64Const(static_cast<s64>(A32::Exception::UndefinedInstruction));
    ctx.code.Call(fn);
}

// Emits the fixed prefix of a coproc thunk call:
// (state, ectx, fn, user_arg, ...) -> u64. The caller pushes arg0/arg1
// afterwards and then calls EmitCoprocCallbackCallEnd.
u32 EmitCoprocCallbackCallBegin(EmitContext& ctx, const A32::Coprocessor::Callback& cb) {
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32, ValType::I32}, {ValType::I64}});
    const u32 fn = ctx.Import(kImportCoprocCallback, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.I32Const(static_cast<s32>(reinterpret_cast<uptr>(cb.function)));
    ctx.code.I32Const(static_cast<s32>(cb.user_arg ? reinterpret_cast<uptr>(*cb.user_arg) : 0));
    return fn;
}

// Inline u32 linear-memory load of a host pointer.
void EmitLoadHostWord(EmitContext& ctx, const u32* ptr) {
    ctx.code.I32Const(static_cast<s32>(reinterpret_cast<uptr>(ptr)));
    ctx.code.Load(Op::I32Load, 0, 2);
}

// Inline u32 linear-memory store to a host pointer; the value is already on
// the stack below the address we push here.
void EmitStoreHostWord(EmitContext& ctx, const u32* ptr) {
    // stack: [value]; stores take [addr, value], so swap via a temp.
    const u32 tv = ctx.Temp(ValType::I32);
    ctx.code.LocalSet(tv);
    ctx.code.I32Const(static_cast<s32>(reinterpret_cast<uptr>(ptr)));
    ctx.code.LocalGet(tv);
    ctx.code.Store(Op::I32Store, 0, 2);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::A32CoprocInternalOperation>(EmitContext& ctx, IR::Inst* inst) {
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const auto opc1 = static_cast<unsigned>(coproc_info[2]);
    const auto CRd = static_cast<A32::CoprocReg>(coproc_info[3]);
    const auto CRn = static_cast<A32::CoprocReg>(coproc_info[4]);
    const auto CRm = static_cast<A32::CoprocReg>(coproc_info[5]);
    const auto opc2 = static_cast<unsigned>(coproc_info[6]);

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        return;
    }

    const auto action = coproc->CompileInternalOperation(two, opc1, CRd, CRn, CRm, opc2);
    if (!action) {
        EmitCoprocException(ctx);
        return;
    }

    const u32 fn = EmitCoprocCallbackCallBegin(ctx, *action);
    ctx.code.I32Const(0);  // arg0 (ignored)
    ctx.code.I32Const(0);  // arg1 (ignored)
    ctx.code.Call(fn);
    ctx.code.U8(Op::Drop);
}

template<>
void EmitIR<IR::Opcode::A32CoprocSendOneWord>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const auto opc1 = static_cast<unsigned>(coproc_info[2]);
    const auto CRn = static_cast<A32::CoprocReg>(coproc_info[3]);
    const auto CRm = static_cast<A32::CoprocReg>(coproc_info[4]);
    const auto opc2 = static_cast<unsigned>(coproc_info[5]);

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        return;
    }

    const auto action = coproc->CompileSendOneWord(two, opc1, CRn, CRm, opc2);
    if (std::holds_alternative<std::monostate>(action)) {
        EmitCoprocException(ctx);
        return;
    }

    if (const auto cb = std::get_if<A32::Coprocessor::Callback>(&action)) {
        const u32 fn = EmitCoprocCallbackCallBegin(ctx, *cb);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // arg0 = word
        ctx.code.I32Const(0);                        // arg1 (ignored)
        ctx.code.Call(fn);
        ctx.code.U8(Op::Drop);
        return;
    }

    if (const auto destination_ptr = std::get_if<u32*>(&action)) {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        EmitStoreHostWord(ctx, *destination_ptr);
        return;
    }

    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::A32CoprocSendTwoWords>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const auto opc = static_cast<unsigned>(coproc_info[2]);
    const auto CRm = static_cast<A32::CoprocReg>(coproc_info[3]);

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        return;
    }

    const auto action = coproc->CompileSendTwoWords(two, opc, CRm);
    if (std::holds_alternative<std::monostate>(action)) {
        EmitCoprocException(ctx);
        return;
    }

    if (const auto cb = std::get_if<A32::Coprocessor::Callback>(&action)) {
        const u32 fn = EmitCoprocCallbackCallBegin(ctx, *cb);
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // arg0 = word1
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);  // arg1 = word2
        ctx.code.Call(fn);
        ctx.code.U8(Op::Drop);
        return;
    }

    if (const auto destination_ptrs = std::get_if<std::array<u32*, 2>>(&action)) {
        EmitPush(ctx.code, ctx.reg_alloc, args[1]);
        EmitStoreHostWord(ctx, (*destination_ptrs)[0]);
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);
        EmitStoreHostWord(ctx, (*destination_ptrs)[1]);
        return;
    }

    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::A32CoprocGetOneWord>(EmitContext& ctx, IR::Inst* inst) {
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const auto opc1 = static_cast<unsigned>(coproc_info[2]);
    const auto CRn = static_cast<A32::CoprocReg>(coproc_info[3]);
    const auto CRm = static_cast<A32::CoprocReg>(coproc_info[4]);
    const auto opc2 = static_cast<unsigned>(coproc_info[5]);

    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        ctx.code.I32Const(0);
        ctx.code.LocalSet(dst);
        return;
    }

    const auto action = coproc->CompileGetOneWord(two, opc1, CRn, CRm, opc2);
    if (std::holds_alternative<std::monostate>(action)) {
        EmitCoprocException(ctx);
        ctx.code.I32Const(0);
        ctx.code.LocalSet(dst);
        return;
    }

    if (const auto cb = std::get_if<A32::Coprocessor::Callback>(&action)) {
        const u32 fn = EmitCoprocCallbackCallBegin(ctx, *cb);
        ctx.code.I32Const(0);  // arg0 (ignored)
        ctx.code.I32Const(0);  // arg1 (ignored)
        ctx.code.Call(fn);
        ctx.code.U8(Op::I32WrapI64);
        ctx.code.LocalSet(dst);
        return;
    }

    if (const auto source_ptr = std::get_if<u32*>(&action)) {
        EmitLoadHostWord(ctx, *source_ptr);
        ctx.code.LocalSet(dst);
        return;
    }

    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::A32CoprocGetTwoWords>(EmitContext& ctx, IR::Inst* inst) {
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const auto opc = static_cast<unsigned>(coproc_info[2]);
    const auto CRm = static_cast<A32::CoprocReg>(coproc_info[3]);

    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        ctx.code.I64Const(0);
        ctx.code.LocalSet(dst);
        return;
    }

    auto action = coproc->CompileGetTwoWords(two, opc, CRm);
    if (std::holds_alternative<std::monostate>(action)) {
        EmitCoprocException(ctx);
        ctx.code.I64Const(0);
        ctx.code.LocalSet(dst);
        return;
    }

    if (const auto cb = std::get_if<A32::Coprocessor::Callback>(&action)) {
        const u32 fn = EmitCoprocCallbackCallBegin(ctx, *cb);
        ctx.code.I32Const(0);  // arg0 (ignored)
        ctx.code.I32Const(0);  // arg1 (ignored)
        ctx.code.Call(fn);
        ctx.code.LocalSet(dst);
        return;
    }

    if (const auto source_ptrs = std::get_if<std::array<u32*, 2>>(&action)) {
        // result = (u64(hi) << 32) | lo
        EmitLoadHostWord(ctx, (*source_ptrs)[1]);
        ctx.code.U8(Op::I64ExtendI32U);
        ctx.code.I64Const(32);
        ctx.code.U8(Op::I64Shl);
        EmitLoadHostWord(ctx, (*source_ptrs)[0]);
        ctx.code.U8(Op::I64ExtendI32U);
        ctx.code.U8(Op::I64Or);
        ctx.code.LocalSet(dst);
        return;
    }

    UNREACHABLE();
}

template<>
void EmitIR<IR::Opcode::A32CoprocLoadWords>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const bool long_transfer = coproc_info[2] != 0;
    const auto CRd = static_cast<A32::CoprocReg>(coproc_info[3]);
    const bool has_option = coproc_info[4] != 0;

    std::optional<u8> option = std::nullopt;
    if (has_option) {
        option = coproc_info[5];
    }

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        return;
    }

    const auto action = coproc->CompileLoadWords(two, long_transfer, CRd, option);
    if (!action) {
        EmitCoprocException(ctx);
        return;
    }

    const u32 fn = EmitCoprocCallbackCallBegin(ctx, *action);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // arg0 = start address
    ctx.code.I32Const(0);                        // arg1 (ignored)
    ctx.code.Call(fn);
    ctx.code.U8(Op::Drop);
}

template<>
void EmitIR<IR::Opcode::A32CoprocStoreWords>(EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto coproc_info = inst->GetArg(0).GetCoprocInfo();
    const size_t coproc_num = coproc_info[0];
    const bool two = coproc_info[1] != 0;
    const bool long_transfer = coproc_info[2] != 0;
    const auto CRd = static_cast<A32::CoprocReg>(coproc_info[3]);
    const bool has_option = coproc_info[4] != 0;

    std::optional<u8> option = std::nullopt;
    if (has_option) {
        option = coproc_info[5];
    }

    A32::Coprocessor* coproc = GetCoproc(ctx.emit_conf, coproc_num);
    if (!coproc) {
        EmitCoprocException(ctx);
        return;
    }

    const auto action = coproc->CompileStoreWords(two, long_transfer, CRd, option);
    if (!action) {
        EmitCoprocException(ctx);
        return;
    }

    const u32 fn = EmitCoprocCallbackCallBegin(ctx, *action);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // arg0 = start address
    ctx.code.I32Const(0);                        // arg1 (ignored)
    ctx.code.Call(fn);
    ctx.code.U8(Op::Drop);
}

}  // namespace Dynarmic::Backend::Wasm
