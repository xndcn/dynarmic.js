// NEON integer guest test: vadd/vsub/logic/vmul/vmin/vmax/vceq/vcgt/vabs/vcnt/
// shifts/vhadd/vrhadd/vhsub/vpaddl/vmull/vmovn/vmovl/vpadd/vdup/lane access.
// All results are mixed into an FNV-1a checksum; neon_ref must produce the
// identical value. Vector results are stored to a buffer with vstmia and the
// words are mixed.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

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

// q2 = insn(q0, q1); stores q2 to r.
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

// q2 = insn(q0) unary.
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

// d4 = insn(q0) narrowing; stores d4 (8 bytes).
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

// q2 = insn(d0) widening.
#define VOPWID(nm, insn)                                 \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        (void)b;                                         \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q2}\n"                          \
            :: "r"(a), "r"(1), "r"(r)                    \
            : "q0", "q2", "memory");                     \
    }

// q2 = insn(d0, d2) widening multiply (low halves of q0/q1).
#define VOPMULL(nm, insn)                                \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q2}\n"                          \
            :: "r"(a), "r"(b), "r"(r)                    \
            : "q0", "q1", "q2", "memory");               \
    }

// d4 = insn(d0, d2) D-register paired add; stores d4.
#define VOPVPADD(nm, insn)                               \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {d4}\n"                          \
            :: "r"(a), "r"(b), "r"(r)                    \
            : "q0", "q1", "d4", "memory");               \
    }

VOP2(vadd_i8, "vadd.i8 q2, q0, q1")
VOP2(vadd_i16, "vadd.i16 q2, q0, q1")
VOP2(vadd_i32, "vadd.i32 q2, q0, q1")
VOP2(vadd_i64, "vadd.i64 q2, q0, q1")
VOP2(vsub_i8, "vsub.i8 q2, q0, q1")
VOP2(vsub_i16, "vsub.i16 q2, q0, q1")
VOP2(vsub_i32, "vsub.i32 q2, q0, q1")
VOP2(vsub_i64, "vsub.i64 q2, q0, q1")
VOP2(vand, "vand q2, q0, q1")
VOP2(vbic, "vbic q2, q0, q1")
VOP2(vorr, "vorr q2, q0, q1")
VOP2(veor, "veor q2, q0, q1")
VOP2(vmul_i8, "vmul.i8 q2, q0, q1")
VOP2(vmul_i16, "vmul.i16 q2, q0, q1")
VOP2(vmul_i32, "vmul.i32 q2, q0, q1")
VOP2(vmin_s8, "vmin.s8 q2, q0, q1")
VOP2(vmin_s16, "vmin.s16 q2, q0, q1")
VOP2(vmin_s32, "vmin.s32 q2, q0, q1")
VOP2(vmin_u8, "vmin.u8 q2, q0, q1")
VOP2(vmin_u16, "vmin.u16 q2, q0, q1")
VOP2(vmin_u32, "vmin.u32 q2, q0, q1")
VOP2(vmax_s8, "vmax.s8 q2, q0, q1")
VOP2(vmax_s16, "vmax.s16 q2, q0, q1")
VOP2(vmax_s32, "vmax.s32 q2, q0, q1")
VOP2(vmax_u8, "vmax.u8 q2, q0, q1")
VOP2(vmax_u16, "vmax.u16 q2, q0, q1")
VOP2(vmax_u32, "vmax.u32 q2, q0, q1")
VOP2(vceq_i8, "vceq.i8 q2, q0, q1")
VOP2(vceq_i16, "vceq.i16 q2, q0, q1")
VOP2(vceq_i32, "vceq.i32 q2, q0, q1")
VOP2(vcgt_s8, "vcgt.s8 q2, q0, q1")
VOP2(vcgt_s16, "vcgt.s16 q2, q0, q1")
VOP2(vcgt_s32, "vcgt.s32 q2, q0, q1")
VOP2(vhadd_s8, "vhadd.s8 q2, q0, q1")
VOP2(vhadd_s16, "vhadd.s16 q2, q0, q1")
VOP2(vhadd_s32, "vhadd.s32 q2, q0, q1")
VOP2(vhadd_u8, "vhadd.u8 q2, q0, q1")
VOP2(vhadd_u16, "vhadd.u16 q2, q0, q1")
VOP2(vhadd_u32, "vhadd.u32 q2, q0, q1")
VOP2(vrhadd_s8, "vrhadd.s8 q2, q0, q1")
VOP2(vrhadd_s16, "vrhadd.s16 q2, q0, q1")
VOP2(vrhadd_s32, "vrhadd.s32 q2, q0, q1")
VOP2(vrhadd_u8, "vrhadd.u8 q2, q0, q1")
VOP2(vrhadd_u16, "vrhadd.u16 q2, q0, q1")
VOP2(vrhadd_u32, "vrhadd.u32 q2, q0, q1")
VOP2(vhsub_s8, "vhsub.s8 q2, q0, q1")
VOP2(vhsub_s16, "vhsub.s16 q2, q0, q1")
VOP2(vhsub_s32, "vhsub.s32 q2, q0, q1")
VOP2(vhsub_u8, "vhsub.u8 q2, q0, q1")
VOP2(vhsub_u16, "vhsub.u16 q2, q0, q1")
VOP2(vhsub_u32, "vhsub.u32 q2, q0, q1")

VOP1(vabs_s8, "vabs.s8 q2, q0")
VOP1(vabs_s16, "vabs.s16 q2, q0")
VOP1(vabs_s32, "vabs.s32 q2, q0")
VOP1(vcnt_8, "vcnt.8 q2, q0")
VOP1(vmvn, "vmvn q2, q0")
VOP1(vpaddl_s8, "vpaddl.s8 q2, q0")
VOP1(vpaddl_s16, "vpaddl.s16 q2, q0")
VOP1(vpaddl_s32, "vpaddl.s32 q2, q0")
VOP1(vpaddl_u8, "vpaddl.u8 q2, q0")
VOP1(vpaddl_u16, "vpaddl.u16 q2, q0")
VOP1(vpaddl_u32, "vpaddl.u32 q2, q0")

VOPWID(vmovl_s8, "vmovl.s8 q2, d0")
VOPWID(vmovl_s16, "vmovl.s16 q2, d0")
VOPWID(vmovl_s32, "vmovl.s32 q2, d0")
VOPWID(vmovl_u8, "vmovl.u8 q2, d0")
VOPWID(vmovl_u16, "vmovl.u16 q2, d0")
VOPWID(vmovl_u32, "vmovl.u32 q2, d0")

VOPNAR(vmovn_i16, "vmovn.i16 d4, q0")
VOPNAR(vmovn_i32, "vmovn.i32 d4, q0")
VOPNAR(vmovn_i64, "vmovn.i64 d4, q0")

VOPMULL(vmull_s8, "vmull.s8 q2, d0, d2")
VOPMULL(vmull_s16, "vmull.s16 q2, d0, d2")
VOPMULL(vmull_s32, "vmull.s32 q2, d0, d2")
VOPMULL(vmull_u8, "vmull.u8 q2, d0, d2")
VOPMULL(vmull_u16, "vmull.u16 q2, d0, d2")
VOPMULL(vmull_u32, "vmull.u32 q2, d0, d2")

VOPVPADD(vpadd_i8, "vpadd.i8 d4, d0, d2")
VOPVPADD(vpadd_i16, "vpadd.i16 d4, d0, d2")
VOPVPADD(vpadd_i32, "vpadd.i32 d4, d0, d2")

// immediate shifts
#define VSHF(nm, insn)                                   \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        (void)b;                                         \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q2}\n"                          \
            :: "r"(a), "r"(1), "r"(r)                    \
            : "q0", "q2", "memory");                     \
    }

VSHF(vshl_i8_3, "vshl.i8 q2, q0, #3")
VSHF(vshl_i16_9, "vshl.i16 q2, q0, #9")
VSHF(vshl_i32_31, "vshl.i32 q2, q0, #31")
VSHF(vshl_i64_1, "vshl.i64 q2, q0, #1")
VSHF(vshl_i64_63, "vshl.i64 q2, q0, #63")
VSHF(vshr_u8_1, "vshr.u8 q2, q0, #1")
VSHF(vshr_u8_7, "vshr.u8 q2, q0, #7")
VSHF(vshr_u16_15, "vshr.u16 q2, q0, #15")
VSHF(vshr_u32_1, "vshr.u32 q2, q0, #1")
VSHF(vshr_u64_40, "vshr.u64 q2, q0, #40")
VSHF(vshr_s8_5, "vshr.s8 q2, q0, #5")
VSHF(vshr_s16_1, "vshr.s16 q2, q0, #1")
VSHF(vshr_s32_31, "vshr.s32 q2, q0, #31")
VSHF(vshr_s64_62, "vshr.s64 q2, q0, #62")

// broadcasts from GPR
#define VDUP(nm, insn)                                   \
    static void op_##nm(u32 x, u32* r) {                 \
        __asm__ volatile(                                \
            insn "\n"                                    \
            "vstmia %0, {q2}\n"                          \
            :: "r"(r), "r"(x)                            \
            : "q2", "memory");                           \
    }

VDUP(vdup_8, "vdup.8 q2, %1")
VDUP(vdup_16, "vdup.16 q2, %1")
VDUP(vdup_32, "vdup.32 q2, %1")

// broadcasts from a vector element
#define VDUPE(nm, insn)                                  \
    static void op_##nm(const u32* a, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %1, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %0, {q2}\n"                          \
            :: "r"(r), "r"(a)                            \
            : "q0", "q2", "memory");                     \
    }

VDUPE(vdup_e16_l2, "vdup.16 q2, d0[2]")
VDUPE(vdup_e32_l1, "vdup.32 q2, d0[1]")

// lane extracts (zero-extended to GPR)
#define VGET(nm, insn)                                   \
    static u32 op_##nm(const u32* a) {                   \
        u32 v;                                           \
        __asm__ volatile(                                \
            "vldmia %1, {q0}\n"                          \
            insn                                         \
            : "=r"(v)                                    \
            : "r"(a)                                     \
            : "q0");                                     \
        return v;                                        \
    }

VGET(get8_d0l3, "vmov.u8 %0, d0[3]")
VGET(get16_d1l2, "vmov.u16 %0, d1[2]")
VGET(get32_d0l1, "vmov.32 %0, d0[1]")

// lane inserts
#define VSET(nm, insn)                                   \
    static void op_##nm(const u32* a, u32 v, u32* r) {   \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {q0}\n"                          \
            :: "r"(a), "r"(v), "r"(r)                    \
            : "q0", "memory");                           \
    }

VSET(set8_d0l5, "vmov.8 d0[5], %1")
VSET(set16_d1l2, "vmov.16 d1[2], %1")
VSET(set32_d0l1, "vmov.32 d0[1], %1")

// 64-bit lane get/set (two GPRs <-> D register)
static void op_get64_d1(const u32* a, u32* r) {
    u32 lo, hi;
    __asm__ volatile(
        "vldmia %2, {q0}\n"
        "vmov %0, %1, d1"
        : "=r"(lo), "=r"(hi)
        : "r"(a)
        : "q0");
    r[0] = lo;
    r[1] = hi;
}

static void op_set64_d1(const u32* a, u32 lo, u32 hi, u32* r) {
    __asm__ volatile(
        "vldmia %0, {q0}\n"
        "vmov d1, %1, %2\n"
        "vstmia %3, {q0}\n"
        :: "r"(a), "r"(lo), "r"(hi), "r"(r)
        : "q0", "memory");
}

#include "neon_body.inc"

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
