// NEON integer guest test, tranche 2: vuzp/vzip/vtrn/vrev/vext, vqadd/vqsub
// (8/16-bit lanes), vabd, vqmovn/vqmovun, vclz, register vshl, vtbl/vtbx.
// Results mix into an FNV-1a checksum; neon2_ref must produce the same value.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define OUT ((volatile u32*)0x00080000)
#define MAGIC_ADDR ((volatile u32*)0x00080040)

static u32 g_acc = 2166136261u;
#ifdef LOG_MIX
static u32* g_log = (u32*)0x90000;
#endif
static void mix(u32 v) {
#ifdef LOG_MIX
    *g_log++ = v;
#endif
    g_acc = (g_acc ^ v) * 16777619u;
}

// q0 = f(q0), q1 = f(q1) two-output op (vuzp/vzip/vtrn on Q regs);
// stores q0 to r[0..3], q1 to r[4..7].
#define VOP2X2(nm, insn)                                 \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q0}\n"                          \
            "vstmia %3, {q1}\n"                          \
            :: "r"(a), "r"(b), "r"(r), "r"(r + 4)        \
            : "q0", "q1", "memory");                     \
    }

// two-output op on D regs d0/d2; stores d0 to r[0..1], d2 to r[2..3].
#define VOP2X2D(nm, insn)                                \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {d0}\n"                          \
            "vstmia %3, {d2}\n"                          \
            :: "r"(a), "r"(b), "r"(r), "r"(r + 2)        \
            : "q0", "q1", "memory");                     \
    }

// q2 = insn(q0, q1); stores q2 (4 words).
#define VOP2(nm, insn)                                   \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q2}\n"                          \
            :: "r"(a), "r"(b), "r"(r)                    \
            : "q0", "q1", "q2", "memory");               \
    }

// q2 = insn(q0); stores q2.
#define VOP1(nm, insn)                                   \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        (void)b;                                         \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q2}\n"                          \
            :: "r"(a), "r"(1), "r"(r)                    \
            : "q0", "q2", "memory");                     \
    }

// d4 = insn(...) using d-regs of q0/q1; stores d4 (2 words).
#define VOPD(nm, insn)                                   \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {d4}\n"                          \
            :: "r"(a), "r"(b), "r"(r)                    \
            : "q0", "q1", "d4", "memory");               \
    }

// d4 = insn(q0) narrowing; stores d4.
#define VOPNAR(nm, insn)                                 \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        (void)b;                                         \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {d4}\n"                          \
            :: "r"(a), "r"(1), "r"(r)                    \
            : "q0", "d4", "memory");                     \
    }

VOP2X2(vuzp_8_q, "vuzp.8 q0, q1")
VOP2X2(vuzp_16_q, "vuzp.16 q0, q1")
VOP2X2(vuzp_32_q, "vuzp.32 q0, q1")
VOP2X2D(vuzp_8_d, "vuzp.8 d0, d2")
VOP2X2D(vuzp_16_d, "vuzp.16 d0, d2")
VOP2X2D(vuzp_32_d, "vuzp.32 d0, d2")

VOP2X2(vzip_8_q, "vzip.8 q0, q1")
VOP2X2(vzip_16_q, "vzip.16 q0, q1")
VOP2X2(vzip_32_q, "vzip.32 q0, q1")
VOP2X2D(vzip_8_d, "vzip.8 d0, d2")
VOP2X2D(vzip_16_d, "vzip.16 d0, d2")
VOP2X2D(vzip_32_d, "vzip.32 d0, d2")

VOP2X2(vtrn_8_q, "vtrn.8 q0, q1")
VOP2X2(vtrn_16_q, "vtrn.16 q0, q1")
VOP2X2(vtrn_32_q, "vtrn.32 q0, q1")
VOP2X2D(vtrn_8_d, "vtrn.8 d0, d2")
VOP2X2D(vtrn_16_d, "vtrn.16 d0, d2")
VOP2X2D(vtrn_32_d, "vtrn.32 d0, d2")

VOP1(vrev16_8, "vrev16.8 q2, q0")
VOP1(vrev32_8, "vrev32.8 q2, q0")
VOP1(vrev32_16, "vrev32.16 q2, q0")
VOP1(vrev64_8, "vrev64.8 q2, q0")
VOP1(vrev64_16, "vrev64.16 q2, q0")
VOP1(vrev64_32, "vrev64.32 q2, q0")

VOP2(vext_8_q3, "vext.8 q2, q0, q1, #3")
VOP2(vext_8_q9, "vext.8 q2, q0, q1, #9")
VOPD(vext_8_d3, "vext.8 d4, d0, d2, #3")
VOPD(vext_8_d6, "vext.8 d4, d0, d2, #6")

VOP2(vqadd_s8, "vqadd.s8 q2, q0, q1")
VOP2(vqadd_s16, "vqadd.s16 q2, q0, q1")
VOP2(vqadd_u8, "vqadd.u8 q2, q0, q1")
VOP2(vqadd_u16, "vqadd.u16 q2, q0, q1")
VOP2(vqsub_s8, "vqsub.s8 q2, q0, q1")
VOP2(vqsub_s16, "vqsub.s16 q2, q0, q1")
VOP2(vqsub_u8, "vqsub.u8 q2, q0, q1")
VOP2(vqsub_u16, "vqsub.u16 q2, q0, q1")

VOP2(vabd_s8, "vabd.s8 q2, q0, q1")
VOP2(vabd_s16, "vabd.s16 q2, q0, q1")
VOP2(vabd_s32, "vabd.s32 q2, q0, q1")
VOP2(vabd_u8, "vabd.u8 q2, q0, q1")
VOP2(vabd_u16, "vabd.u16 q2, q0, q1")
VOP2(vabd_u32, "vabd.u32 q2, q0, q1")

VOPNAR(vqmovn_s16, "vqmovn.s16 d4, q0")
VOPNAR(vqmovn_s32, "vqmovn.s32 d4, q0")
VOPNAR(vqmovn_u16, "vqmovn.u16 d4, q0")
VOPNAR(vqmovn_u32, "vqmovn.u32 d4, q0")
VOPNAR(vqmovun_s16, "vqmovun.s16 d4, q0")
VOPNAR(vqmovun_s32, "vqmovun.s32 d4, q0")

VOP1(vclz_8, "vclz.i8 q2, q0")
VOP1(vclz_16, "vclz.i16 q2, q0")
VOP1(vclz_32, "vclz.i32 q2, q0")

VOP2(vshl_s8, "vshl.s8 q2, q0, q1")
VOP2(vshl_s16, "vshl.s16 q2, q0, q1")
VOP2(vshl_s32, "vshl.s32 q2, q0, q1")
VOP2(vshl_s64, "vshl.s64 q2, q0, q1")
VOP2(vshl_u8, "vshl.u8 q2, q0, q1")
VOP2(vshl_u16, "vshl.u16 q2, q0, q1")
VOP2(vshl_u32, "vshl.u32 q2, q0, q1")
VOP2(vshl_u64, "vshl.u64 q2, q0, q1")

// d4 = vtbl.8 {d0[,d1...]}, d2; indices from idx buffer.
#define VTBL(nm, regs, reglist)                          \
    static void op_##nm(const u32* tab, const u32* idx, u32* r) {        \
        __asm__ volatile(                                \
            "vldmia %0, {" reglist "}\n"                 \
            "vldmia %1, {d6}\n"                          \
            "vtbl.8 d4, {" reglist "}, d6\n"             \
            "vstmia %2, {d4}\n"                          \
            :: "r"(tab), "r"(idx), "r"(r)                \
            : "d0", "d1", "d2", "d3", "d4", "d6", "memory");             \
    }

VTBL(vtbl1, 1, "d0")
VTBL(vtbl2, 2, "d0, d1")
VTBL(vtbl4, 4, "d0, d1, d2, d3")

// d4 preloaded with defaults, then vtbx keeps defaults for OOR indices.
#define VTBX(nm, reglist)                                \
    static void op_##nm(const u32* tab, const u32* idx, const u32* def, u32* r) { \
        __asm__ volatile(                                \
            "vldmia %0, {" reglist "}\n"                 \
            "vldmia %1, {d6}\n"                          \
            "vldmia %2, {d4}\n"                          \
            "vtbx.8 d4, {" reglist "}, d6\n"             \
            "vstmia %3, {d4}\n"                          \
            :: "r"(tab), "r"(idx), "r"(def), "r"(r)      \
            : "d0", "d1", "d2", "d4", "d6", "memory");   \
    }

VTBX(vtbx2, "d0, d1")
VTBX(vtbx3, "d0, d1, d2")

#include "neon2_body.inc"

int guest_main(void) {
    test_body();
    OUT[0] = g_acc;
    *MAGIC_ADDR = 0x600DC0DEu;
#ifdef LOG_MIX
    OUT[3] = 0x90000;
    OUT[4] = (u32)g_log;
#endif
    return 0;
}
