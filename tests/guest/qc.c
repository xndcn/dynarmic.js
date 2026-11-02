// FPSCR.QC test: every NEON saturating op over a boundary-heavy grid.
#include <stdint.h>
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
#define DEF(nm, insn)                                                                \
    static u32 op_##nm(const u32* a, const u32* b, u32* r) {                        \
        u32 f;                                                                       \
        __asm__ volatile("vmrs %0, fpscr\n bic %0, %0, #0x08000000\n vmsr fpscr, %0\n" \
                         "vldmia %1, {q0}\n vldmia %2, {q1}\n vmov.i32 q2, #0\n " insn "\n" \
                         "vstmia %3, {q2}\n vmrs %0, fpscr\n"                          \
                         : "=&r"(f) : "r"(a), "r"(b), "r"(r) : "q0", "q1", "q2", "memory", "cc"); \
        return f;                                                                    \
    }

DEF(qadd_s8, "vqadd.s8 q2, q0, q1")
DEF(qadd_s16, "vqadd.s16 q2, q0, q1")
DEF(qadd_s32, "vqadd.s32 q2, q0, q1")
DEF(qadd_u8, "vqadd.u8 q2, q0, q1")
DEF(qadd_u16, "vqadd.u16 q2, q0, q1")
DEF(qadd_u32, "vqadd.u32 q2, q0, q1")
DEF(qsub_s8, "vqsub.s8 q2, q0, q1")
DEF(qsub_s16, "vqsub.s16 q2, q0, q1")
DEF(qsub_s32, "vqsub.s32 q2, q0, q1")
DEF(qsub_u8, "vqsub.u8 q2, q0, q1")
DEF(qsub_u16, "vqsub.u16 q2, q0, q1")
DEF(qsub_u32, "vqsub.u32 q2, q0, q1")
DEF(qabs_s8, "vqabs.s8 q2, q0")
DEF(qabs_s16, "vqabs.s16 q2, q0")
DEF(qabs_s32, "vqabs.s32 q2, q0")
DEF(qneg_s8, "vqneg.s8 q2, q0")
DEF(qneg_s16, "vqneg.s16 q2, q0")
DEF(qneg_s32, "vqneg.s32 q2, q0")
DEF(qdmulh_s16, "vqdmulh.s16 q2, q0, q1")
DEF(qdmulh_s32, "vqdmulh.s32 q2, q0, q1")
DEF(qrdmulh_s16, "vqrdmulh.s16 q2, q0, q1")
DEF(qrdmulh_s32, "vqrdmulh.s32 q2, q0, q1")
DEF(qdmull_s16, "vqdmull.s16 q2, d0, d2[1]")
DEF(qdmull_s32, "vqdmull.s32 q2, d0, d2[1]")
DEF(qshl_s8, "vqshl.s8 q2, q0, q1")
DEF(qshl_s16, "vqshl.s16 q2, q0, q1")
DEF(qshl_s32, "vqshl.s32 q2, q0, q1")
DEF(qshl_s64, "vqshl.s64 q2, q0, q1")
DEF(qshl_u8, "vqshl.u8 q2, q0, q1")
DEF(qshl_u16, "vqshl.u16 q2, q0, q1")
DEF(qshl_u32, "vqshl.u32 q2, q0, q1")
DEF(qshl_u64, "vqshl.u64 q2, q0, q1")
DEF(qshli_s8, "vqshl.s8 q2, q0, #3")
DEF(qshli_s16, "vqshl.s16 q2, q0, #5")
DEF(qshli_s32, "vqshl.s32 q2, q0, #9")
DEF(qshli_s64, "vqshl.s64 q2, q0, #17")
DEF(qshli_u8, "vqshl.u8 q2, q0, #3")
DEF(qshli_u16, "vqshl.u16 q2, q0, #5")
DEF(qshli_u32, "vqshl.u32 q2, q0, #9")
DEF(qshli_u64, "vqshl.u64 q2, q0, #17")
DEF(qshlu_s8, "vqshlu.s8 q2, q0, #3")
DEF(qshlu_s16, "vqshlu.s16 q2, q0, #5")
DEF(qshlu_s32, "vqshlu.s32 q2, q0, #9")
DEF(qshlu_s64, "vqshlu.s64 q2, q0, #17")
DEF(qmovn_s16, "vqmovn.s16 d4, q0")
DEF(qmovn_s32, "vqmovn.s32 d4, q0")
DEF(qmovn_s64, "vqmovn.s64 d4, q0")
DEF(qmovn_u16, "vqmovn.u16 d4, q0")
DEF(qmovn_u32, "vqmovn.u32 d4, q0")
DEF(qmovn_u64, "vqmovn.u64 d4, q0")
DEF(qmovun_s16, "vqmovun.s16 d4, q0")
DEF(qmovun_s32, "vqmovun.s32 d4, q0")
DEF(qmovun_s64, "vqmovun.s64 d4, q0")

#include "qc_body.inc"
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
