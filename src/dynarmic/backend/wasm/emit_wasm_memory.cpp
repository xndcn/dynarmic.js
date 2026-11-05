// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Memory access opcodes. When a page table is configured, accesses are
// inlined against the shared linear memory (fastmem), falling back to the
// imported host thunks on null page entries. Otherwise all accesses go
// through the thunks.

#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

constexpr u32 kPageBits = 12;
constexpr u32 kPageMask = (1 << kPageBits) - 1;

// Pushes the page table entry pointer for the vaddr held in local `tv`,
// or 0 if unmapped. Consumes nothing.
void EmitPageLookup(EmitContext& ctx, u32 tv) {
    // tp = *(u32*)(page_table_base + (vaddr >> 12) * 4)
    ctx.code.LocalGet(tv);
    ctx.code.I32Const(kPageBits);
    ctx.code.U8(Op::I32ShrU);
    ctx.code.I32Const(2);
    ctx.code.U8(Op::I32Shl);
    ctx.code.I32Const(static_cast<s32>(ctx.emit_conf.page_table_base));
    ctx.code.U8(Op::I32Add);
    ctx.code.Load(Op::I32Load, 0, 2);
    if (ctx.emit_conf.page_table_pointer_mask_bits > 0) {
        const u32 mask = ~((1u << ctx.emit_conf.page_table_pointer_mask_bits) - 1);
        ctx.code.I32Const(static_cast<s32>(mask));
        ctx.code.U8(Op::I32And);
    }
}

// WebAssembly supports unaligned accesses, but the next guest page need not
// share backing storage or permissions. Only page-contained accesses may use
// one translated pointer. Null entries (MMIO/protection/missing) remain slow.
void EmitPageContainedPointer(EmitContext& ctx, u32 tv, u32 tp, u32 width) {
    ctx.code.LocalGet(tp);
    if (width > 1) {
        ctx.code.I32Const(0);
        ctx.code.LocalGet(tv);
        ctx.code.I32Const(kPageMask);
        ctx.code.U8(Op::I32And);
        ctx.code.I32Const((1u << kPageBits) - width);
        ctx.code.U8(Op::I32LeU);
        ctx.code.U8(Op::Select);
    }
}

// Pushes the linear-memory address for page pointer `tp` and vaddr `tv`.
void EmitEffectiveAddress(EmitContext& ctx, u32 tp, u32 tv) {
    ctx.code.LocalGet(tp);
    ctx.code.LocalGet(tv);
    if (!ctx.emit_conf.absolute_offset_page_table) {
        ctx.code.I32Const(static_cast<s32>(kPageMask));
        ctx.code.U8(Op::I32And);
    }
    ctx.code.U8(Op::I32Add);
}

// Thunk signature: (state, ectx, vaddr) -> value
void EmitReadThunkCall(EmitContext& ctx, const RegAlloc::Argument& vaddr_arg, const char* name, ValType result_type) {
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32}, {result_type}});
    const u32 fn = ctx.Import(name, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    EmitPush(ctx.code, ctx.reg_alloc, vaddr_arg);
    ctx.code.Call(fn);
}

void EmitReadThunk(EmitContext& ctx, IR::Inst* inst, const char* name, ValType result_type) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitReadThunkCall(ctx, args[1], name, result_type);
    ctx.code.LocalSet(dst);
}

// Literal and absolute addresses already have a known page and offset. Keep
// the page pointer dynamic (unmapping, permissions and MMIO can still change),
// but resolve page containment and indexing during compilation.
void EmitConstantPage(EmitContext& ctx, u32 address) {
    ctx.code.I32Const(static_cast<s32>(ctx.emit_conf.page_table_base));
    ctx.code.Load(Op::I32Load, (address >> kPageBits) * 4, 2);
    if (ctx.emit_conf.page_table_pointer_mask_bits > 0) {
        const u32 mask = ~((1u << ctx.emit_conf.page_table_pointer_mask_bits) - 1);
        ctx.code.I32Const(static_cast<s32>(mask));
        ctx.code.U8(Op::I32And);
    }
}

void EmitConstantAddress(EmitContext& ctx, u32 pointer, u32 address) {
    ctx.code.LocalGet(pointer);
    ctx.code.I32Const(static_cast<s32>(ctx.emit_conf.absolute_offset_page_table ? address : address & kPageMask));
    ctx.code.U8(Op::I32Add);
}

// Read with inline fastmem: fast path loads from shared linear memory via the
// page table, slow path (null page entry) calls the host thunk.
void EmitReadFastmem(EmitContext& ctx, IR::Inst* inst, const char* name, ValType result_type, u8 load_op) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);

    if (ctx.emit_conf.enable_constant_memory && ctx.emit_conf.flat_limit == 0 && args[1].IsImmediate()) {
        const u32 address = args[1].GetImmediateU32();
        const u32 width = load_op == Op::I32Load8U ? 1 : load_op == Op::I32Load16U ? 2 : load_op == Op::I64Load ? 8 : 4;
        if ((address & kPageMask) <= (1u << kPageBits) - width) {
            const u32 tp = ctx.Temp(ValType::I32);
            EmitConstantPage(ctx, address);
            ctx.code.LocalTee(tp);
            ctx.code.If({{result_type}});
            EmitConstantAddress(ctx, tp, address);
            ctx.code.Load(load_op, 0, 0);
            ctx.code.Else();
            EmitReadThunkCall(ctx, args[1], name, result_type);
            ctx.code.End();
        } else {
            EmitReadThunkCall(ctx, args[1], name, result_type);
        }
        ctx.code.LocalSet(dst);
        return;
    }

    const u32 tv = ctx.Temp(ValType::I32);
    const u32 tp = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tv);
    if (ctx.emit_conf.flat_limit != 0) {
        // Flat window: bounds check + load with the window base as the
        // memarg offset.
        ctx.code.LocalGet(tv);
        ctx.code.I32Const(static_cast<s32>(ctx.emit_conf.flat_limit));
        ctx.code.U8(Op::I32LtU);
        ctx.code.If({{result_type}});
        ctx.code.LocalGet(tv);
        ctx.code.Load(load_op, ctx.emit_conf.flat_base, 0);
        ctx.code.Else();
        EmitReadThunkCall(ctx, args[1], name, result_type);
        ctx.code.End();
        ctx.code.LocalSet(dst);
        return;
    }
    EmitPageLookup(ctx, tv);
    ctx.code.LocalSet(tp);

    const u32 width = load_op == Op::I32Load8U ? 1 : load_op == Op::I32Load16U ? 2 : load_op == Op::I64Load ? 8 : 4;
    EmitPageContainedPointer(ctx, tv, tp, width);
    ctx.code.If({{result_type}});
    {
        // Fast path: load from tp + (vaddr & 0xFFF) [or tp + vaddr].
        EmitEffectiveAddress(ctx, tp, tv);
        ctx.code.Load(load_op, 0, 0);
    }
    ctx.code.Else();
    {
        EmitReadThunkCall(ctx, args[1], name, result_type);
    }
    ctx.code.End();
    ctx.code.LocalSet(dst);
}

// Thunk signature: (state, ectx, vaddr, value) -> ()
void EmitWriteThunkCall(EmitContext& ctx, const RegAlloc::Argument& vaddr_arg, const RegAlloc::Argument& value_arg, const char* name, ValType value_type) {
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, value_type}, {}});
    const u32 fn = ctx.Import(name, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    EmitPush(ctx.code, ctx.reg_alloc, vaddr_arg);
    EmitPush(ctx.code, ctx.reg_alloc, value_arg);
    ctx.code.Call(fn);
}

void EmitWriteThunk(EmitContext& ctx, IR::Inst* inst, const char* name, ValType value_type) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    EmitWriteThunkCall(ctx, args[1], args[2], name, value_type);
}

void EmitWriteFastmem(EmitContext& ctx, IR::Inst* inst, const char* name, ValType value_type, u8 store_op) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);

    if (ctx.emit_conf.enable_constant_memory && ctx.emit_conf.flat_limit == 0 && args[1].IsImmediate()) {
        const u32 address = args[1].GetImmediateU32();
        const u32 width = store_op == Op::I32Store8 ? 1 : store_op == Op::I32Store16 ? 2 : store_op == Op::I64Store ? 8 : 4;
        if ((address & kPageMask) <= (1u << kPageBits) - width) {
            const u32 tp = ctx.Temp(ValType::I32);
            EmitConstantPage(ctx, address);
            ctx.code.LocalTee(tp);
            ctx.code.If();
            EmitConstantAddress(ctx, tp, address);
            EmitPush(ctx.code, ctx.reg_alloc, args[2]);
            ctx.code.Store(store_op, 0, 0);
            ctx.code.Else();
            EmitWriteThunkCall(ctx, args[1], args[2], name, value_type);
            ctx.code.End();
        } else {
            EmitWriteThunkCall(ctx, args[1], args[2], name, value_type);
        }
        return;
    }

    const u32 tv = ctx.Temp(ValType::I32);
    const u32 tp = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tv);
    if (ctx.emit_conf.flat_limit != 0) {
        ctx.code.LocalGet(tv);
        ctx.code.I32Const(static_cast<s32>(ctx.emit_conf.flat_limit));
        ctx.code.U8(Op::I32LtU);
        ctx.code.If();
        ctx.code.LocalGet(tv);
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);
        ctx.code.Store(store_op, ctx.emit_conf.flat_base, 0);
        ctx.code.Else();
        EmitWriteThunkCall(ctx, args[1], args[2], name, value_type);
        ctx.code.End();
        return;
    }
    EmitPageLookup(ctx, tv);
    ctx.code.LocalSet(tp);

    const u32 width = store_op == Op::I32Store8 ? 1 : store_op == Op::I32Store16 ? 2 : store_op == Op::I64Store ? 8 : 4;
    EmitPageContainedPointer(ctx, tv, tp, width);
    ctx.code.If();
    {
        EmitEffectiveAddress(ctx, tp, tv);
        EmitPush(ctx.code, ctx.reg_alloc, args[2]);
        ctx.code.Store(store_op, 0, 0);
    }
    ctx.code.Else();
    {
        EmitWriteThunkCall(ctx, args[1], args[2], name, value_type);
    }
    ctx.code.End();
}

bool UseFastmem(const EmitContext& ctx) {
    return ctx.emit_conf.page_table_base != 0;
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::A32ReadMemory8>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitReadFastmem(ctx, inst, kImportReadMemory8, ValType::I32, Op::I32Load8U);
    } else {
        EmitReadThunk(ctx, inst, kImportReadMemory8, ValType::I32);
    }
}

template<>
void EmitIR<IR::Opcode::A32ReadMemory16>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitReadFastmem(ctx, inst, kImportReadMemory16, ValType::I32, Op::I32Load16U);
    } else {
        EmitReadThunk(ctx, inst, kImportReadMemory16, ValType::I32);
    }
}

template<>
void EmitIR<IR::Opcode::A32ReadMemory32>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitReadFastmem(ctx, inst, kImportReadMemory32, ValType::I32, Op::I32Load);
    } else {
        EmitReadThunk(ctx, inst, kImportReadMemory32, ValType::I32);
    }
}

template<>
void EmitIR<IR::Opcode::A32ReadMemory64>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitReadFastmem(ctx, inst, kImportReadMemory64, ValType::I64, Op::I64Load);
    } else {
        EmitReadThunk(ctx, inst, kImportReadMemory64, ValType::I64);
    }
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveReadMemory8>(EmitContext& ctx, IR::Inst* inst) {
    EmitReadThunk(ctx, inst, kImportExclusiveReadMemory8, ValType::I32);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveReadMemory16>(EmitContext& ctx, IR::Inst* inst) {
    EmitReadThunk(ctx, inst, kImportExclusiveReadMemory16, ValType::I32);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveReadMemory32>(EmitContext& ctx, IR::Inst* inst) {
    EmitReadThunk(ctx, inst, kImportExclusiveReadMemory32, ValType::I32);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveReadMemory64>(EmitContext& ctx, IR::Inst* inst) {
    EmitReadThunk(ctx, inst, kImportExclusiveReadMemory64, ValType::I64);
}

template<>
void EmitIR<IR::Opcode::A32WriteMemory8>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitWriteFastmem(ctx, inst, kImportWriteMemory8, ValType::I32, Op::I32Store8);
    } else {
        EmitWriteThunk(ctx, inst, kImportWriteMemory8, ValType::I32);
    }
}

template<>
void EmitIR<IR::Opcode::A32WriteMemory16>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitWriteFastmem(ctx, inst, kImportWriteMemory16, ValType::I32, Op::I32Store16);
    } else {
        EmitWriteThunk(ctx, inst, kImportWriteMemory16, ValType::I32);
    }
}

template<>
void EmitIR<IR::Opcode::A32WriteMemory32>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitWriteFastmem(ctx, inst, kImportWriteMemory32, ValType::I32, Op::I32Store);
    } else {
        EmitWriteThunk(ctx, inst, kImportWriteMemory32, ValType::I32);
    }
}

template<>
void EmitIR<IR::Opcode::A32WriteMemory64>(EmitContext& ctx, IR::Inst* inst) {
    if (UseFastmem(ctx)) {
        EmitWriteFastmem(ctx, inst, kImportWriteMemory64, ValType::I64, Op::I64Store);
    } else {
        EmitWriteThunk(ctx, inst, kImportWriteMemory64, ValType::I64);
    }
}

// Thunk signature: (state, ectx, vaddr, value) -> i32 (0 = success, ARM semantics)
void EmitExclusiveWriteThunk(EmitContext& ctx, IR::Inst* inst, const char* name, ValType value_type) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32, ValType::I32, value_type}, {ValType::I32}});
    const u32 fn = ctx.Import(name, ty);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    ctx.code.Call(fn);
    ctx.code.LocalSet(dst);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveWriteMemory8>(EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteThunk(ctx, inst, kImportExclusiveWriteMemory8, ValType::I32);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveWriteMemory16>(EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteThunk(ctx, inst, kImportExclusiveWriteMemory16, ValType::I32);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveWriteMemory32>(EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteThunk(ctx, inst, kImportExclusiveWriteMemory32, ValType::I32);
}

template<>
void EmitIR<IR::Opcode::A32ExclusiveWriteMemory64>(EmitContext& ctx, IR::Inst* inst) {
    EmitExclusiveWriteThunk(ctx, inst, kImportExclusiveWriteMemory64, ValType::I64);
}

template<>
void EmitIR<IR::Opcode::A32ClearExclusive>(EmitContext& ctx, IR::Inst*) {
    const u32 ty = ctx.Type({{ValType::I32, ValType::I32}, {}});
    const u32 fn = ctx.Import(kImportClearExclusive, ty);
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.Call(fn);
}

}  // namespace Dynarmic::Backend::Wasm
