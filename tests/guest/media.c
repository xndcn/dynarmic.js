// Media/saturation guest test: saturating scalar ops (QADD/SSAT/USAT...),
// parallel media ops (SADD8/SASX/SHSUB16/...), packed saturating ops,
// USAD8/USADA8 and SEL. Mixes all results + GE/Q flags into an FNV-1a
// checksum; media_ref must produce the identical value.

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

static u32 read_q(void) {
    u32 c;
    __asm__ volatile("mrs %0, cpsr" : "=r"(c));
    return (c >> 27) & 1;
}
static u32 read_ge(void) {
    u32 c;
    __asm__ volatile("mrs %0, cpsr" : "=r"(c));
    return (c >> 16) & 0xF;
}

#define DEF2(nm, insn)                                                   \
    static u32 op_##nm(u32 a, u32 b) {                                   \
        u32 r;                                                           \
        __asm__ volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(b)); \
        return r;                                                        \
    }

DEF2(qadd, "qadd")
DEF2(qsub, "qsub")
DEF2(qdadd, "qdadd")
DEF2(qdsub, "qdsub")
DEF2(sadd8, "sadd8")
DEF2(sadd16, "sadd16")
DEF2(ssub8, "ssub8")
DEF2(ssub16, "ssub16")
DEF2(uadd8, "uadd8")
DEF2(uadd16, "uadd16")
DEF2(usub8, "usub8")
DEF2(usub16, "usub16")
DEF2(sasx, "sasx")
DEF2(ssax, "ssax")
DEF2(uasx, "uasx")
DEF2(usax, "usax")
DEF2(shadd8, "shadd8")
DEF2(shadd16, "shadd16")
DEF2(shsub8, "shsub8")
DEF2(shsub16, "shsub16")
DEF2(uhadd8, "uhadd8")
DEF2(uhadd16, "uhadd16")
DEF2(uhsub8, "uhsub8")
DEF2(uhsub16, "uhsub16")
DEF2(shasx, "shasx")
DEF2(shsax, "shsax")
DEF2(uhasx, "uhasx")
DEF2(uhsax, "uhsax")
DEF2(qadd8, "qadd8")
DEF2(qadd16, "qadd16")
DEF2(qsub8, "qsub8")
DEF2(qsub16, "qsub16")
DEF2(uqadd8, "uqadd8")
DEF2(uqadd16, "uqadd16")
DEF2(uqsub8, "uqsub8")
DEF2(uqsub16, "uqsub16")
DEF2(usad8, "usad8")
DEF2(sel, "sel")

#define DEF_SSAT(nm, insn)                                        \
    static u32 op_##nm(u32 a, u32 n) {                            \
        u32 r;                                                    \
        switch (n) {                                              \
        case 1: __asm__ volatile(insn " %0, #1, %1" : "=r"(r) : "r"(a)); break;   \
        case 8: __asm__ volatile(insn " %0, #8, %1" : "=r"(r) : "r"(a)); break;   \
        case 12: __asm__ volatile(insn " %0, #12, %1" : "=r"(r) : "r"(a)); break; \
        case 16: __asm__ volatile(insn " %0, #16, %1" : "=r"(r) : "r"(a)); break; \
        case 31: __asm__ volatile(insn " %0, #31, %1" : "=r"(r) : "r"(a)); break; \
        case 32: __asm__ volatile(insn " %0, #32, %1" : "=r"(r) : "r"(a)); break; \
        default: r = 0xDEAD0000u | n; break;                      \
        }                                                         \
        return r;                                                 \
    }

DEF_SSAT(ssat, "ssat")
DEF_SSAT(usat, "usat")

static u32 op_ssat_lsl(u32 a, u32 n, u32 s) {
    u32 r;
    (void)s;  // fixed shift 5
    switch (n) {
    case 12: __asm__ volatile("ssat %0, #12, %1, lsl #5" : "=r"(r) : "r"(a)); break;
    default: r = 0xDEAD1111u; break;
    }
    return r;
}
static u32 op_ssat_asr(u32 a, u32 n, u32 s) {
    u32 r;
    (void)s;  // fixed shift 7
    switch (n) {
    case 16: __asm__ volatile("ssat %0, #16, %1, asr #7" : "=r"(r) : "r"(a)); break;
    default: r = 0xDEAD2222u; break;
    }
    return r;
}
static u32 op_usat_asr(u32 a, u32 n, u32 s) {
    u32 r;
    (void)s;
    switch (n) {
    case 16: __asm__ volatile("usat %0, #16, %1, asr #7" : "=r"(r) : "r"(a)); break;
    default: r = 0xDEAD3333u; break;
    }
    return r;
}

#define DEF_SAT16(nm, insn)                                       \
    static u32 op_##nm(u32 a, u32 n) {                            \
        u32 r;                                                    \
        switch (n) {                                              \
        case 1: __asm__ volatile(insn " %0, #1, %1" : "=r"(r) : "r"(a)); break;   \
        case 9: __asm__ volatile(insn " %0, #9, %1" : "=r"(r) : "r"(a)); break;   \
        case 15: __asm__ volatile(insn " %0, #15, %1" : "=r"(r) : "r"(a)); break; \
        case 16: __asm__ volatile(insn " %0, #16, %1" : "=r"(r) : "r"(a)); break; \
        default: r = 0xDEAD4444u | n; break;                      \
        }                                                         \
        return r;                                                 \
    }

DEF_SAT16(ssat16, "ssat16")
DEF_SAT16(usat16, "usat16")

static u32 op_usada8(u32 a, u32 b, u32 c) {
    u32 r;
    __asm__ volatile("usada8 %0, %1, %2, %3" : "=r"(r) : "r"(a), "r"(b), "r"(c));
    return r;
}

#include "media_body.inc"

int guest_main(void) {
    test_body();
    OUT[0] = g_acc;
    OUT[1] = read_q();
    OUT[2] = read_ge();
    *MAGIC_ADDR = 0x600DC0DEu;
#ifdef LOG_MIX
    OUT[3] = 0x90000;
    OUT[4] = (u32)g_log;
#endif
    return 0;
}
