// NEON floating-point guest test (tranche 5): vadd/vsub/vmul.f32,
// vabs/vneg, vceq/vcgt/vcge/vcage/vcagt, vmin/vmax, vabd, vpadd.f32,
// fixed-point vcvt (both directions), vrecpe/vrsqrte/vrecps/vrsqrts.f32,
// vcvt.f32.f16 / vcvt.f16.f32. (Vector vdiv/vsqrt are A64-only.)
// Results mix into an FNV-1a checksum; neon4_ref must produce the same value.

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

// d4 = insn(d0, d2); stores d4 (2 words).
#define VOP2D(nm, insn)                                  \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {d4}\n"                          \
            :: "r"(a), "r"(b), "r"(r)                    \
            : "q0", "q1", "d4", "memory");               \
    }

// d4 = insn(q0); stores d4 (2 words).
#define VOP1D(nm, insn)                                  \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        (void)b;                                         \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            insn "\n"                                    \
            "vstmia %2, {d4}\n"                          \
            :: "r"(a), "r"(1), "r"(r)                    \
            : "q0", "d4", "memory");                     \
    }

VOP2(vadd_f32, "vadd.f32 q2, q0, q1")
VOP2(vsub_f32, "vsub.f32 q2, q0, q1")
VOP2(vmul_f32, "vmul.f32 q2, q0, q1")
VOP2(vabd_f32, "vabd.f32 q2, q0, q1")

VOP1(vabs_f32, "vabs.f32 q2, q0")
VOP1(vneg_f32, "vneg.f32 q2, q0")

VOP2(vceq_f32, "vceq.f32 q2, q0, q1")
VOP2(vcgt_f32, "vcgt.f32 q2, q0, q1")
VOP2(vcge_f32, "vcge.f32 q2, q0, q1")
VOP2(vcage_f32, "vacge.f32 q2, q0, q1")
VOP2(vcagt_f32, "vacgt.f32 q2, q0, q1")

VOP2(vmin_f32, "vmin.f32 q2, q0, q1")
VOP2(vmax_f32, "vmax.f32 q2, q0, q1")

VOP2D(vpadd_f32, "vpadd.f32 d4, d0, d2")

// fixed-point conversions (immediate fraction bits)
VOP1(vcvt_s32_f32_3, "vcvt.s32.f32 q2, q0, #3")
VOP1(vcvt_u32_f32_7, "vcvt.u32.f32 q2, q0, #7")
VOP1(vcvt_f32_s32_5, "vcvt.f32.s32 q2, q0, #5")
VOP1(vcvt_f32_u32_11, "vcvt.f32.u32 q2, q0, #11")
// integer conversions (no fraction bits; vcvt truncates)
VOP1(vcvt_s32_f32, "vcvt.s32.f32 q2, q0")
VOP1(vcvt_u32_f32, "vcvt.u32.f32 q2, q0")
VOP1(vcvt_f32_s32, "vcvt.f32.s32 q2, q0")
VOP1(vcvt_f32_u32, "vcvt.f32.u32 q2, q0")

// reciprocal estimates and steps (reference LUT semantics via thunks)
VOP1(vrecpe_f32, "vrecpe.f32 q2, q0")
VOP1(vrsqrte_f32, "vrsqrte.f32 q2, q0")
VOP2(vrecps_f32, "vrecps.f32 q2, q0, q1")
VOP2(vrsqrts_f32, "vrsqrts.f32 q2, q0, q1")

// half-precision vector conversions
VOP1(vcvt_f32_f16, "vcvt.f32.f16 q2, d0")
VOP1D(vcvt_f16_f32, "vcvt.f16.f32 d4, q0")

// Thunk-based ops (vrecpe/vrsqrte/vrecps/vrsqrts/vcvt-f16) fold FP exception
// bits into FPSCR; observe the accumulator once after all of them.
#define FPSCR_MIX()                                             \
    do {                                                        \
        u32 fpscr_;                                             \
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr_));      \
        mix(fpscr_);                                            \
    } while (0)

#include "neon4_body.inc"

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
