// Host reference for neon5.c. vfma/vfms model the wasm lowering exactly:
// promote each f32 lane to f64 (cvtss2sd NaN rule), f64 mul then add with the
// SSE two-operand NaN priority (src2 wins, quieted), demote (cvtsd2ss).
// Build with -msse4.1 so rintf/truncf/... lower to roundss like V8 does.
// The vector ops run under ARMv7 NEON's standard FPSCR: denormal inputs and
// results flush to zero (ftz), default NaN. The scalar vminnm/vmaxnm follow
// FPSCR (= 0).

#include <cmath>
#include <cstdint>
#include <cstdio>

static FILE* g_log = nullptr;

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;

static u32 g_acc = 2166136261u;
static void mix(u32 v) {
    if (g_log) std::fprintf(g_log, "%08x\n", v);
    g_acc = (g_acc ^ v) * 16777619u;
}

static float fval(u32 v) {
    union {
        u32 u;
        float f;
    } x;
    x.u = v;
    __asm__ volatile("" : "+x"(x.f));
    return x.f;
}
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
static double dval(u64 v) {
    union {
        u64 u;
        double d;
    } x;
    x.u = v;
    __asm__ volatile("" : "+x"(x.d));
    return x.d;
}
static u64 dbits(double d) {
    union {
        u64 u;
        double d;
    } x;
    x.d = d;
    return x.u;
}

// ---- vfma/vfms (f64x2 promote/mul/add/demote model) ----

static u64 dmul_sse(u64 xb, u64 yb) {
    const double x = dval(xb), y = dval(yb);
    if (std::isnan(y))
        return yb | (1ULL << 51);
    if (std::isnan(x))
        return xb | (1ULL << 51);
    return dbits(x * y);
}
static u64 dadd_sse(u64 xb, u64 yb) {
    const double x = dval(xb), y = dval(yb);
    if (std::isnan(y))
        return yb | (1ULL << 51);
    if (std::isnan(x))
        return xb | (1ULL << 51);
    return dbits(x + y);
}
// cvtss2sd: quiet NaN, payload << 29.
static u64 promote(u32 v) {
    return dbits(static_cast<double>(fval(v)));
}
// cvtsd2ss: quiet NaN, payload = top 22 mantissa bits.
static u32 demote(u64 v) {
    const double d = dval(v);
    if (std::isnan(d))
        return ((u32)(v >> 32) & 0x80000000u) | 0x7FC00000u | (u32)((v >> 29) & 0x3FFFFFu);
    return bits(static_cast<float>(d));
}
static u32 vfma_lane(u32 nb, u32 mb, u32 cb) {
    return ftz(demote(dadd_sse(dmul_sse(promote(ftz(nb)), promote(ftz(mb))), promote(ftz(cb)))));
}
static u32 vfms_lane(u32 nb, u32 mb, u32 cb) {
    return vfma_lane(nb ^ 0x80000000u, mb, cb);
}
static void op_vfma_f32(const u32* n, const u32* m, const u32* c, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = vfma_lane(n[i], m[i], c[i]);
}
static void op_vfms_f32(const u32* n, const u32* m, const u32* c, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = vfms_lane(n[i], m[i], c[i]);
}

// ---- vrint* ----

// TieAway mirroring the emitter's bitselect form.
static float round_tieaway(float x) {
    const float t = std::trunc(x);
    return std::fabs(x - t) >= 0.5f ? t + std::copysign(1.0f, x) : t;
}
static void op_vrintn_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = bits(std::rint(fval(ftz(a[i]))));
}
static void op_vrinta_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = bits(round_tieaway(fval(ftz(a[i]))));
}
static void op_vrintp_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = bits(std::ceil(fval(ftz(a[i]))));
}
static void op_vrintm_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = bits(std::floor(fval(ftz(a[i]))));
}
static void op_vrintz_f32(const u32* a, const u32*, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = bits(std::trunc(fval(ftz(a[i]))));
}

// ---- vminnm/vmaxnm (scalar chain, mirrors the emitter's selects) ----

static float wmin(float x, float y) {
    if (x < y)
        return x;
    if (y < x)
        return y;
    if (x == y)
        return std::signbit(x) ? x : y;
    return x;  // NaN: value never observed
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
static bool is_snan(u32 v) {
    return (v & 0x7FC00000u) == 0x7F800000u && (v & 0x003FFFFFu) != 0;
}
// Only quiet NaNs count as missing; a signaling NaN operand (first one
// first) gives that NaN quieted, or the default NaN when `dn`.
static u32 fp_minmax_num(u32 xb, u32 yb, bool is_max, bool dn) {
    if (is_snan(xb) || is_snan(yb)) {
        return dn ? 0x7FC00000u : (is_snan(xb) ? xb : yb) | 0x00400000u;
    }
    const float x = fval(xb), y = fval(yb);
    const float core = is_max ? wmax(x, y) : wmin(x, y);
    const float r1 = (y == y) ? core : x;
    const float r2 = (x == x) ? r1 : y;
    const float r = ((x == x) || (y == y)) ? r2 : x;
    return bits(r);
}
static void op_vminnm_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = ftz(fp_minmax_num(ftz(a[i]), ftz(b[i]), false, true));
}
static void op_vmaxnm_f32(const u32* a, const u32* b, u32* r) {
    for (int i = 0; i < 4; i++)
        r[i] = ftz(fp_minmax_num(ftz(a[i]), ftz(b[i]), true, true));
}
static void op_vminnm_s(const u32* a, const u32* b, u32* r) {
    *r = fp_minmax_num(*a, *b, false, false);
}
static void op_vmaxnm_s(const u32* a, const u32* b, u32* r) {
    *r = fp_minmax_num(*a, *b, true, false);
}

// ---- vcvtn/a/p/m: round per mode, then trunc_sat-style saturation ----

static float round_mode(float x, int mode) {
    switch (mode) {
    case 0:
        return std::rint(x);  // FPSCR RNE
    case 1:
        return round_tieaway(x);
    case 2:
        return std::ceil(x);
    default:
        return std::floor(x);
    }
}
static u32 to_fixed_rm_s(float x, int mode) {
    const float v = round_mode(x, mode);
    if (std::isnan(v))
        return 0;
    if (v >= 2147483648.0f)
        return 0x7FFFFFFFu;
    if (v < -2147483648.0f)
        return 0x80000000u;
    return static_cast<u32>(static_cast<s32>(v));
}
static u32 to_fixed_rm_u(float x, int mode) {
    const float v = round_mode(x, mode);
    if (std::isnan(v))
        return 0;
    if (v >= 4294967296.0f)
        return 0xFFFFFFFFu;
    if (v < 0.0f)
        return 0u;
    return static_cast<u32>(v);
}
#define DEF_VCVTRM(nm, is_signed, mode)                          \
    static void op_##nm(const u32* a, const u32*, u32* r) {      \
        for (int i = 0; i < 4; i++)                              \
            r[i] = is_signed ? to_fixed_rm_s(fval(ftz(a[i])), mode) \
                             : to_fixed_rm_u(fval(ftz(a[i])), mode); \
    }
DEF_VCVTRM(vcvtn_s32_f32, true, 0)
DEF_VCVTRM(vcvtn_u32_f32, false, 0)
DEF_VCVTRM(vcvta_s32_f32, true, 1)
DEF_VCVTRM(vcvta_u32_f32, false, 1)
DEF_VCVTRM(vcvtp_s32_f32, true, 2)
DEF_VCVTRM(vcvtp_u32_f32, false, 2)
DEF_VCVTRM(vcvtm_s32_f32, true, 3)
DEF_VCVTRM(vcvtm_u32_f32, false, 3)

#include "neon5_body.inc"

int main() {
    g_log = std::fopen("ref_mix.log", "w");
    test_body();
    if (g_log)
        std::fclose(g_log);
    std::printf("checksum %08x\n", g_acc);
    return 0;
}
