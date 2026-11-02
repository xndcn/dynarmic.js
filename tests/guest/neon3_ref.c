// Host reference for neon3.c: implements ARM NEON semantics in plain C and
// must produce the identical FNV-1a checksum.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef __int128 s128;
typedef unsigned __int128 u128;

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

// saturating add/sub
#define R_QADD(nm, T, LO, HI)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s64 v = (s64)a[i] + (s64)b[i];                      \
            r[i] = (T)(v < (LO) ? (LO) : v > (HI) ? (HI) : v);  \
        }                                                       \
    }
#define R_QSUB(nm, T, LO, HI)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s64 v = (s64)a[i] - (s64)b[i];                      \
            r[i] = (T)(v < (LO) ? (LO) : v > (HI) ? (HI) : v);  \
        }                                                       \
    }

R_QADD(vqadd_s32, s32, -2147483648LL, 2147483647LL)
R_QSUB(vqsub_s32, s32, -2147483648LL, 2147483647LL)
R_QADD(vqadd_u32, u32, 0, 4294967295LL)
R_QSUB(vqsub_u32, u32, 0, 4294967295LL)

// saturating abs/neg (LO == MIN saturates to MAX)
#define R_QABS(nm, T, LO, HI)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s64 v = (s64)a[i];                                  \
            r[i] = (T)(v == (LO) ? (HI) : v < 0 ? -v : v);      \
        }                                                       \
    }
#define R_QNEG(nm, T, LO, HI)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s64 v = (s64)a[i];                                  \
            r[i] = (T)(v == (LO) ? (HI) : -v);                  \
        }                                                       \
    }

R_QABS(vqabs_s8, s8, -128, 127)
R_QABS(vqabs_s16, s16, -32768, 32767)
R_QABS(vqabs_s32, s32, -2147483648LL, 2147483647LL)
R_QNEG(vqneg_s8, s8, -128, 127)
R_QNEG(vqneg_s16, s16, -32768, 32767)
R_QNEG(vqneg_s32, s32, -2147483648LL, 2147483647LL)

// saturating left shift helper: x << amt saturated to [LO, HI]
#define QSHL_LEFT(T, ST, W, LO, HI, x, amt)                              \
    ((x) == 0 ? (T)0                                                     \
              : (amt) >= (W) ? (T)((ST)(x) < 0 ? (LO) : (HI))            \
                             : ({ s128 s = (s128)(ST)(x) << (amt);       \
                                  (T)(s < (LO) ? (LO) : s > (HI) ? (HI) : s); }))

#define R_VQSHL_S(nm, T, ST, W, LO, HI)                         \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s8 amt = (s8)(u8)b[i];                              \
            ST x = (ST)a[i];                                    \
            if (amt >= 0) {                                     \
                r[i] = QSHL_LEFT(T, ST, W, LO, HI, x, amt);     \
            } else {                                            \
                int sh = -amt > (W) - 1 ? (W) - 1 : -amt;       \
                r[i] = (T)(x >> sh);                            \
            }                                                   \
        }                                                       \
    }
#define R_VQSHL_U(nm, T, W)                                     \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        const T hi = (T)~ (T)0;                                 \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s8 amt = (s8)(u8)b[i];                              \
            T x = a[i];                                         \
            if (amt >= 0) {                                     \
                if (x == 0)                                     \
                    r[i] = 0;                                   \
                else if (amt >= (W))                            \
                    r[i] = hi;                                  \
                else {                                          \
                    u128 s = (u128)x << amt;                    \
                    r[i] = s > hi ? hi : (T)s;                  \
                }                                               \
            } else {                                            \
                r[i] = -amt >= (W) ? (T)0 : (T)(x >> -amt);     \
            }                                                   \
        }                                                       \
    }

R_VQSHL_S(vqshl_s8, u8, s8, 8, -128, 127)
R_VQSHL_S(vqshl_s16, u16, s16, 16, -32768, 32767)
R_VQSHL_S(vqshl_s32, u32, s32, 32, -2147483648LL, 2147483647LL)
R_VQSHL_S(vqshl_s64, u64, s64, 64, (s64)0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFLL)
R_VQSHL_U(vqshl_u8, u8, 8)
R_VQSHL_U(vqshl_u16, u16, 16)
R_VQSHL_U(vqshl_u32, u32, 32)
R_VQSHL_U(vqshl_u64, u64, 64)

// immediate vqshl: same semantics, fixed amount
#define R_VQSHLI_S(nm, T, ST, W, LO, HI, IMM)                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++)         \
            r[i] = QSHL_LEFT(T, ST, W, LO, HI, (ST)a[i], IMM);  \
    }
#define R_VQSHLI_U(nm, T, W, IMM)                               \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        const T hi = (T)~ (T)0;                                 \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            u128 s = (u128)a[i] << (IMM);                       \
            r[i] = s > hi ? hi : (T)s;                          \
        }                                                       \
    }

R_VQSHLI_S(vqshli_s8_3, u8, s8, 8, -128, 127, 3)
R_VQSHLI_S(vqshli_s16_9, u16, s16, 16, -32768, 32767, 9)
R_VQSHLI_S(vqshli_s32_20, u32, s32, 32, -2147483648LL, 2147483647LL, 20)
R_VQSHLI_S(vqshli_s64_40, u64, s64, 64, (s64)0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFLL, 40)
R_VQSHLI_U(vqshli_u8_5, u8, 8, 5)
R_VQSHLI_U(vqshli_u16_13, u16, 16, 13)
R_VQSHLI_U(vqshli_u32_31, u32, 32, 31)
R_VQSHLI_U(vqshli_u64_63, u64, 64, 63)

// vqshlu: negative input -> 0; else unsigned-saturating shift
#define R_VQSHLU(nm, T, ST, W, IMM)                             \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        const T hi = (T)~ (T)0;                                 \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            if ((ST)a[i] < 0) {                                 \
                r[i] = 0;                                       \
            } else {                                            \
                u128 s = (u128)a[i] << (IMM);                   \
                r[i] = s > hi ? hi : (T)s;                      \
            }                                                   \
        }                                                       \
    }

R_VQSHLU(vqshlu_s8_5, u8, s8, 8, 5)
R_VQSHLU(vqshlu_s16_13, u16, s16, 16, 13)
R_VQSHLU(vqshlu_s32_1, u32, s32, 32, 1)
R_VQSHLU(vqshlu_s64_63, u64, s64, 64, 63)

// rounding variable shift
#define R_VRSHL(nm, T, ST, W)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s8 amt = (s8)(u8)b[i];                              \
            if (amt >= 0) {                                     \
                r[i] = amt >= (W) ? (T)0 : (T)((T)a[i] << amt); \
            } else {                                            \
                int sh = -amt;                                  \
                if (sh > (W)) {                                 \
                    r[i] = 0;                                   \
                } else if (sh == (W) && (ST)-1 > 0) {           \
                    r[i] = (T)(a[i] >> ((W) - 1));              \
                } else if (sh == (W)) {                         \
                    r[i] = 0;                                   \
                } else {                                        \
                    ST x = (ST)a[i];                            \
                    r[i] = (T)(x >> sh) + (T)((x >> (sh - 1)) & 1);     \
                }                                               \
            }                                                   \
        }                                                       \
    }

R_VRSHL(vrshl_s8, u8, s8, 8)
R_VRSHL(vrshl_s16, u16, s16, 16)
R_VRSHL(vrshl_s32, u32, s32, 32)
R_VRSHL(vrshl_s64, u64, s64, 64)
R_VRSHL(vrshl_u8, u8, u8, 8)
R_VRSHL(vrshl_u16, u16, u16, 16)
R_VRSHL(vrshl_u32, u32, u32, 32)
R_VRSHL(vrshl_u64, u64, u64, 64)

// saturating doubling multiplies
#define R_QDMULH(nm, T, ST, W, ROUND)                           \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        const s64 lo = -(1LL << ((W) - 1));                     \
        const s64 hi = (1LL << ((W) - 1)) - 1;                  \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s64 p = (s64)(ST)a[i] * (s64)(ST)b[i];              \
            if (ROUND)                                          \
                p += 1LL << ((W) - 2);                          \
            s64 v = p >> ((W) - 1);                             \
            r[i] = (T)(v < lo ? lo : v > hi ? hi : v);          \
        }                                                       \
    }

R_QDMULH(vqdmulh_s16, u16, s16, 16, 0)
R_QDMULH(vqdmulh_s32, u32, s32, 32, 0)
R_QDMULH(vqrdmulh_s16, u16, s16, 16, 1)
R_QDMULH(vqrdmulh_s32, u32, s32, 32, 1)

#define R_QDMULL(nm, NT, NST, RT, RST)                          \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const NT* a = (const NT*)A;                             \
        const NT* b = (const NT*)B;                             \
        RT* r = (RT*)R;                                         \
        const s128 lo = -(s128)(RST)(1ULL << (8 * sizeof(RT) - 1));     \
        const s128 hi = (s128)((1ULL << (8 * sizeof(RT) - 1)) - 1);     \
        for (int i = 0; i < (int)(8 / sizeof(NT)); i++) {       \
            s128 v = 2 * (s128)(NST)a[i] * (s128)(NST)b[i];     \
            r[i] = (RT)(v < lo ? lo : v > hi ? hi : v);         \
        }                                                       \
    }


// polynomial (carryless) multiply
static void op_vmul_p8(const u32* A, const u32* B, u32* R) {
    const u8* a = (const u8*)A;
    const u8* b = (const u8*)B;
    u8* r = (u8*)R;
    for (int i = 0; i < 16; i++) {
        u8 v = 0;
        for (int k = 0; k < 8; k++)
            if ((a[i] >> k) & 1)
                v ^= (u8)(b[i] << k);
        r[i] = v;
    }
}

static void op_vmull_p8(const u32* A, const u32* B, u32* R) {
    const u8* a = (const u8*)A;
    const u8* b = (const u8*)B;
    u16* r = (u16*)R;
    for (int i = 0; i < 8; i++) {
        u16 v = 0;
        for (int k = 0; k < 8; k++)
            if ((a[i] >> k) & 1)
                v ^= (u16)(b[i] << k);
        r[i] = v;
    }
}

// reciprocal estimates (replicates dynarmic's Common:: LUT math)
static u8 recip_est(u32 a9) {
    u64 a = a9;  // in [256, 511]
    a = a * 2 + 1;
    u64 b = (1u << 19) / a;
    return (u8)((b + 1) / 2);
}
static u8 rsqrt_est(u32 a9) {
    u64 a = a9;  // in [128, 511]
    if (a < 256)
        a = a * 2 + 1;
    else
        a = (a | 1) * 2;
    u64 b = 512;
    while (a * (b + 1) * (b + 1) < (1u << 28))
        b++;
    return (u8)((b + 1) / 2);
}

#define R_RECPE(nm, EST, MASK)                                  \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const u32* a = A;                                       \
        u32* r = R;                                             \
        for (int i = 0; i < 4; i++) {                           \
            if ((a[i] & (MASK)) == 0)                           \
                r[i] = 0xFFFFFFFFu;                             \
            else                                                \
                r[i] = (0x100u | EST((a[i] >> 23) & 0x1FF)) << 23;      \
        }                                                       \
    }

R_RECPE(vrecpe_u32, recip_est, 0x80000000u)
R_RECPE(vrsqrte_u32, rsqrt_est, 0xC0000000u)

#include "neon3_body.inc"

int main(void) {
#ifdef LOG_MIX
    g_logf = fopen("ref3_mix.log", "w");
#endif
    test_body();
#ifdef LOG_MIX
    fclose(g_logf);
#endif
    printf("checksum %08x\n", g_acc);
    return 0;
}
