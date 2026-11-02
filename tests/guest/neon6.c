// 64->32 saturating narrows: vqmovn.s64, vqmovn.u64, vqmovun.s64.
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

// d4 = insn(q0); stores d4 (2 words).
#define VOP1D(nm, insn)                                        \
    static void op_##nm(const u32* a, u32* r) {                \
        __asm__ volatile("vldmia %0, {q0}\n" insn "\n vstmia %1, {d4}\n" \
                         :: "r"(a), "r"(r) : "q0", "d4", "memory"); \
    }

VOP1D(vqmovn_s64, "vqmovn.s64 d4, q0")
VOP1D(vqmovn_u64, "vqmovn.u64 d4, q0")
VOP1D(vqmovun_s64, "vqmovun.s64 d4, q0")

#include "neon6_body.inc"

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
