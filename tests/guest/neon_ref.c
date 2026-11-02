// Host reference for neon.c: implements ARM NEON semantics in plain C and
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

// binary lane op: r[i] = EXPR over a[i], b[i]
#define R_BIN(nm, T, N, EXPR)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (T)(EXPR);                                   \
    }

// unary lane op: r[i] = EXPR over a[i]
#define R_UN(nm, T, N, EXPR)                                    \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (T)(EXPR);                                   \
    }

R_BIN(vadd_i8, u8, 16, a[i] + b[i])
R_BIN(vadd_i16, u16, 8, a[i] + b[i])
R_BIN(vadd_i32, u32, 4, a[i] + b[i])
R_BIN(vadd_i64, u64, 2, a[i] + b[i])
R_BIN(vsub_i8, u8, 16, a[i] - b[i])
R_BIN(vsub_i16, u16, 8, a[i] - b[i])
R_BIN(vsub_i32, u32, 4, a[i] - b[i])
R_BIN(vsub_i64, u64, 2, a[i] - b[i])
R_BIN(vand, u32, 4, a[i] & b[i])
R_BIN(vbic, u32, 4, a[i] & ~b[i])
R_BIN(vorr, u32, 4, a[i] | b[i])
R_BIN(veor, u32, 4, a[i] ^ b[i])
R_BIN(vmul_i8, u8, 16, (u8)(a[i] * b[i]))
R_BIN(vmul_i16, u16, 8, (u16)(a[i] * b[i]))
R_BIN(vmul_i32, u32, 4, a[i] * b[i])

R_BIN(vmin_s8, s8, 16, a[i] < b[i] ? a[i] : b[i])
R_BIN(vmin_s16, s16, 8, a[i] < b[i] ? a[i] : b[i])
R_BIN(vmin_s32, s32, 4, a[i] < b[i] ? a[i] : b[i])
R_BIN(vmin_u8, u8, 16, a[i] < b[i] ? a[i] : b[i])
R_BIN(vmin_u16, u16, 8, a[i] < b[i] ? a[i] : b[i])
R_BIN(vmin_u32, u32, 4, a[i] < b[i] ? a[i] : b[i])
R_BIN(vmax_s8, s8, 16, a[i] > b[i] ? a[i] : b[i])
R_BIN(vmax_s16, s16, 8, a[i] > b[i] ? a[i] : b[i])
R_BIN(vmax_s32, s32, 4, a[i] > b[i] ? a[i] : b[i])
R_BIN(vmax_u8, u8, 16, a[i] > b[i] ? a[i] : b[i])
R_BIN(vmax_u16, u16, 8, a[i] > b[i] ? a[i] : b[i])
R_BIN(vmax_u32, u32, 4, a[i] > b[i] ? a[i] : b[i])

R_BIN(vceq_i8, u8, 16, a[i] == b[i] ? 0xFF : 0)
R_BIN(vceq_i16, u16, 8, a[i] == b[i] ? 0xFFFF : 0)
R_BIN(vceq_i32, u32, 4, a[i] == b[i] ? 0xFFFFFFFFu : 0)
R_BIN(vcgt_s8, s8, 16, a[i] > b[i] ? (s8)-1 : 0)
R_BIN(vcgt_s16, s16, 8, a[i] > b[i] ? (s16)-1 : 0)
R_BIN(vcgt_s32, s32, 4, a[i] > b[i] ? -1 : 0)

// halving: VHADD truncates (floor via arithmetic shift for signed),
// VRHADD rounds (a+b+1)>>1; VHSUB floors (a-b)>>1.
R_BIN(vhadd_s8, s8, 16, (s8)(((int)a[i] + (int)b[i]) >> 1))
R_BIN(vhadd_s16, s16, 8, (s16)(((int)a[i] + (int)b[i]) >> 1))
R_BIN(vhadd_s32, s32, 4, (s32)(((s64)a[i] + (s64)b[i]) >> 1))
R_BIN(vhadd_u8, u8, 16, ((unsigned)a[i] + b[i]) >> 1)
R_BIN(vhadd_u16, u16, 8, ((unsigned)a[i] + b[i]) >> 1)
R_BIN(vhadd_u32, u32, 4, (((u64)a[i] + b[i]) >> 1))
R_BIN(vrhadd_s8, s8, 16, (s8)(((int)a[i] + (int)b[i] + 1) >> 1))
R_BIN(vrhadd_s16, s16, 8, (s16)(((int)a[i] + (int)b[i] + 1) >> 1))
R_BIN(vrhadd_s32, s32, 4, (s32)(((s64)a[i] + (s64)b[i] + 1) >> 1))
R_BIN(vrhadd_u8, u8, 16, ((unsigned)a[i] + b[i] + 1) >> 1)
R_BIN(vrhadd_u16, u16, 8, ((unsigned)a[i] + b[i] + 1) >> 1)
R_BIN(vrhadd_u32, u32, 4, (((u64)a[i] + b[i] + 1) >> 1))
R_BIN(vhsub_s8, s8, 16, (s8)(((int)a[i] - (int)b[i]) >> 1))
R_BIN(vhsub_s16, s16, 8, (s16)(((int)a[i] - (int)b[i]) >> 1))
R_BIN(vhsub_s32, s32, 4, (s32)(((s64)a[i] - (s64)b[i]) >> 1))
R_BIN(vhsub_u8, u8, 16, (u8)(((int)a[i] - (int)b[i]) >> 1))
R_BIN(vhsub_u16, u16, 8, (u16)(((int)a[i] - (int)b[i]) >> 1))
R_BIN(vhsub_u32, u32, 4, (u32)(((s64)a[i] - (s64)b[i]) >> 1))

static u32 popcnt8(u32 v) {
    v -= (v >> 1) & 0x55u;
    v = (v & 0x33u) + ((v >> 2) & 0x33u);
    return (v + (v >> 4)) & 0x0Fu;
}

// abs of the most negative value wraps back to itself
R_UN(vabs_s8, s8, 16, a[i] < 0 ? (s8)(0 - (u8)a[i]) : a[i])
R_UN(vabs_s16, s16, 8, a[i] < 0 ? (s16)(0 - (u16)a[i]) : a[i])
R_UN(vabs_s32, s32, 4, a[i] < 0 ? (s32)(0u - (u32)a[i]) : a[i])
R_UN(vcnt_8, u8, 16, popcnt8(a[i]))
R_UN(vmvn, u32, 4, ~a[i])

// VPADDL: adjacent pairs of a, widened
#define R_PADDL(nm, ST, DT, N)                                  \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const ST* a = (const ST*)A;                             \
        DT* r = (DT*)R;                                         \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (DT)((DT)a[2 * i] + (DT)a[2 * i + 1]);       \
    }

R_PADDL(vpaddl_s8, s8, s16, 8)
R_PADDL(vpaddl_s16, s16, s32, 4)
R_PADDL(vpaddl_s32, s32, s64, 2)
R_PADDL(vpaddl_u8, u8, u16, 8)
R_PADDL(vpaddl_u16, u16, u32, 4)
R_PADDL(vpaddl_u32, u32, u64, 2)

// VMOVL: extend the low half
#define R_MOVL(nm, ST, DT, N)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const ST* a = (const ST*)A;                             \
        DT* r = (DT*)R;                                         \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (DT)a[i];                                    \
    }

R_MOVL(vmovl_s8, s8, s16, 8)
R_MOVL(vmovl_s16, s16, s32, 4)
R_MOVL(vmovl_s32, s32, s64, 2)
R_MOVL(vmovl_u8, u8, u16, 8)
R_MOVL(vmovl_u16, u16, u32, 4)
R_MOVL(vmovl_u32, u32, u64, 2)

// VMOVN: truncate each lane to its low half; only 8 bytes are stored
#define R_MOVN(nm, ST, DT, N)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const ST* a = (const ST*)A;                             \
        DT* r = (DT*)R;                                         \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (DT)a[i];                                    \
    }

R_MOVN(vmovn_i16, u16, u8, 8)
R_MOVN(vmovn_i32, u32, u16, 4)
R_MOVN(vmovn_i64, u64, u32, 2)

// VMULL: widening multiply of the low halves
#define R_MULL(nm, ST, DT, N)                                   \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const ST* a = (const ST*)A;                             \
        const ST* b = (const ST*)B;                             \
        DT* r = (DT*)R;                                         \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (DT)a[i] * (DT)b[i];                         \
    }

R_MULL(vmull_s8, s8, s16, 8)
R_MULL(vmull_s16, s16, s32, 4)
R_MULL(vmull_s32, s32, s64, 2)
R_MULL(vmull_u8, u8, u16, 8)
R_MULL(vmull_u16, u16, u32, 4)
R_MULL(vmull_u32, u32, u64, 2)

// VPADD (D regs): low 64 bits only -> [pairs(a_lo), pairs(b_lo)]
#define R_VPADD(nm, T, N)                                       \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        const T* a = (const T*)A;                               \
        const T* b = (const T*)B;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N) / 2; i++) {                     \
            r[i] = (T)(a[2 * i] + a[2 * i + 1]);                \
            r[(N) / 2 + i] = (T)(b[2 * i] + b[2 * i + 1]);      \
        }                                                       \
    }

R_VPADD(vpadd_i8, u8, 8)
R_VPADD(vpadd_i16, u16, 4)
R_VPADD(vpadd_i32, u32, 2)

// immediate shifts
#define R_SH(nm, T, N, EXPR)                                    \
    static void op_##nm(const u32* A, const u32* B, u32* R) {   \
        (void)B;                                                \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (N); i++)                           \
            r[i] = (T)(EXPR);                                   \
    }

R_SH(vshl_i8_3, u8, 16, a[i] << 3)
R_SH(vshl_i16_9, u16, 8, a[i] << 9)
R_SH(vshl_i32_31, u32, 4, a[i] << 31)
R_SH(vshl_i64_1, u64, 2, a[i] << 1)
R_SH(vshl_i64_63, u64, 2, a[i] << 63)
R_SH(vshr_u8_1, u8, 16, a[i] >> 1)
R_SH(vshr_u8_7, u8, 16, a[i] >> 7)
R_SH(vshr_u16_15, u16, 8, a[i] >> 15)
R_SH(vshr_u32_1, u32, 4, a[i] >> 1)
R_SH(vshr_u64_40, u64, 2, a[i] >> 40)
R_SH(vshr_s8_5, s8, 16, a[i] >> 5)
R_SH(vshr_s16_1, s16, 8, a[i] >> 1)
R_SH(vshr_s32_31, s32, 4, a[i] >> 31)
R_SH(vshr_s64_62, s64, 2, a[i] >> 62)

// broadcasts from GPR
#define R_DUP(nm, T, EXPR)                                      \
    static void op_##nm(u32 x, u32* R) {                        \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++)         \
            r[i] = (T)(EXPR);                                   \
    }

R_DUP(vdup_8, u8, x)
R_DUP(vdup_16, u16, x)
R_DUP(vdup_32, u32, x)

// broadcasts from a vector element
#define R_DUPE(nm, T, LANE)                                     \
    static void op_##nm(const u32* A, u32* R) {                 \
        const T* a = (const T*)A;                               \
        T* r = (T*)R;                                           \
        for (int i = 0; i < (int)(16 / sizeof(T)); i++)         \
            r[i] = a[LANE];                                     \
    }

R_DUPE(vdup_e16_l2, u16, 2)
R_DUPE(vdup_e32_l1, u32, 1)

// lane extracts
static u32 op_get8_d0l3(const u32* A) {
    return ((const u8*)A)[3];
}
static u32 op_get16_d1l2(const u32* A) {
    return ((const u16*)A)[6];  // d1 = words 2,3; lane 2 of d1 = half 6
}
static u32 op_get32_d0l1(const u32* A) {
    return A[1];
}

// lane inserts
static void op_set8_d0l5(const u32* A, u32 v, u32* R) {
    memcpy(R, A, 16);
    ((u8*)R)[5] = (u8)v;
}
static void op_set16_d1l2(const u32* A, u32 v, u32* R) {
    memcpy(R, A, 16);
    ((u16*)R)[6] = (u16)v;
}
static void op_set32_d0l1(const u32* A, u32 v, u32* R) {
    memcpy(R, A, 16);
    R[1] = v;
}

// 64-bit lane get/set via two GPRs
static void op_get64_d1(const u32* A, u32* R) {
    R[0] = A[2];
    R[1] = A[3];
}
static void op_set64_d1(const u32* A, u32 lo, u32 hi, u32* R) {
    memcpy(R, A, 16);
    R[2] = lo;
    R[3] = hi;
}

#include "neon_body.inc"

int main(void) {
#ifdef LOG_MIX
    g_logf = fopen("ref_mix.log", "w");
#endif
    test_body();
#ifdef LOG_MIX
    fclose(g_logf);
#endif
    printf("checksum %08x\n", g_acc);
    return 0;
}
