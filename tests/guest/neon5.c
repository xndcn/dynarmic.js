// ARMv8 NEON/VFP floating-point guest test (tranche 5, second half):
// vfma/vfms.f32, vrintn/a/p/m/z.f32, vminnm/vmaxnm.f32 (vector and scalar),
// vcvtn/vcvta/vcvtp/vcvtm .s32/.u32.f32 (rounding-mode vector conversions).
// Results mix into an FNV-1a checksum; neon5_ref must produce the same value.

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

// q2 (+)= insn(q0, q1): three-operand fused form; stores q2.
#define VOP3(nm, insn)                                   \
    static void op_##nm(const u32* n, const u32* m, const u32* c, u32* r) { \
        __asm__ volatile(                                \
            "vldmia %0, {q0}\n"                          \
            "vldmia %1, {q1}\n"                          \
            "vldmia %2, {q2}\n"                          \
            insn "\n"                                    \
            "vstmia %3, {q2}\n"                          \
            :: "r"(n), "r"(m), "r"(c), "r"(r)            \
            : "q0", "q1", "q2", "memory");               \
    }

// s4 = insn(s0, s2); stores one word.
#define SOP2(nm, insn)                                   \
    static void op_##nm(const u32* a, const u32* b, u32* r) {          \
        __asm__ volatile(                                \
            "vldr s0, [%0]\n"                            \
            "vldr s2, [%1]\n"                            \
            insn "\n"                                    \
            "vstr s4, [%2]\n"                            \
            :: "r"(a), "r"(b), "r"(r)                    \
            : "s0", "s2", "s4", "memory");               \
    }

VOP3(vfma_f32, "vfma.f32 q2, q0, q1")
VOP3(vfms_f32, "vfms.f32 q2, q0, q1")

VOP1(vrintn_f32, "vrintn.f32 q2, q0")
VOP1(vrinta_f32, "vrinta.f32 q2, q0")
VOP1(vrintp_f32, "vrintp.f32 q2, q0")
VOP1(vrintm_f32, "vrintm.f32 q2, q0")
VOP1(vrintz_f32, "vrintz.f32 q2, q0")

VOP2(vminnm_f32, "vminnm.f32 q2, q0, q1")
VOP2(vmaxnm_f32, "vmaxnm.f32 q2, q0, q1")

VOP1(vcvtn_s32_f32, "vcvtn.s32.f32 q2, q0")
VOP1(vcvtn_u32_f32, "vcvtn.u32.f32 q2, q0")
VOP1(vcvta_s32_f32, "vcvta.s32.f32 q2, q0")
VOP1(vcvta_u32_f32, "vcvta.u32.f32 q2, q0")
VOP1(vcvtp_s32_f32, "vcvtp.s32.f32 q2, q0")
VOP1(vcvtp_u32_f32, "vcvtp.u32.f32 q2, q0")
VOP1(vcvtm_s32_f32, "vcvtm.s32.f32 q2, q0")
VOP1(vcvtm_u32_f32, "vcvtm.u32.f32 q2, q0")

SOP2(vminnm_s, "vminnm.f32 s4, s0, s2")
SOP2(vmaxnm_s, "vmaxnm.f32 s4, s0, s2")

#include "neon5_body.inc"

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
