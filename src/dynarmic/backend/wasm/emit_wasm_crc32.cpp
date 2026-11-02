// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// ARMv8 CRC32 instructions (CRC32{B,H,W,X} and CRC32C{B,H,W,X}), table
// driven. The lookup tables live in the main module's linear memory at a
// fixed address, baked into the generated code: 8/16-bit data use one
// byte-at-a-time table, 32/64-bit data use slicing-by-4 (four independent
// loads per 32-bit word).

#include <array>

#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/emit_wasm.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

namespace Dynarmic::Backend::Wasm {

namespace {

constexpr u32 kPolyCastagnoli = 0x82F63B78;  // CRC32C (poly 0x1EDC6F41, reflected)
constexpr u32 kPolyISO = 0xEDB88320;         // CRC32 (poly 0x04C11DB7, reflected)

// Slicing-by-4 tables for a reflected polynomial: table[0] is the classic
// byte table, table[k][i] = (table[k-1][i] >> 8) ^ table[0][table[k-1][i] & 0xFF].
using CrcTables = std::array<std::array<u32, 256>, 4>;

CrcTables MakeTables(u32 poly) {
    CrcTables t{};
    for (u32 i = 0; i < 256; i++) {
        u32 crc = i;
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
        }
        t[0][i] = crc;
    }
    for (size_t k = 1; k < 4; k++) {
        for (u32 i = 0; i < 256; i++) {
            t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFF];
        }
    }
    return t;
}

u32 TablesAddress(u32 poly) {
    static const CrcTables castagnoli = MakeTables(kPolyCastagnoli);
    static const CrcTables iso = MakeTables(kPolyISO);
    return static_cast<u32>(reinterpret_cast<uptr>((poly == kPolyCastagnoli ? castagnoli : iso).data()));
}

// Pushes tables[k][(crc >> shift) & 0xFF].
void EmitLookup(EmitContext& ctx, u32 tables, u32 k, u32 tc, u32 shift) {
    ctx.code.LocalGet(tc);
    if (shift != 0) {
        ctx.code.I32Const(static_cast<s32>(shift));
        ctx.code.U8(Op::I32ShrU);
    }
    if (shift != 24) {
        ctx.code.I32Const(0xFF);
        ctx.code.U8(Op::I32And);
    }
    ctx.code.I32Const(2);
    ctx.code.U8(Op::I32Shl);
    ctx.code.Load(Op::I32Load, tables + k * 1024, 2);
}

// crc = T[crc & 0xFF] ^ (crc >> 8), `bytes` times.
void EmitBytewise(EmitContext& ctx, u32 tables, u32 tc, int bytes) {
    for (int i = 0; i < bytes; i++) {
        EmitLookup(ctx, tables, 0, tc, 0);
        ctx.code.LocalGet(tc);
        ctx.code.I32Const(8);
        ctx.code.U8(Op::I32ShrU);
        ctx.code.U8(Op::I32Xor);
        ctx.code.LocalSet(tc);
    }
}

// One slicing-by-4 step on a crc already XORed with a 32-bit data word.
void EmitWord(EmitContext& ctx, u32 tables, u32 tc) {
    EmitLookup(ctx, tables, 3, tc, 0);
    EmitLookup(ctx, tables, 2, tc, 8);
    ctx.code.U8(Op::I32Xor);
    EmitLookup(ctx, tables, 1, tc, 16);
    ctx.code.U8(Op::I32Xor);
    EmitLookup(ctx, tables, 0, tc, 24);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tc);
}

// crc = acc ^ (data & mask), then one step per data byte.
void EmitCRC32(EmitContext& ctx, IR::Inst* inst, u32 poly, u32 data_mask, int bytes) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 tables = TablesAddress(poly);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tc = ctx.Temp(ValType::I32);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);  // accumulator
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // data
    if (data_mask != 0xFFFFFFFFu) {
        ctx.code.I32Const(static_cast<s32>(data_mask));
        ctx.code.U8(Op::I32And);
    }
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tc);
    if (bytes == 4) {
        EmitWord(ctx, tables, tc);
    } else {
        EmitBytewise(ctx, tables, tc, bytes);
    }
    ctx.code.LocalGet(tc);
    ctx.code.LocalSet(dst);
}

// 64-bit data: the low word, then the high word.
void EmitCRC32Wide(EmitContext& ctx, IR::Inst* inst, u32 poly) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 tables = TablesAddress(poly);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    const u32 tc = ctx.Temp(ValType::I32);
    const u32 td = ctx.Temp(ValType::I64);

    EmitPush(ctx.code, ctx.reg_alloc, args[1]);  // data (U64)
    ctx.code.LocalSet(td);

    EmitPush(ctx.code, ctx.reg_alloc, args[0]);  // accumulator
    ctx.code.LocalGet(td);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tc);
    EmitWord(ctx, tables, tc);

    ctx.code.LocalGet(tc);
    ctx.code.LocalGet(td);
    ctx.code.I64Const(32);
    ctx.code.U8(Op::I64ShrU);
    ctx.code.U8(Op::I32WrapI64);
    ctx.code.U8(Op::I32Xor);
    ctx.code.LocalSet(tc);
    EmitWord(ctx, tables, tc);

    ctx.code.LocalGet(tc);
    ctx.code.LocalSet(dst);
}

}  // namespace

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli8>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32(ctx, inst, kPolyCastagnoli, 0xFF, 1);
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli16>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32(ctx, inst, kPolyCastagnoli, 0xFFFF, 2);
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli32>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32(ctx, inst, kPolyCastagnoli, 0xFFFFFFFFu, 4);
}

template<>
void EmitIR<IR::Opcode::CRC32Castagnoli64>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32Wide(ctx, inst, kPolyCastagnoli);
}

template<>
void EmitIR<IR::Opcode::CRC32ISO8>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32(ctx, inst, kPolyISO, 0xFF, 1);
}

template<>
void EmitIR<IR::Opcode::CRC32ISO16>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32(ctx, inst, kPolyISO, 0xFFFF, 2);
}

template<>
void EmitIR<IR::Opcode::CRC32ISO32>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32(ctx, inst, kPolyISO, 0xFFFFFFFFu, 4);
}

template<>
void EmitIR<IR::Opcode::CRC32ISO64>(EmitContext& ctx, IR::Inst* inst) {
    EmitCRC32Wide(ctx, inst, kPolyISO);
}

}  // namespace Dynarmic::Backend::Wasm
