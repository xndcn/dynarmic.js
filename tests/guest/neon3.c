// NEON integer guest test, tranche 3: vqadd/vqsub 32-bit lanes, vqabs/vqneg,
// vqshl (register and immediate), vqshlu, vrshl, vqdmulh/vqrdmulh/vqdmull,
// vmul.p8/vmull.p8, vrecpe.u32/vrsqrte.u32.
// Results mix into an FNV-1a checksum; neon3_ref must produce the same value.

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

VOP2(vqadd_s32, "vqadd.s32 q2, q0, q1")
VOP2(vqadd_u32, "vqadd.u32 q2, q0, q1")
VOP2(vqsub_s32, "vqsub.s32 q2, q0, q1")
VOP2(vqsub_u32, "vqsub.u32 q2, q0, q1")

VOP1(vqabs_s8, "vqabs.s8 q2, q0")
VOP1(vqabs_s16, "vqabs.s16 q2, q0")
VOP1(vqabs_s32, "vqabs.s32 q2, q0")
VOP1(vqneg_s8, "vqneg.s8 q2, q0")
VOP1(vqneg_s16, "vqneg.s16 q2, q0")
VOP1(vqneg_s32, "vqneg.s32 q2, q0")

// register vqshl: q0 data, q1 amounts (low byte of each amount lane)
VOP2(vqshl_s8, "vqshl.s8 q2, q0, q1")
VOP2(vqshl_s16, "vqshl.s16 q2, q0, q1")
VOP2(vqshl_s32, "vqshl.s32 q2, q0, q1")
VOP2(vqshl_s64, "vqshl.s64 q2, q0, q1")
VOP2(vqshl_u8, "vqshl.u8 q2, q0, q1")
VOP2(vqshl_u16, "vqshl.u16 q2, q0, q1")
VOP2(vqshl_u32, "vqshl.u32 q2, q0, q1")
VOP2(vqshl_u64, "vqshl.u64 q2, q0, q1")

// immediate vqshl
VOP1(vqshli_s8_3, "vqshl.s8 q2, q0, #3")
VOP1(vqshli_s16_9, "vqshl.s16 q2, q0, #9")
VOP1(vqshli_s32_20, "vqshl.s32 q2, q0, #20")
VOP1(vqshli_s64_40, "vqshl.s64 q2, q0, #40")
VOP1(vqshli_u8_5, "vqshl.u8 q2, q0, #5")
VOP1(vqshli_u16_13, "vqshl.u16 q2, q0, #13")
VOP1(vqshli_u32_31, "vqshl.u32 q2, q0, #31")
VOP1(vqshli_u64_63, "vqshl.u64 q2, q0, #63")

// vqshlu (signed -> unsigned saturating, immediate only)
VOP1(vqshlu_s8_5, "vqshlu.s8 q2, q0, #5")
VOP1(vqshlu_s16_13, "vqshlu.s16 q2, q0, #13")
VOP1(vqshlu_s32_1, "vqshlu.s32 q2, q0, #1")
VOP1(vqshlu_s64_63, "vqshlu.s64 q2, q0, #63")

// register vrshl: q0 data, q1 amounts
VOP2(vrshl_s8, "vrshl.s8 q2, q0, q1")
VOP2(vrshl_s16, "vrshl.s16 q2, q0, q1")
VOP2(vrshl_s32, "vrshl.s32 q2, q0, q1")
VOP2(vrshl_s64, "vrshl.s64 q2, q0, q1")
VOP2(vrshl_u8, "vrshl.u8 q2, q0, q1")
VOP2(vrshl_u16, "vrshl.u16 q2, q0, q1")
VOP2(vrshl_u32, "vrshl.u32 q2, q0, q1")
VOP2(vrshl_u64, "vrshl.u64 q2, q0, q1")

VOP2(vqdmulh_s16, "vqdmulh.s16 q2, q0, q1")
VOP2(vqdmulh_s32, "vqdmulh.s32 q2, q0, q1")
VOP2(vqrdmulh_s16, "vqrdmulh.s16 q2, q0, q1")
VOP2(vqrdmulh_s32, "vqrdmulh.s32 q2, q0, q1")

// long ops take D regs (low halves of q0/q1), produce Q

VOP2(vmul_p8, "vmul.p8 q2, q0, q1")
VOP2(vmull_p8, "vmull.p8 q2, d0, d2")

VOP1(vrecpe_u32, "vrecpe.u32 q2, q0")
VOP1(vrsqrte_u32, "vrsqrte.u32 q2, q0")

#include "neon3_body.inc"

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
