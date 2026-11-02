// This file is part of Dynarmic.
// Licensed under the MIT License. See LICENSE in the project root.
//
// Host-callback thunks imported by generated WASM block modules.
// They devirtualize A32::UserCallbacks using the calling JIT execution context.

#include "dynarmic/backend/wasm/wasm_runtime.h"

#include <cmath>
#include <cstring>

#include <mcl/assert.hpp>
#include <mcl/bit/bit_field.hpp>

#include "dynarmic/backend/wasm/a32_jitstate.h"
#include "dynarmic/backend/wasm/abi.h"
#include "dynarmic/common/crypto/aes.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op.h"
#include "dynarmic/common/math_util.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Wasm {
namespace {

const A32::UserConfig* ConfigOf(uptr ectx) {
    return static_cast<const A32::UserConfig*>(reinterpret_cast<const ExecutionContext*>(ectx)->callback_config);
}

A32JitState* StateOf(uptr state) {
    return reinterpret_cast<A32JitState*>(state);
}

ExecutionContext* EctxOf(uptr ectx) {
    return reinterpret_cast<ExecutionContext*>(ectx);
}

// Halt bits pending after a callback ran (blocks return these to the
// dispatcher, which stops the run loop).
u32 HaltBits(uptr ectx) {
    return EctxOf(ectx)->halt_reason;
}

// FP reference thunks receive the address of the guest FPSR word (A32 fpsr /
// A64 fpsr, see EmitPushFpsrAddr) and fold the exception bits they raise into
// it (the low FPSR bits: IDC/IXC/UFC/OFC/DZC/IOC).
void FoldFpsr(uptr fpsr_addr, FP::FPSR fpsr) {
    *reinterpret_cast<u32*>(fpsr_addr) |= fpsr.Value() & 0x9F;
}

// Half-precision conversions go through the reference FP::FPConvert
// implementation.
template<typename To, typename From>
To FpConvertThunk(uptr fpsr_addr, u64 value, u32 fpcr, u32 rounding) {
    FP::FPSR fpsr{};
    const To result = FP::FPConvert<To, From>(static_cast<From>(value), FP::FPCR{fpcr},
                                              static_cast<FP::RoundingMode>(rounding), fpsr);
    FoldFpsr(fpsr_addr, fpsr);
    return result;
}

}  // anonymous namespace

namespace {
bool g_debug_function_names = false;
}  // anonymous namespace

bool DebugFunctionNames() {
    return g_debug_function_names;
}


}  // namespace Dynarmic::Backend::Wasm

using namespace Dynarmic;
using namespace Dynarmic::Backend::Wasm;

namespace {

u32 Ror32(u32 x, int n) {
    return (x >> n) | (x << (32 - n));
}
u32 ShaChoose(u32 x, u32 y, u32 z) {
    return (x & y) ^ (~x & z);
}
u32 ShaMajority(u32 x, u32 y, u32 z) {
    return (x & y) | ((x | y) & z);
}
u32 ShaSigma0(u32 x) {
    return Ror32(x, 2) ^ Ror32(x, 13) ^ Ror32(x, 22);
}
u32 ShaSigma1(u32 x) {
    return Ror32(x, 6) ^ Ror32(x, 11) ^ Ror32(x, 25);
}
u32 ShaLowSigma0(u32 x) {
    return Ror32(x, 7) ^ Ror32(x, 18) ^ (x >> 3);
}
u32 ShaLowSigma1(u32 x) {
    return Ror32(x, 17) ^ Ror32(x, 19) ^ (x >> 10);
}

// ARM ARM SHA256hash(X, Y, W, part1); X = {a,b,c,d}, Y = {e,f,g,h} (lane 0
// first), W = four wk words. Returns the new X (part1) or Y (part2).
void Sha256Hash(const u32* x_in, const u32* y_in, const u32* w, bool part1, u32* out) {
    u32 x[4] = {x_in[0], x_in[1], x_in[2], x_in[3]};
    u32 y[4] = {y_in[0], y_in[1], y_in[2], y_in[3]};
    for (int e = 0; e < 4; e++) {
        const u32 chs = ShaChoose(y[0], y[1], y[2]);
        const u32 maj = ShaMajority(x[0], x[1], x[2]);
        const u32 t = y[3] + ShaSigma1(y[0]) + chs + w[e];
        x[3] = t + x[3];
        y[3] = t + ShaSigma0(x[0]) + maj;
        // <Y, X> = ROL(Y : X, 32): the top word of Y moves to the bottom of X.
        const u32 new_x[4] = {y[3], x[0], x[1], x[2]};
        const u32 new_y[4] = {x[3], y[0], y[1], y[2]};
        for (int i = 0; i < 4; i++) {
            x[i] = new_x[i];
            y[i] = new_y[i];
        }
    }
    const u32* r = part1 ? x : y;
    for (int i = 0; i < 4; i++)
        out[i] = r[i];
}

void Sha256MessageSchedule0(const u32* x, const u32* y, u32* out) {
    const u32 t[4] = {x[1], x[2], x[3], y[0]};
    for (int i = 0; i < 4; i++)
        out[i] = x[i] + ShaLowSigma0(t[i]);
}

void Sha256MessageSchedule1(const u32* x, const u32* y, const u32* z, u32* out) {
    const u32 t0[4] = {y[1], y[2], y[3], z[0]};
    out[0] = x[0] + ShaLowSigma1(z[2]) + t0[0];
    out[1] = x[1] + ShaLowSigma1(z[3]) + t0[1];
    out[2] = x[2] + ShaLowSigma1(out[0]) + t0[2];
    out[3] = x[3] + ShaLowSigma1(out[1]) + t0[3];
}

}  // anonymous namespace

namespace {

template<typename FPT>
u64 FpRef(FpRefOp op, u32 imm, FP::FPCR fpcr, FP::FPSR& fpsr, u64 a, u64 b, u64 c) {
    const FPT x = static_cast<FPT>(a);
    const FPT y = static_cast<FPT>(b);
    const FPT z = static_cast<FPT>(c);
    switch (op) {
    case FpRefOp::MulAdd:
        return FP::FPMulAdd<FPT>(x, y, z, fpcr, fpsr);
    case FpRefOp::MulSub:
        return FP::FPMulSub<FPT>(x, y, z, fpcr, fpsr);
    case FpRefOp::RecipEstimate:
        return FP::FPRecipEstimate<FPT>(x, fpcr, fpsr);
    case FpRefOp::RSqrtEstimate:
        return FP::FPRSqrtEstimate<FPT>(x, fpcr, fpsr);
    case FpRefOp::RecipExponent:
        return FP::FPRecipExponent<FPT>(x, fpcr, fpsr);
    case FpRefOp::RecipStepFused:
        return FP::FPRecipStepFused<FPT>(x, y, fpcr, fpsr);
    case FpRefOp::RSqrtStepFused:
        return FP::FPRSqrtStepFused<FPT>(x, y, fpcr, fpsr);
    case FpRefOp::RoundInt:
        return static_cast<FPT>(FP::FPRoundInt<FPT>(x, fpcr, static_cast<FP::RoundingMode>(imm & 0xFF), (imm >> 8) & 1, fpsr));
    case FpRefOp::ToFixed:
        return FP::FPToFixed<FPT>((imm >> 16) & 0xFF, x, imm & 0xFF, (imm >> 24) & 1, fpcr,
                                  static_cast<FP::RoundingMode>((imm >> 8) & 0xFF), fpsr);
    case FpRefOp::CompareEqual:
        return FP::FPCompareEQ<FPT>(x, y, fpcr, fpsr) ? static_cast<FPT>(~FPT{0}) : FPT{0};
    }
    UNREACHABLE();
}

}  // anonymous namespace

extern "C" {

// AES / SHA256 helpers: operands staged in the state's crypto scratch area,
// whose address is passed as the first argument (see abi.h).
void dynarmic_wasm_crypto(uptr scratch, uptr, u32 op) {
    u32* s = reinterpret_cast<u32*>(scratch);
    u32* out = s + 12;
    auto aes = [&](void (*fn)(Common::Crypto::AES::State&, const Common::Crypto::AES::State&)) {
        Common::Crypto::AES::State in, res;
        std::memcpy(in.data(), s, 16);
        fn(res, in);
        std::memcpy(out, res.data(), 16);
    };
    switch (static_cast<CryptoOp>(op)) {
    case CryptoOp::AesEncryptRound:
        aes(Common::Crypto::AES::EncryptSingleRound);
        break;
    case CryptoOp::AesDecryptRound:
        aes(Common::Crypto::AES::DecryptSingleRound);
        break;
    case CryptoOp::AesMixColumns:
        aes(Common::Crypto::AES::MixColumns);
        break;
    case CryptoOp::AesInverseMixColumns:
        aes(Common::Crypto::AES::InverseMixColumns);
        break;
    case CryptoOp::Sha256HashPart1:
        Sha256Hash(s, s + 4, s + 8, true, out);
        break;
    case CryptoOp::Sha256HashPart2:
        Sha256Hash(s, s + 4, s + 8, false, out);
        break;
    case CryptoOp::Sha256MessageSchedule0:
        Sha256MessageSchedule0(s, s + 4, out);
        break;
    case CryptoOp::Sha256MessageSchedule1:
        Sha256MessageSchedule1(s, s + 4, s + 8, out);
        break;
    }
}


u32 dynarmic_wasm_read8(uptr, uptr ectx, u32 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead8(vaddr);
}

u32 dynarmic_wasm_read16(uptr, uptr ectx, u32 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead16(vaddr);
}

u32 dynarmic_wasm_read32(uptr, uptr ectx, u32 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead32(vaddr);
}

u64 dynarmic_wasm_read64(uptr, uptr ectx, u32 vaddr) {
    return ConfigOf(ectx)->callbacks->MemoryRead64(vaddr);
}

void dynarmic_wasm_write8(uptr, uptr ectx, u32 vaddr, u32 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite8(vaddr, static_cast<u8>(value));
}

void dynarmic_wasm_write16(uptr, uptr ectx, u32 vaddr, u32 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite16(vaddr, static_cast<u16>(value));
}

void dynarmic_wasm_write32(uptr, uptr ectx, u32 vaddr, u32 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite32(vaddr, value);
}

void dynarmic_wasm_write64(uptr, uptr ectx, u32 vaddr, u64 value) {
    ConfigOf(ectx)->callbacks->MemoryWrite64(vaddr, value);
}

u32 dynarmic_wasm_xread8(uptr state, uptr ectx, u32 vaddr) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    StateOf(state)->exclusive_state = 1;
    return ConfigOf(ectx)->global_monitor->ReadAndMark<u8>(ConfigOf(ectx)->processor_id, vaddr, [&] {
        return ConfigOf(ectx)->callbacks->MemoryRead8(vaddr);
    });
}

u32 dynarmic_wasm_xread16(uptr state, uptr ectx, u32 vaddr) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    StateOf(state)->exclusive_state = 1;
    return ConfigOf(ectx)->global_monitor->ReadAndMark<u16>(ConfigOf(ectx)->processor_id, vaddr, [&] {
        return ConfigOf(ectx)->callbacks->MemoryRead16(vaddr);
    });
}

u32 dynarmic_wasm_xread32(uptr state, uptr ectx, u32 vaddr) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    StateOf(state)->exclusive_state = 1;
    return ConfigOf(ectx)->global_monitor->ReadAndMark<u32>(ConfigOf(ectx)->processor_id, vaddr, [&] {
        return ConfigOf(ectx)->callbacks->MemoryRead32(vaddr);
    });
}

u64 dynarmic_wasm_xread64(uptr state, uptr ectx, u32 vaddr) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    StateOf(state)->exclusive_state = 1;
    return ConfigOf(ectx)->global_monitor->ReadAndMark<u64>(ConfigOf(ectx)->processor_id, vaddr, [&] {
        return ConfigOf(ectx)->callbacks->MemoryRead64(vaddr);
    });
}

u32 dynarmic_wasm_xwrite8(uptr state, uptr ectx, u32 vaddr, u32 value) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    A32JitState* jit_state = StateOf(state);
    if (!jit_state->exclusive_state) {
        return 1;
    }
    jit_state->exclusive_state = 0;
    return ConfigOf(ectx)->global_monitor->DoExclusiveOperation<u8>(ConfigOf(ectx)->processor_id, vaddr, [&](u8 expected) {
               return ConfigOf(ectx)->callbacks->MemoryWriteExclusive8(vaddr, static_cast<u8>(value), expected);
           })
                   ? 0
                   : 1;
}

u32 dynarmic_wasm_xwrite16(uptr state, uptr ectx, u32 vaddr, u32 value) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    A32JitState* jit_state = StateOf(state);
    if (!jit_state->exclusive_state) {
        return 1;
    }
    jit_state->exclusive_state = 0;
    return ConfigOf(ectx)->global_monitor->DoExclusiveOperation<u16>(ConfigOf(ectx)->processor_id, vaddr, [&](u16 expected) {
               return ConfigOf(ectx)->callbacks->MemoryWriteExclusive16(vaddr, static_cast<u16>(value), expected);
           })
                   ? 0
                   : 1;
}

u32 dynarmic_wasm_xwrite32(uptr state, uptr ectx, u32 vaddr, u32 value) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    A32JitState* jit_state = StateOf(state);
    if (!jit_state->exclusive_state) {
        return 1;
    }
    jit_state->exclusive_state = 0;
    return ConfigOf(ectx)->global_monitor->DoExclusiveOperation<u32>(ConfigOf(ectx)->processor_id, vaddr, [&](u32 expected) {
               return ConfigOf(ectx)->callbacks->MemoryWriteExclusive32(vaddr, value, expected);
           })
                   ? 0
                   : 1;
}

u32 dynarmic_wasm_xwrite64(uptr state, uptr ectx, u32 vaddr, u64 value) {
    ASSERT(ConfigOf(ectx)->global_monitor != nullptr);
    A32JitState* jit_state = StateOf(state);
    if (!jit_state->exclusive_state) {
        return 1;
    }
    jit_state->exclusive_state = 0;
    return ConfigOf(ectx)->global_monitor->DoExclusiveOperation<u64>(ConfigOf(ectx)->processor_id, vaddr, [&](u64 expected) {
               return ConfigOf(ectx)->callbacks->MemoryWriteExclusive64(vaddr, value, expected);
           })
                   ? 0
                   : 1;
}

void dynarmic_wasm_xclear(uptr state, uptr) {
    StateOf(state)->exclusive_state = 0;
}

void dynarmic_wasm_svc(uptr, uptr ectx, u32 swi) {
    ConfigOf(ectx)->callbacks->CallSVC(swi);
}

void dynarmic_wasm_raise(uptr, uptr ectx, u32 pc, u64 exception) {
    ConfigOf(ectx)->callbacks->ExceptionRaised(pc, static_cast<A32::Exception>(exception));
}

u32 dynarmic_wasm_interp(uptr, uptr ectx, u32 next_low, u32, u32 num_instructions) {
    ConfigOf(ectx)->callbacks->InterpreterFallback(next_low, num_instructions);
    return HaltBits(ectx);
}

void dynarmic_wasm_dsb(uptr, uptr) {
    // Data synchronization barrier: no observable effect for a
    // single-threaded WASM host beyond ordering the callback stream,
    // which WASM call ordering already guarantees.
}

void dynarmic_wasm_icache(uptr, uptr) {
    // The dispatcher invalidates compiled blocks through the Jit interface;
    // nothing to do per-block here.
}

u64 dynarmic_wasm_coproc_cb(uptr, uptr, u32 fn, u32 user_arg, u32 arg0, u32 arg1) {
    using Fn = u64 (*)(void*, u32, u32);
    const auto callback = reinterpret_cast<Fn>(static_cast<uptr>(fn));
    return callback(reinterpret_cast<void*>(static_cast<uptr>(user_arg)), arg0, arg1);
}

u32 dynarmic_wasm_fp_h2s(uptr fpsr_addr, uptr, u32 value, u32 fpcr, u32 rounding) {
    return FpConvertThunk<u32, u16>(fpsr_addr, value, fpcr, rounding);
}

u32 dynarmic_wasm_fp_s2h(uptr fpsr_addr, uptr, u32 value, u32 fpcr, u32 rounding) {
    return FpConvertThunk<u16, u32>(fpsr_addr, value, fpcr, rounding);
}

u64 dynarmic_wasm_fp_h2d(uptr fpsr_addr, uptr, u32 value, u32 fpcr, u32 rounding) {
    return FpConvertThunk<u64, u16>(fpsr_addr, value, fpcr, rounding);
}

u32 dynarmic_wasm_fp_d2h(uptr fpsr_addr, uptr, u64 value, u32 fpcr, u32 rounding) {
    return FpConvertThunk<u16, u64>(fpsr_addr, value, fpcr, rounding);
}

u32 dynarmic_wasm_recip_est(uptr, uptr, u32 value) {
    if ((value & 0x80000000) == 0) {
        return 0xFFFFFFFF;
    }
    const u32 input = mcl::bit::get_bits<23, 31>(value);
    return (u32{0b100000000} | Common::RecipEstimate(input)) << 23;
}

u32 dynarmic_wasm_rsqrt_est(uptr, uptr, u32 value) {
    if ((value & 0xC0000000) == 0) {
        return 0xFFFFFFFF;
    }
    const u32 input = mcl::bit::get_bits<23, 31>(value);
    return (u32{0b100000000} | Common::RecipSqrtEstimate(input)) << 23;
}

// NEON f32 reciprocal ops go through the reference FP:: implementations per
// lane.
u32 dynarmic_wasm_fp_vrecpe32(uptr fpsr_addr, uptr, u32 value, u32 fpcr) {
    FP::FPSR fpsr{};
    const u32 result = FP::FPRecipEstimate<u32>(value, FP::FPCR{fpcr}, fpsr);
    FoldFpsr(fpsr_addr, fpsr);
    return result;
}

u32 dynarmic_wasm_fp_vrsqrte32(uptr fpsr_addr, uptr, u32 value, u32 fpcr) {
    FP::FPSR fpsr{};
    const u32 result = FP::FPRSqrtEstimate<u32>(value, FP::FPCR{fpcr}, fpsr);
    FoldFpsr(fpsr_addr, fpsr);
    return result;
}

u32 dynarmic_wasm_fp_vrecps32(uptr fpsr_addr, uptr, u32 a, u32 b, u32 fpcr) {
    FP::FPSR fpsr{};
    const u32 result = FP::FPRecipStepFused<u32>(a, b, FP::FPCR{fpcr}, fpsr);
    FoldFpsr(fpsr_addr, fpsr);
    return result;
}

u32 dynarmic_wasm_fp_vrsqrts32(uptr fpsr_addr, uptr, u32 a, u32 b, u32 fpcr) {
    FP::FPSR fpsr{};
    const u32 result = FP::FPRSqrtStepFused<u32>(a, b, FP::FPCR{fpcr}, fpsr);
    FoldFpsr(fpsr_addr, fpsr);
    return result;
}


// Debug aid, callable from JS as Module._dynarmic_wasm_debug_names(1): name
// every block module emitted from now on after its guest location.
void dynarmic_wasm_debug_names(u32 enable) {
    g_debug_function_names = enable != 0;
}

u64 dynarmic_wasm_fp_ref(uptr fpsr_addr, uptr, u32 op_size, u32 imm, u32 fpcr, u64 a, u64 b, u64 c) {
    const auto op = static_cast<FpRefOp>(op_size & 0xFF);
    FP::FPSR fpsr{};
    u64 result = 0;
    switch (op_size >> 8) {
    case 16:
        result = FpRef<u16>(op, imm, FP::FPCR{fpcr}, fpsr, a, b, c);
        break;
    case 32:
        result = FpRef<u32>(op, imm, FP::FPCR{fpcr}, fpsr, a, b, c);
        break;
    case 64:
        result = FpRef<u64>(op, imm, FP::FPCR{fpcr}, fpsr, a, b, c);
        break;
    default:
        UNREACHABLE();
    }
    FoldFpsr(fpsr_addr, fpsr);
    return result;
}

double dynarmic_wasm_fp_fma64(double a, double b, double c) {
    return std::fma(a, b, c);
}

}  // extern "C"
