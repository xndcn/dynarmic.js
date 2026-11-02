// Host reference for neon4.c. Plain float ops run through SSE at runtime (the
// "+x" barrier in fval defeats constant folding, whose NaN choices differ from
// SSE/V8); reciprocal/half conversions call dynarmic's FP:: reference code
// exactly like the wasm thunks do. Must print the same checksum as the guest.
//
// ARMv7 NEON arithmetic runs under the standard FPSCR: flush-to-zero (inputs
// and results; ftz below) and default NaN (FP:: calls get kStandardFpcr). NaNs
// propagated by plain ops keep SSE's payload rule, a known deviation.

#include <cmath>
#include <cstdint>
#include <cstdio>

static FILE* g_log = nullptr;

#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/common/fp/op.h"

using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

static u32 g_acc = 2166136261u;
static void mix(u32 v) {
    if (g_log) std::fprintf(g_log, "%08x\n", v);
    g_acc = (g_acc ^ v) * 16777619u;
}

static u32 g_fpsr_exc = 0;
#define FPSCR_MIX() mix(g_fpsr_exc)

static float fval(u32 v) {
    union {
        u32 u;
        float f;
    } x;
    x.u = v;
    __asm__ volatile("" : "+x"(x.f));
    return x.f;
}

static constexpr u32 kStandardFpcr = 0x03000000;  // DN | FZ, round to nearest

// Denormal -> signed zero.
static u32 ftz(u32 v) {
    return (v & 0x7F800000u) == 0 ? v & 0x80000000u : v;
}

static u32 bits(float f) {
    union {
        u32 u;
        float f;
    } x;
    x.f = f;
    return x.u;
}

// SSE two-operand NaN rule (what V8 lowers f32x4 arithmetic to): a NaN src2
// wins over a NaN src1, signaling NaNs are quieted (bit 22 set). The C
// compiler may swap operands of commutative float ops, so NaN propagation is
// resolved explicitly and only non-NaN cases reach the hardware op.
static u32 arith2(u32 xb, u32 yb, int op) {
    xb = ftz(xb);
    yb = ftz(yb);
    const float x = fval(xb), y = fval(yb);
    if (std::isnan(y))
        return yb | 0x00400000u;
    if (std::isnan(x))
        return xb | 0x00400000u;
    switch (op) {
    case 0:
        return ftz(bits(x + y));
    case 1:
        return ftz(bits(x - y));
    default:
        return ftz(bits(x * y));
    }
}

static void op_vadd_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = arith2(a[i], b[i], 0);
}
static void op_vsub_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = arith2(a[i], b[i], 1);
}
static void op_vmul_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = arith2(a[i], b[i], 2);
}
static void op_vabd_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = arith2(a[i], b[i], 1) & 0x7FFFFFFFu;
}

static void op_vabs_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = a[i] & 0x7FFFFFFFu;
}
static void op_vneg_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = a[i] ^ 0x80000000u;
}

static void op_vceq_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fval(ftz(a[i])) == fval(ftz(b[i])) ? 0xFFFFFFFFu : 0u;
}
static void op_vcgt_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fval(ftz(a[i])) > fval(ftz(b[i])) ? 0xFFFFFFFFu : 0u;
}
static void op_vcge_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fval(ftz(a[i])) >= fval(ftz(b[i])) ? 0xFFFFFFFFu : 0u;
}
static void op_vcage_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = std::fabs(fval(ftz(a[i]))) >= std::fabs(fval(ftz(b[i]))) ? 0xFFFFFFFFu : 0u;
}
static void op_vcagt_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = std::fabs(fval(ftz(a[i]))) > std::fabs(fval(ftz(b[i]))) ? 0xFFFFFFFFu : 0u;
}

// wasm f32.min/f32.max semantics (IEEE minimum/maximum with signed-zero rule);
// the guest only feeds these NaN-free pairs, so the NaN branch is unreachable.
static float wmin(float x, float y) {
    if (x < y)
        return x;
    if (y < x)
        return y;
    if (x == y)
        return std::signbit(x) ? x : y;
    return x;  // NaN: value irrelevant, never observed
}
static float wmax(float x, float y) {
    if (x > y)
        return x;
    if (y > x)
        return y;
    if (x == y)
        return std::signbit(x) ? y : x;
    return x;
}
static void op_vmin_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = ftz(bits(wmin(fval(ftz(a[i])), fval(ftz(b[i])))));
}
static void op_vmax_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = ftz(bits(wmax(fval(ftz(a[i])), fval(ftz(b[i])))));
}

// vpadd.f32 d4, d0, d2: d4 = {a0+a1, b0+b1} (d0/d2 are the low halves of q0/q1).
static void op_vpadd_f32(const u32* a, const u32* b, u32* r) {
    r[0] = arith2(a[0], a[1], 0);
    r[1] = arith2(b[0], b[1], 0);
}

// float -> fixed/int: scale, truncate, saturate like i32.trunc_sat_f32_s/u.
static u32 to_fixed_s(float x, int fbits) {
    float v = std::trunc(x * std::ldexp(1.0f, fbits));
    if (std::isnan(v))
        return 0;
    if (v >= 2147483648.0f)
        return 0x7FFFFFFFu;
    if (v < -2147483648.0f)
        return 0x80000000u;
    return static_cast<u32>(static_cast<s32>(v));
}
static u32 to_fixed_u(float x, int fbits) {
    float v = std::trunc(x * std::ldexp(1.0f, fbits));
    if (std::isnan(v))
        return 0;
    if (v >= 4294967296.0f)
        return 0xFFFFFFFFu;
    if (v < 0.0f)
        return 0u;
    return static_cast<u32>(v);
}
#define DEF_TOFIX(nm, fbits, is_signed)                          \
    static void op_##nm(const u32* a, const u32*, u32* r) {      \
        for (int i = 0; i < 4; i++)                              \
            r[i] = is_signed ? to_fixed_s(fval(ftz(a[i])), fbits) \
                             : to_fixed_u(fval(ftz(a[i])), fbits); \
    }
DEF_TOFIX(vcvt_s32_f32_3, 3, true)
DEF_TOFIX(vcvt_u32_f32_7, 7, false)
DEF_TOFIX(vcvt_s32_f32, 0, true)
DEF_TOFIX(vcvt_u32_f32, 0, false)

// int/fixed -> float: convert RNE (SSE cvtdq2ps), scale by 2^-fbits.
#define DEF_FROMFIX(nm, fbits, is_signed)                        \
    static void op_##nm(const u32* a, const u32*, u32* r) {      \
        const float scale = std::ldexp(1.0f, -(fbits));          \
        for (int i = 0; i < 4; i++) {                            \
            const float f = is_signed ? static_cast<float>(static_cast<s32>(a[i])) \
                                      : static_cast<float>(a[i]); \
            r[i] = ftz(bits(f * scale));                         \
        }                                                        \
    }
DEF_FROMFIX(vcvt_f32_s32_5, 5, true)
DEF_FROMFIX(vcvt_f32_u32_11, 11, false)
DEF_FROMFIX(vcvt_f32_s32, 0, true)
DEF_FROMFIX(vcvt_f32_u32, 0, false)

// Reciprocal estimate/step ops: same FP:: reference code the wasm thunks call,
// same (standard) FPCR, same FPSR folding.
static u32 fp_vrecpe(u32 v) {
    Dynarmic::FP::FPSR fpsr{};
    const u32 r = Dynarmic::FP::FPRecipEstimate<u32>(v, Dynarmic::FP::FPCR{kStandardFpcr}, fpsr);
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    return r;
}
static u32 fp_vrsqrte(u32 v) {
    Dynarmic::FP::FPSR fpsr{};
    const u32 r = Dynarmic::FP::FPRSqrtEstimate<u32>(v, Dynarmic::FP::FPCR{kStandardFpcr}, fpsr);
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    return r;
}
static u32 fp_vrecps(u32 a, u32 b) {
    Dynarmic::FP::FPSR fpsr{};
    const u32 r = Dynarmic::FP::FPRecipStepFused<u32>(a, b, Dynarmic::FP::FPCR{kStandardFpcr}, fpsr);
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    return r;
}
static u32 fp_vrsqrts(u32 a, u32 b) {
    Dynarmic::FP::FPSR fpsr{};
    const u32 r = Dynarmic::FP::FPRSqrtStepFused<u32>(a, b, Dynarmic::FP::FPCR{kStandardFpcr}, fpsr);
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    return r;
}
static void op_vrecpe_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fp_vrecpe(a[i]);
}
static void op_vrsqrte_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fp_vrsqrte(a[i]);
}
static void op_vrecps_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fp_vrecps(a[i], b[i]);
}
static void op_vrsqrts_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = fp_vrsqrts(a[i], b[i]);
}

static u32 fp_h2s(u16 v) {
    Dynarmic::FP::FPSR fpsr{};
    const u32 r = Dynarmic::FP::FPConvert<u32, u16>(
        v, Dynarmic::FP::FPCR{kStandardFpcr}, Dynarmic::FP::RoundingMode::ToNearest_TieEven, fpsr);
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    return r;
}
// vcvt.f32.f16 q2, d0: sources are the four u16 halves of a[0..1].
static void op_vcvt_f32_f16(const u32* a, const u32*, u32* r) {
    r[0] = fp_h2s(static_cast<u16>(a[0]));
    r[1] = fp_h2s(static_cast<u16>(a[0] >> 16));
    r[2] = fp_h2s(static_cast<u16>(a[1]));
    r[3] = fp_h2s(static_cast<u16>(a[1] >> 16));
}
// vcvt.f16.f32 d4, q0: four f32 -> f16, packed two per word.
static void op_vcvt_f16_f32(const u32* a, const u32*, u32* r) {
    Dynarmic::FP::FPSR fpsr{};
    u32 h[4];
    for (int i = 0; i < 4; i++) {
        h[i] = Dynarmic::FP::FPConvert<u16, u32>(
            a[i], Dynarmic::FP::FPCR{kStandardFpcr}, Dynarmic::FP::RoundingMode::ToNearest_TieEven, fpsr);
    }
    g_fpsr_exc |= fpsr.Value() & 0x9F;
    r[0] = h[0] | (h[1] << 16);
    r[1] = h[2] | (h[3] << 16);
}

#include "neon4_body.inc"

int main() {
    g_log = std::fopen("ref_mix.log", "w");
    test_body();
    if (g_log)
        std::fclose(g_log);
    std::printf("checksum %08x\n", g_acc);
    return 0;
}
