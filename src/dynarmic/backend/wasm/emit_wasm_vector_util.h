// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Shared emission helpers for the WASM backend vector emitter tranches
// (emit_wasm_vector*.cpp): v128 constants, lane splats, shuffle helpers.

#pragma once

#include <array>

#include <mcl/stdint.hpp>

#include "dynarmic/backend/wasm/emit_context.h"
#include "dynarmic/backend/wasm/reg_alloc.h"
#include "dynarmic/backend/wasm/wasm_binary.h"
#include "dynarmic/ir/microinstruction.h"

namespace Dynarmic::Backend::Wasm {

inline void VConst(EmitContext& ctx, const std::array<u8, 16>& bytes) {
    ctx.code.Simd(Op::V128Const);
    ctx.code.Bytes(bytes.data(), bytes.size());
}

inline void VZero(EmitContext& ctx) {
    VConst(ctx, {});
}

// Splat a per-lane constant (lane width esize bits).
inline void VLaneConst(EmitContext& ctx, u32 esize, u64 value) {
    std::array<u8, 16> bytes{};
    const size_t lane_bytes = esize / 8;
    for (size_t i = 0; i < 16; i += lane_bytes) {
        for (size_t b = 0; b < lane_bytes; b++) {
            bytes[i + b] = static_cast<u8>(value >> (8 * b));
        }
    }
    VConst(ctx, bytes);
}

inline constexpr std::array<u8, 16> kLow64Mask = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0};

// Binary vector op: dst = subop(args[0], args[1]).
inline void EmitVBinOp(EmitContext& ctx, IR::Inst* inst, u32 subop) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(subop);
    ctx.code.LocalSet(dst);
}

// Push args[0], args[1], shuffle with pattern, store into inst's local.
inline void EmitShuffle2(EmitContext& ctx, IR::Inst* inst, const std::array<u8, 16>& pattern) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
    ctx.code.LocalSet(dst);
}

// EmitShuffle2, then AND with a mask (kills garbage lanes).
inline void EmitShuffle2Masked(EmitContext& ctx, IR::Inst* inst, const std::array<u8, 16>& pattern, const std::array<u8, 16>& mask) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[1]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
    VConst(ctx, mask);
    ctx.code.Simd(Op::V128And);
    ctx.code.LocalSet(dst);
}

// Unary shuffle: both operands are args[0].
inline void EmitShuffle1(EmitContext& ctx, IR::Inst* inst, const std::array<u8, 16>& pattern) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const u32 dst = ctx.reg_alloc.DefineLocal(ctx.code, inst);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    EmitPush(ctx.code, ctx.reg_alloc, args[0]);
    ctx.code.Simd(Op::I8x16Shuffle);
    ctx.code.Bytes(pattern.data(), pattern.size());
    ctx.code.LocalSet(dst);
}

// Byte indices for lane `lane` of width lane_bytes, from operand `vec` (0 or 1).
inline void SetLane(std::array<u8, 16>& pattern, u32 out_byte, u32 vec, u32 lane, u32 lane_bytes) {
    const u32 base = vec * 16 + lane * lane_bytes;
    for (u32 b = 0; b < lane_bytes; b++) {
        pattern[out_byte + b] = static_cast<u8>(base + b);
    }
}

// Sets FPSCR.QC (bit 27 of the state's fpsr word) if any bit of the v128 local
// `mask_local` is set.
void EmitSetQcIfAny(EmitContext& ctx, u32 mask_local);

// Saturation-flag (FPSCR.QC) detection for an already-emitted saturating
// vector instruction, recomputed from its operands and result:
//   Add/Sub     : saturated result differs from the wrapping result
//   Abs/Neg     : same, against wrapping abs/neg
//   NarrowSigned / NarrowUnsigned: sign/zero-extending the narrowed low half
//                 back to the source width differs from the source
enum class QcKind { Add, Sub, Abs, Neg, NarrowSigned, NarrowUnsigned };
void EmitQcCheck(EmitContext& ctx, IR::Inst* inst, QcKind kind, u32 esize);

}  // namespace Dynarmic::Backend::Wasm
