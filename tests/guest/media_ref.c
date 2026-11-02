// Host reference for media.c: implements ARM semantics in plain C and must
// produce the identical FNV-1a checksum. Matches dynarmic's behaviour where
// it deviates from hardware (packed saturating ops do not set Q).

#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

static u32 g_acc = 2166136261u;
#ifdef LOG_MIX
static FILE* g_logf;
#endif
static void mix(u32 v) {
#ifdef LOG_MIX
    fprintf(g_logf, "%08x\n", v);
#endif
    g_acc = (g_acc ^ v) * 16777619u;
}

static u32 g_q = 0;
static u32 g_ge = 0;
static u32 read_q(void) { return g_q; }
static u32 read_ge(void) { return g_ge; }

static s32 clamp_q(s64 v, s64 lo, s64 hi) {
    if (v < lo) {
        g_q = 1;
        return (s32)lo;
    }
    if (v > hi) {
        g_q = 1;
        return (s32)hi;
    }
    return (s32)v;
}

static s32 clamp_noq(s32 v, s32 lo, s32 hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// --- scalar saturating ---

static u32 op_qadd(u32 a, u32 b) {
    return (u32)clamp_q((s64)(s32)a + (s32)b, INT32_MIN, INT32_MAX);
}
static u32 op_qsub(u32 a, u32 b) {
    return (u32)clamp_q((s64)(s32)a - (s32)b, INT32_MIN, INT32_MAX);
}
static u32 op_qdadd(u32 a, u32 b) {
    const s32 d = clamp_q(2 * (s64)(s32)b, INT32_MIN, INT32_MAX);
    return (u32)clamp_q((s64)(s32)a + d, INT32_MIN, INT32_MAX);
}
static u32 op_qdsub(u32 a, u32 b) {
    const s32 d = clamp_q(2 * (s64)(s32)b, INT32_MIN, INT32_MAX);
    return (u32)clamp_q((s64)(s32)a - d, INT32_MIN, INT32_MAX);
}

static u32 op_ssat(u32 a, u32 n) {
    if (n == 32)
        return a;
    return (u32)clamp_q((s32)a, -(1 << (n - 1)), (1 << (n - 1)) - 1);
}
static u32 op_ssat_lsl(u32 a, u32 n, u32 s) {
    return (u32)clamp_q((s32)(a << s), -(1 << (n - 1)), (1 << (n - 1)) - 1);
}
static u32 op_ssat_asr(u32 a, u32 n, u32 s) {
    return (u32)clamp_q((s32)a >> s, -(1 << (n - 1)), (1 << (n - 1)) - 1);
}
static u32 op_usat(u32 a, u32 n) {
    return (u32)clamp_q((s32)a, 0, (1 << n) - 1);
}
static u32 op_usat_asr(u32 a, u32 n, u32 s) {
    return (u32)clamp_q((s32)a >> s, 0, (1 << n) - 1);
}

static u32 op_ssat16(u32 a, u32 n) {
    const s32 lo = clamp_q((s16)a, -(1 << (n - 1)), (1 << (n - 1)) - 1);
    const s32 hi = clamp_q((s16)(a >> 16), -(1 << (n - 1)), (1 << (n - 1)) - 1);
    return (u16)lo | ((u32)(u16)hi << 16);
}
static u32 op_usat16(u32 a, u32 n) {
    const s32 lo = clamp_q((s16)a, 0, (1 << n) - 1);
    const s32 hi = clamp_q((s16)(a >> 16), 0, (1 << n) - 1);
    return (u16)lo | ((u32)(u16)hi << 16);
}

// --- parallel ops with GE ---

static u32 op_sadd8(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 4; i++) {
        const s32 s = (s8)(a >> (8 * i)) + (s8)(b >> (8 * i));
        r |= (u32)(u8)s << (8 * i);
        g_ge |= (s >= 0) << i;
    }
    return r;
}
static u32 op_ssub8(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 4; i++) {
        const s32 d = (s8)(a >> (8 * i)) - (s8)(b >> (8 * i));
        r |= (u32)(u8)d << (8 * i);
        g_ge |= (d >= 0) << i;
    }
    return r;
}
static u32 op_uadd8(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 4; i++) {
        const u32 s = (a >> (8 * i) & 0xFF) + (b >> (8 * i) & 0xFF);
        r |= (s & 0xFF) << (8 * i);
        g_ge |= (s >= 256) << i;
    }
    return r;
}
static u32 op_usub8(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 4; i++) {
        const u32 ua = a >> (8 * i) & 0xFF, ub = b >> (8 * i) & 0xFF;
        r |= ((ua - ub) & 0xFF) << (8 * i);
        g_ge |= (ua >= ub) << i;
    }
    return r;
}

static u32 op_sadd16(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 2; i++) {
        const s32 s = (s16)(a >> (16 * i)) + (s16)(b >> (16 * i));
        r |= (u32)(u16)s << (16 * i);
        g_ge |= (s >= 0 ? 3u : 0u) << (2 * i);
    }
    return r;
}
static u32 op_ssub16(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 2; i++) {
        const s32 d = (s16)(a >> (16 * i)) - (s16)(b >> (16 * i));
        r |= (u32)(u16)d << (16 * i);
        g_ge |= (d >= 0 ? 3u : 0u) << (2 * i);
    }
    return r;
}
static u32 op_uadd16(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 2; i++) {
        const u32 s = (a >> (16 * i) & 0xFFFF) + (b >> (16 * i) & 0xFFFF);
        r |= (s & 0xFFFF) << (16 * i);
        g_ge |= (s >= 65536 ? 3u : 0u) << (2 * i);
    }
    return r;
}
static u32 op_usub16(u32 a, u32 b) {
    u32 r = 0;
    g_ge = 0;
    for (u32 i = 0; i < 2; i++) {
        const u32 ua = a >> (16 * i) & 0xFFFF, ub = b >> (16 * i) & 0xFFFF;
        r |= ((ua - ub) & 0xFFFF) << (16 * i);
        g_ge |= (ua >= ub ? 3u : 0u) << (2 * i);
    }
    return r;
}

// sasx: lo = a_lo - b_hi, hi = a_hi + b_lo (signed)
static u32 op_sasx(u32 a, u32 b) {
    const s32 d = (s16)a - (s16)(b >> 16);
    const s32 s = (s16)(a >> 16) + (s16)b;
    g_ge = (d >= 0 ? 3u : 0u) | (s >= 0 ? 0xCu : 0u);
    return (u16)d | ((u32)(u16)s << 16);
}
// ssax: lo = a_lo + b_hi, hi = a_hi - b_lo (signed)
static u32 op_ssax(u32 a, u32 b) {
    const s32 s = (s16)a + (s16)(b >> 16);
    const s32 d = (s16)(a >> 16) - (s16)b;
    g_ge = (s >= 0 ? 3u : 0u) | (d >= 0 ? 0xCu : 0u);
    return (u16)s | ((u32)(u16)d << 16);
}
static u32 op_uasx(u32 a, u32 b) {
    const u32 ua_lo = a & 0xFFFF, ua_hi = a >> 16, ub_lo = b & 0xFFFF, ub_hi = b >> 16;
    const u32 s = ua_hi + ub_lo;
    g_ge = (ua_lo >= ub_hi ? 3u : 0u) | (s >= 65536 ? 0xCu : 0u);
    return ((ua_lo - ub_hi) & 0xFFFF) | ((s & 0xFFFF) << 16);
}
static u32 op_usax(u32 a, u32 b) {
    const u32 ua_lo = a & 0xFFFF, ua_hi = a >> 16, ub_lo = b & 0xFFFF, ub_hi = b >> 16;
    const u32 s = ua_lo + ub_hi;
    g_ge = (s >= 65536 ? 3u : 0u) | (ua_hi >= ub_lo ? 0xCu : 0u);
    return (s & 0xFFFF) | (((ua_hi - ub_lo) & 0xFFFF) << 16);
}

// --- halving (truncating, no flags) ---

static u32 op_shadd8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (u32)(u8)(((s8)(a >> (8 * i)) + (s8)(b >> (8 * i))) >> 1) << (8 * i);
    return r;
}
static u32 op_shsub8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (u32)(u8)(((s8)(a >> (8 * i)) - (s8)(b >> (8 * i))) >> 1) << (8 * i);
    return r;
}
static u32 op_uhadd8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (((a >> (8 * i) & 0xFF) + (b >> (8 * i) & 0xFF)) >> 1) << (8 * i);
    return r;
}
static u32 op_uhsub8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (((a >> (8 * i) & 0xFF) - (b >> (8 * i) & 0xFF)) >> 1 & 0xFF) << (8 * i);
    return r;
}
static u32 op_shadd16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (u32)(u16)(((s16)(a >> (16 * i)) + (s16)(b >> (16 * i))) >> 1) << (16 * i);
    return r;
}
static u32 op_shsub16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (u32)(u16)(((s16)(a >> (16 * i)) - (s16)(b >> (16 * i))) >> 1) << (16 * i);
    return r;
}
static u32 op_uhadd16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (((a >> (16 * i) & 0xFFFF) + (b >> (16 * i) & 0xFFFF)) >> 1) << (16 * i);
    return r;
}
static u32 op_uhsub16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (((a >> (16 * i) & 0xFFFF) - (b >> (16 * i) & 0xFFFF)) >> 1 & 0xFFFF) << (16 * i);
    return r;
}
static u32 op_shasx(u32 a, u32 b) {
    const s32 d = ((s16)a - (s16)(b >> 16)) >> 1;
    const s32 s = ((s16)(a >> 16) + (s16)b) >> 1;
    return (u16)d | ((u32)(u16)s << 16);
}
static u32 op_shsax(u32 a, u32 b) {
    const s32 s = ((s16)a + (s16)(b >> 16)) >> 1;
    const s32 d = ((s16)(a >> 16) - (s16)b) >> 1;
    return (u16)s | ((u32)(u16)d << 16);
}
static u32 op_uhasx(u32 a, u32 b) {
    const u32 d = ((a & 0xFFFF) - (b >> 16)) >> 1;
    const u32 s = ((a >> 16) + (b & 0xFFFF)) >> 1;
    return (d & 0xFFFF) | (s << 16);
}
static u32 op_uhsax(u32 a, u32 b) {
    const u32 s = ((a & 0xFFFF) + (b >> 16)) >> 1;
    const u32 d = ((a >> 16) - (b & 0xFFFF)) >> 1;
    return s | ((d & 0xFFFF) << 16);
}

// --- parallel saturating (no Q flag in dynarmic) ---

static u32 op_qadd8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (u32)(u8)clamp_noq((s8)(a >> (8 * i)) + (s8)(b >> (8 * i)), -128, 127) << (8 * i);
    return r;
}
static u32 op_qsub8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (u32)(u8)clamp_noq((s8)(a >> (8 * i)) - (s8)(b >> (8 * i)), -128, 127) << (8 * i);
    return r;
}
static u32 op_uqadd8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (u32)clamp_noq((s32)(a >> (8 * i) & 0xFF) + (s32)(b >> (8 * i) & 0xFF), 0, 255) << (8 * i);
    return r;
}
static u32 op_uqsub8(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (u32)clamp_noq((s32)(a >> (8 * i) & 0xFF) - (s32)(b >> (8 * i) & 0xFF), 0, 255) << (8 * i);
    return r;
}
static u32 op_qadd16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (u32)(u16)clamp_noq((s16)(a >> (16 * i)) + (s16)(b >> (16 * i)), -32768, 32767) << (16 * i);
    return r;
}
static u32 op_qsub16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (u32)(u16)clamp_noq((s16)(a >> (16 * i)) - (s16)(b >> (16 * i)), -32768, 32767) << (16 * i);
    return r;
}
static u32 op_uqadd16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (u32)clamp_noq((s32)(a >> (16 * i) & 0xFFFF) + (s32)(b >> (16 * i) & 0xFFFF), 0, 65535) << (16 * i);
    return r;
}
static u32 op_uqsub16(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 2; i++)
        r |= (u32)clamp_noq((s32)(a >> (16 * i) & 0xFFFF) - (s32)(b >> (16 * i) & 0xFFFF), 0, 65535) << (16 * i);
    return r;
}

// --- misc ---

static u32 op_usad8(u32 a, u32 b) {
    u32 s = 0;
    for (u32 i = 0; i < 4; i++) {
        const s32 d = (s32)(a >> (8 * i) & 0xFF) - (s32)(b >> (8 * i) & 0xFF);
        s += d < 0 ? (u32)-d : (u32)d;
    }
    return s;
}
static u32 op_usada8(u32 a, u32 b, u32 c) {
    return c + op_usad8(a, b);
}
static u32 op_sel(u32 a, u32 b) {
    u32 r = 0;
    for (u32 i = 0; i < 4; i++)
        r |= (((g_ge >> i) & 1 ? a : b) >> (8 * i) & 0xFF) << (8 * i);
    return r;
}

#include "media_body.inc"

int main(void) {
#ifdef LOG_MIX
    g_logf = fopen("ref_mix.log", "w");
#endif
    test_body();
    printf("checksum %08x q %u ge %x\n", g_acc, read_q(), read_ge());
    return 0;
}
