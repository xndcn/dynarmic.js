// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// A64 memory access opcodes. Virtual addresses are 64-bit; the fast paths
// (flat window, page table) turn them into 32-bit linear-memory addresses,
// everything else goes through the imported a64_* host thunks.

#include <mcl/assert.hpp>

#include "dynarmic/backend/wasm/a64_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

constexpr u32 kPageBits = 12;
constexpr u32 kPageMask = (1 << kPageBits) - 1;

// Access sizes, indexed like the kImportA64* name tables.
enum Size : u32 { B8, B16, B32, B64, B128 };

ValType ValueType(Size size) {
    switch (size) {
    case B64:
        return ValType::I64;
    case B128:
        return ValType::V128;
    default:
        return ValType::I32;
    }
}

// Thunk value type: 128-bit values travel through A64JitState::mem_scratch.
bool ThunkPassesValue(Size size) {
    return size != B128;
}

void EmitLoadOp(EmitContext& ctx, Size size, u32 offset) {
    switch (size) {
    case B8:
        ctx.code.Load(Op::I32Load8U, offset, 0);
        return;
    case B16:
        ctx.code.Load(Op::I32Load16U, offset, 0);
        return;
    case B32:
        ctx.code.Load(Op::I32Load, offset, 0);
        return;
    case B64:
        ctx.code.Load(Op::I64Load, offset, 0);
        return;
    case B128:
        ctx.code.Simd(Op::V128Load);
        ctx.code.ULEB(0);
        ctx.code.ULEB(offset);
        return;
    }
    UNREACHABLE();
}

void EmitStoreOp(EmitContext& ctx, Size size, u32 offset) {
    switch (size) {
    case B8:
        ctx.code.Store(Op::I32Store8, offset, 0);
        return;
    case B16:
        ctx.code.Store(Op::I32Store16, offset, 0);
        return;
    case B32:
        ctx.code.Store(Op::I32Store, offset, 0);
        return;
    case B64:
        ctx.code.Store(Op::I64Store, offset, 0);
        return;
    case B128:
        ctx.code.Simd(Op::V128Store);
        ctx.code.ULEB(0);
        ctx.code.ULEB(offset);
        return;
    }
    UNREACHABLE();
}

void EmitPushMemScratch(EmitContext& ctx) {
    ctx.code.LocalGet(kParamState);
    ctx.code.Simd(Op::V128Load);
    ctx.code.ULEB(0);
    ctx.code.ULEB(offsetof(A64JitState, mem_scratch));
}

// Stores the v128 on top of the stack into mem_scratch (state pushed first by caller).
void EmitStoreMemScratch(EmitContext& ctx) {
    ctx.code.Simd(Op::V128Store);
    ctx.code.ULEB(0);
    ctx.code.ULEB(offsetof(A64JitState, mem_scratch));
}

// Read thunk call for the vaddr in local `tv` (i64); leaves the value on the stack.
void EmitReadThunkCall(EmitContext& ctx, const char* name, Size size, u32 tv) {
    const FuncType sig = ThunkPassesValue(size)
                             ? FuncType{{ValType::I32, ValType::I32, ValType::I64}, {ValueType(size)}}
                             : FuncType{{ValType::I32, ValType::I32, ValType::I64}, {}};
    const u32 fn = ctx.Import(name, ctx.Type(sig));
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.LocalGet(tv);
    ctx.code.Call(fn);
    if (!ThunkPassesValue(size)) {
        EmitPushMemScratch(ctx);
    }
}

// Write thunk call for vaddr local `tv` and value `value_arg`; returns the
// thunk's result (exclusive writes) on the stack when `has_result`.
void EmitWriteThunkCall(EmitContext& ctx, const char* name, Size size, u32 tv, const RegAlloc::Argument& value_arg, bool has_result) {
    std::vector<ValType> params{ValType::I32, ValType::I32, ValType::I64};
    if (ThunkPassesValue(size)) {
        params.push_back(ValueType(size));
    } else {
        ctx.code.LocalGet(kParamState);
        EmitPush(ctx.code, ctx.reg_alloc, value_arg);
        EmitStoreMemScratch(ctx);
    }
    const FuncType sig = has_result ? FuncType{params, {ValType::I32}} : FuncType{params, {}};
    const u32 fn = ctx.Import(name, ctx.Type(sig));
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.LocalGet(tv);
    if (ThunkPassesValue(size)) {
        EmitPush(ctx.code, ctx.reg_alloc, value_arg);
    }
    ctx.code.Call(fn);
}

bool UseFastmem(const EmitContext& ctx) {
    return ctx.emit_conf.page_table_base != 0;
}

// Computes the fast-path linear-memory address for the vaddr in `tv` (i64)
// into `addr` (i32), and pushes the i32 "fast path usable" condition.
// For the flat window `addr` is the low 32 bits of the vaddr and the window
// base is applied as the memarg offset (returned through `memarg_offset`).
void EmitFastPathAddress(EmitContext& ctx, u32 tv, u32 addr, Size size, u32& memarg_offset) {
    FuncBody& code = ctx.code;
    const EmitConfig& conf = ctx.emit_conf;

    if (conf.flat_limit != 0) {
        memarg_offset = conf.flat_base;
        code.LocalGet(tv);
        code.U8(Op::I32WrapI64);
        code.LocalSet(addr);
        code.LocalGet(tv);
        code.I64Const(static_cast<s64>(conf.flat_limit));
        code.U8(Op::I64LtU);
        return;
    }

    memarg_offset = 0;
    const u32 page_bits = conf.page_table_address_space_bits - kPageBits;
    const u64 page_index_mask = (u64{1} << page_bits) - 1;

    // page = vaddr >> 12 (masked when mirroring; with no mirroring, a vaddr
    // beyond the table takes the slow path).
    auto push_entry = [&] {
        code.LocalGet(tv);
        code.I64Const(kPageBits);
        code.U8(Op::I64ShrU);
        if (conf.silently_mirror_page_table) {
            code.I64Const(static_cast<s64>(page_index_mask));
            code.U8(Op::I64And);
        }
        code.U8(Op::I32WrapI64);
        code.I32Const(2);
        code.U8(Op::I32Shl);
        code.Load(Op::I32Load, conf.page_table_base, 2);
        if (conf.page_table_pointer_mask_bits > 0) {
            code.I32Const(static_cast<s32>(~((1u << conf.page_table_pointer_mask_bits) - 1)));
            code.U8(Op::I32And);
        }
    };
    if (conf.silently_mirror_page_table) {
        push_entry();
    } else {
        code.LocalGet(tv);
        code.I64Const(static_cast<s64>(conf.page_table_address_space_bits));
        code.U8(Op::I64ShrU);
        code.U8(Op::I64Eqz);
        code.If({{ValType::I32}});
        push_entry();
        code.Else();
        code.I32Const(0);
        code.End();
    }
    // The entry stays on the stack as the condition (null = slow path).
    code.LocalTee(addr);

    // A translated pointer only covers this guest page. Adjacent pages may
    // have unrelated backing storage or permissions, including for v128
    // accesses. Keep unaligned accesses within the page on the fast path.
    const u32 width = 1u << static_cast<u32>(size);
    if (width > 1) {
        code.I32Const(0);
        code.LocalGet(tv);
        code.U8(Op::I32WrapI64);
        code.I32Const(kPageMask);
        code.U8(Op::I32And);
        code.I32Const((1u << kPageBits) - width);
        code.U8(Op::I32LeU);
        code.U8(Op::Select);
    }

    // addr = entry + (vaddr & 0xFFF)   [absolute: entry + vaddr]
    code.LocalGet(addr);
    code.LocalGet(tv);
    code.U8(Op::I32WrapI64);
    if (!conf.absolute_offset_page_table) {
        code.I32Const(static_cast<s32>(kPageMask));
        code.U8(Op::I32And);
    }
    code.U8(Op::I32Add);
    code.LocalSet(addr);
}

void EmitRead(EmitContext& ctx, IR::Inst* inst, Size size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tv = ctx.Temp(ValType::I64);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tv);

    if (!UseFastmem(ctx)) {
        EmitReadThunkCall(ctx, kImportA64Read[size], size, tv);
        ctx.code.LocalSet(dst);
        return;
    }

    const u32 addr = ctx.Temp(ValType::I32);
    u32 memarg_offset = 0;
    EmitFastPathAddress(ctx, tv, addr, size, memarg_offset);
    ctx.code.If({{ValueType(size)}});
    ctx.code.LocalGet(addr);
    EmitLoadOp(ctx, size, memarg_offset);
    ctx.code.Else();
    EmitReadThunkCall(ctx, kImportA64Read[size], size, tv);
    ctx.code.End();
    ctx.code.LocalSet(dst);
}

void EmitWrite(EmitContext& ctx, IR::Inst* inst, Size size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 tv = ctx.Temp(ValType::I64);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tv);

    if (!UseFastmem(ctx)) {
        EmitWriteThunkCall(ctx, kImportA64Write[size], size, tv, args[2], false);
        return;
    }

    const u32 addr = ctx.Temp(ValType::I32);
    u32 memarg_offset = 0;
    EmitFastPathAddress(ctx, tv, addr, size, memarg_offset);
    ctx.code.If();
    ctx.code.LocalGet(addr);
    EmitPush(ctx.code, ctx.reg_alloc, args[2]);
    EmitStoreOp(ctx, size, memarg_offset);
    ctx.code.Else();
    EmitWriteThunkCall(ctx, kImportA64Write[size], size, tv, args[2], false);
    ctx.code.End();
}

void EmitExclusiveRead(EmitContext& ctx, IR::Inst* inst, Size size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tv = ctx.Temp(ValType::I64);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tv);
    EmitReadThunkCall(ctx, kImportA64ExclusiveRead[size], size, tv);
    ctx.code.LocalSet(dst);
}

void EmitExclusiveWrite(EmitContext& ctx, IR::Inst* inst, Size size) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tv = ctx.Temp(ValType::I64);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.LocalSet(tv);
    EmitWriteThunkCall(ctx, kImportA64ExclusiveWrite[size], size, tv, args[2], true);
    ctx.code.LocalSet(dst);
}

}  // anonymous namespace

template<>
void EmitIR<IR::Opcode::A64ClearExclusive>(EmitContext& ctx, IR::Inst*) {
    const u32 fn = ctx.Import(kImportA64ClearExclusive, ctx.Type({{ValType::I32, ValType::I32}, {}}));
    ctx.code.LocalGet(kParamState);
    ctx.code.LocalGet(kParamEctx);
    ctx.code.Call(fn);
}

#define A64_MEMORY_OPS(bits, size)                                                     \
    template<>                                                                         \
    void EmitIR<IR::Opcode::A64ReadMemory##bits>(EmitContext & ctx, IR::Inst * inst) { \
        EmitRead(ctx, inst, size);                                                     \
    }                                                                                  \
    template<>                                                                         \
    void EmitIR<IR::Opcode::A64WriteMemory##bits>(EmitContext & ctx, IR::Inst * inst) { \
        EmitWrite(ctx, inst, size);                                                    \
    }                                                                                  \
    template<>                                                                         \
    void EmitIR<IR::Opcode::A64ExclusiveReadMemory##bits>(EmitContext & ctx, IR::Inst * inst) { \
        EmitExclusiveRead(ctx, inst, size);                                            \
    }                                                                                  \
    template<>                                                                         \
    void EmitIR<IR::Opcode::A64ExclusiveWriteMemory##bits>(EmitContext & ctx, IR::Inst * inst) { \
        EmitExclusiveWrite(ctx, inst, size);                                           \
    }

A64_MEMORY_OPS(8, B8)
A64_MEMORY_OPS(16, B16)
A64_MEMORY_OPS(32, B32)
A64_MEMORY_OPS(64, B64)
A64_MEMORY_OPS(128, B128)

#undef A64_MEMORY_OPS

}  // namespace Dynarmic::Backend::Wasm
