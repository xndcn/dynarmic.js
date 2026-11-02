// Host reference for neon2.c: implements ARM NEON semantics in plain C and
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

// vuzp: out_a = [even lanes of a, even lanes of b]; out_b = odd lanes.
#define R_UZP(nm, T, N)                                         \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N) / 2; i++) {                     \
            r[i] = a[2 * i];                                    \
            r[(N) / 2 + i] = b[2 * i];                          \
            r[(N) + i] = a[2 * i + 1];                          \
            r[(N) + (N) / 2 + i] = b[2 * i + 1];                \
        }                                                       \
    }

R_UZP(vuzp_8_q, u8, 16)
R_UZP(vuzp_16_q, u16, 8)
R_UZP(vuzp_32_q, u32, 4)
R_UZP(vuzp_8_d, u8, 8)
R_UZP(vuzp_16_d, u16, 4)
R_UZP(vuzp_32_d, u32, 2)

// vzip: out_a = interleave of the low halves; out_b = high halves.
#define R_ZIP(nm, T, N)                                         \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N) / 2; i++) {                     \
            r[2 * i] = a[i];                                    \
            r[2 * i + 1] = b[i];                                \
            r[(N) + 2 * i] = a[(N) / 2 + i];                    \
            r[(N) + 2 * i + 1] = b[(N) / 2 + i];                \
        }                                                       \
    }

R_ZIP(vzip_8_q, u8, 16)
R_ZIP(vzip_16_q, u16, 8)
R_ZIP(vzip_32_q, u32, 4)
R_ZIP(vzip_8_d, u8, 8)
R_ZIP(vzip_16_d, u16, 4)
R_ZIP(vzip_32_d, u32, 2)

// vtrn: out_a = [a_even, b_even] interleaved lane pairs, out_b = odd lanes.
#define R_TRN(nm, T, N)                                         \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N) / 2; i++) {                     \
            r[2 * i] = a[2 * i];                                \
            r[2 * i + 1] = b[2 * i];                            \
            r[(N) + 2 * i] = a[2 * i + 1];                      \
            r[(N) + 2 * i + 1] = b[2 * i + 1];                  \
        }                                                       \
    }

R_TRN(vtrn_8_q, u8, 16)
R_TRN(vtrn_16_q, u16, 8)
R_TRN(vtrn_32_q, u32, 4)
R_TRN(vtrn_8_d, u8, 8)
R_TRN(vtrn_16_d, u16, 4)
R_TRN(vtrn_32_d, u32, 2)

// vrev: reverse elements of ELEM bytes within groups of GROUP bytes.
#define R_REV(nm, ELEM, GROUP)                                  \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const u8* a = (const u8*)A;                             \
        u8* r = (u8*)R;                                         \
        for (int g = 0; g < 16; g += (GROUP))                   \
            for (int e = 0; e < (GROUP); e += (ELEM))           \
                for (int k = 0; k < (ELEM); k++)                \
                    r[g + e + k] = a[g + (GROUP) - (ELEM) - e + k];     \
    }

R_REV(vrev16_8, 1, 2)
R_REV(vrev32_8, 1, 4)
R_REV(vrev32_16, 2, 4)
R_REV(vrev64_8, 1, 8)
R_REV(vrev64_16, 2, 8)
R_REV(vrev64_32, 4, 8)

// vext: byte funnel shift of (a,b) right by K bytes.
#define R_EXT(nm, K, N)                                         \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const u8* a = (const u8*)A;                             \
        const u8* b = (const u8*)B;                             \
        u8* r = (u8*)R;                                         \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (i + (K) < (N)) ? a[i + (K)] : b[i + (K) - (N)];     \
    }

R_EXT(vext_8_q3, 3, 16)
R_EXT(vext_8_q9, 9, 16)
R_EXT(vext_8_d3, 3, 8)
R_EXT(vext_8_d6, 6, 8)

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

R_QADD(vqadd_s8, s8, -128, 127)
R_QADD(vqadd_s16, s16, -32768, 32767)
R_QADD(vqadd_u8, u8, 0, 255)
R_QADD(vqadd_u16, u16, 0, 65535)
R_QSUB(vqsub_s8, s8, -128, 127)
R_QSUB(vqsub_s16, s16, -32768, 32767)
R_QSUB(vqsub_u8, u8, 0, 255)
R_QSUB(vqsub_u16, u16, 0, 65535)

// absolute difference (result is unsigned, can exceed the signed range)
#define R_ABD(nm, T, UT, WIDE)                                  \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        UT* r = (UT*)R;                                         \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            WIDE d = (WIDE)a[i] - (WIDE)b[i];                   \
            r[i] = (UT)(d < 0 ? -d : d);                        \
        }                                                       \
    }

R_ABD(vabd_s8, s8, u8, int)
R_ABD(vabd_s16, s16, u16, int)
R_ABD(vabd_s32, s32, u32, s64)
R_ABD(vabd_u8, u8, u8, int)
R_ABD(vabd_u16, u16, u16, int)
R_ABD(vabd_u32, u32, u32, s64)

// saturating narrow (only the low half is stored)
#define R_QMOVN(nm, T, UT, LO, HI)                              \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        UT* r = (UT*)R;                                         \
        for (int i = 0; i < (int)(8 / sizeof(UT)); i++) {       \
            s64 v = (s64)a[i];                                  \
            r[i] = (UT)(v < (LO) ? (LO) : v > (HI) ? (HI) : v); \
        }                                                       \
    }

R_QMOVN(vqmovn_s16, s16, u8, -128, 127)
R_QMOVN(vqmovn_s32, s32, u16, -32768, 32767)
R_QMOVN(vqmovn_u16, u16, u8, 0, 255)
R_QMOVN(vqmovn_u32, u32, u16, 0, 65535)
R_QMOVN(vqmovun_s16, s16, u8, 0, 255)
R_QMOVN(vqmovun_s32, s32, u16, 0, 65535)

// count leading zeros
#define R_CLZ(nm, T, W)                                         \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            T v = a[i];                                         \
            int n = 0;                                          \
            for (int k = (W) - 1; k >= 0 && !(v & ((T)1 << k)); k--)    \
                n++;                                            \
            r[i] = (T)n;                                        \
        }                                                       \
    }

R_CLZ(vclz_8, u8, 8)
R_CLZ(vclz_16, u16, 16)
R_CLZ(vclz_32, u32, 32)

// register vshl: shift amount is the low byte of each amount lane, signed.
#define R_VSHL(nm, T, ST, W)                                    \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++) {       \
            s8 amt = (s8)(u8)b[i];                              \
            ST x = (ST)a[i];                                    \
            if (amt >= (W))                                     \
                r[i] = 0;                                       \
            else if (amt <= -(W))                               \
                r[i] = (T)(sizeof(ST) == sizeof(T) && (ST)-1 < 0 ? x >> ((W) - 1) : 0); \
            else if (amt < 0)                                   \
                r[i] = (T)(x >> -amt);                          \
            else                                                \
                r[i] = (T)(a[i] << amt);                        \
        }                                                       \
    }

R_VSHL(vshl_s8, u8, s8, 8)
R_VSHL(vshl_s16, u16, s16, 16)
R_VSHL(vshl_s32, u32, s32, 32)
R_VSHL(vshl_s64, u64, s64, 64)
R_VSHL(vshl_u8, u8, u8, 8)
R_VSHL(vshl_u16, u16, u16, 16)
R_VSHL(vshl_u32, u32, u32, 32)
R_VSHL(vshl_u64, u64, u64, 64)

// vtbl/vtbx
#define R_TBL(nm, NREGS)                                        \
    static void op_##nm(const u32* TAB, const u32* IDX, u32* R) {       \
        const u8* tab = (const u8*)TAB;                         \
        const u8* idx = (const u8*)IDX;                         \
        u8* r = (u8*)R;                                         \
        for (int i = 0; i < 8; i++)                             \
            r[i] = idx[i] < 8 * (NREGS) ? tab[idx[i]] : 0;      \
    }

R_TBL(vtbl1, 1)
R_TBL(vtbl2, 2)
R_TBL(vtbl4, 4)

#define R_TBX(nm, NREGS)                                        \
    static void op_##nm(const u32* TAB, const u32* IDX, const u32* DEF, u32* R) { \
        const u8* tab = (const u8*)TAB;                         \
        const u8* idx = (const u8*)IDX;                         \
        const u8* def = (const u8*)DEF;                         \
        u8* r = (u8*)R;                                         \
        for (int i = 0; i < 8; i++)                             \
            r[i] = idx[i] < 8 * (NREGS) ? tab[idx[i]] : def[i]; \
    }

R_TBX(vtbx2, 2)
R_TBX(vtbx3, 3)

#include "neon2_body.inc"

int main(void) {
#ifdef LOG_MIX
    g_logf = fopen("ref2_mix.log", "w");
#endif
    test_body();
#ifdef LOG_MIX
    fclose(g_logf);
#endif
    printf("checksum %08x\n", g_acc);
    return 0;
}
