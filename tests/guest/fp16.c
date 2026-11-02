// FP16 conversion guest test: VCVTB/VCVTT f16<->f32/f64 over assorted bit
// patterns, mixed into an FNV-1a checksum; fp16_ref must produce the same
// value. Also mixes the accumulated FPSR exception bits (mask 0x9F).

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

// All conversions go through fixed fp regs s2/d4; clobbers listed.
static u32 h2s_b(u32 h) {
    u32 r;
    __asm__ volatile("vmov s2, %1\n\t"
                     "vcvtb.f32.f16 s2, s2\n\t"
                     "vmov %0, s2\n\t"
                     : "=r"(r)
                     : "r"(h)
                     : "s2");
    return r;
}
static u32 h2s_t(u32 h) {
    u32 r;
    __asm__ volatile("vmov s2, %1\n\t"
                     "vcvtt.f32.f16 s2, s2\n\t"
                     "vmov %0, s2\n\t"
                     : "=r"(r)
                     : "r"(h << 16)
                     : "s2");
    return r;
}
static u32 s2h_b(u32 f) {
    u32 r;
    __asm__ volatile("vmov s2, %1\n\t"
                     "vcvtb.f16.f32 s2, s2\n\t"
                     "vmov %0, s2\n\t"
                     : "=r"(r)
                     : "r"(f)
                     : "s2");
    return r;
}
static u32 s2h_t(u32 f) {
    u32 r;
    __asm__ volatile("vmov s2, %1\n\t"
                     "vcvtt.f16.f32 s2, s2\n\t"
                     "vmov %0, s2\n\t"
                     : "=r"(r)
                     : "r"(f)
                     : "s2");
    return r;
}
static u64 h2d_b(u32 h) {
    u32 lo, hi;
    __asm__ volatile("vmov s2, %2\n\t"
                     "vcvtb.f64.f16 d4, s2\n\t"
                     "vmov %0, %1, d4\n\t"
                     : "=r"(lo), "=r"(hi)
                     : "r"(h)
                     : "s2", "d4");
    return ((u64)hi << 32) | lo;
}
static u64 h2d_t(u32 h) {
    u32 lo, hi;
    __asm__ volatile("vmov s2, %2\n\t"
                     "vcvtt.f64.f16 d4, s2\n\t"
                     "vmov %0, %1, d4\n\t"
                     : "=r"(lo), "=r"(hi)
                     : "r"(h << 16)
                     : "s2", "d4");
    return ((u64)hi << 32) | lo;
}
static u32 d2h_b(u64 d) {
    u32 r;
    __asm__ volatile("vmov d4, %1, %2\n\t"
                     "vmov s2, %1\n\t"
                     "vcvtb.f16.f64 s2, d4\n\t"
                     "vmov %0, s2\n\t"
                     : "=r"(r)
                     : "r"((u32)d), "r"((u32)(d >> 32))
                     : "s2", "d4");
    return r;
}
static u32 d2h_t(u64 d) {
    u32 r;
    __asm__ volatile("vmov d4, %1, %2\n\t"
                     "vmov s2, %1\n\t"
                     "vcvtt.f16.f64 s2, d4\n\t"
                     "vmov %0, s2\n\t"
                     : "=r"(r)
                     : "r"((u32)d), "r"((u32)(d >> 32))
                     : "s2", "d4");
    return r;
}

static const u32 HV[] = {
    0x0000u, 0x8000u, 0x0001u, 0x03FFu, 0x0400u, 0x3C00u,
    0xC000u, 0x3555u, 0x7BFFu, 0x7C00u, 0xFC00u, 0x7E00u,
};
static const u32 FV[] = {
    0x00000000u, 0x80000000u, 0x3F800000u, 0xBF800000u,
    0x00000001u, 0x33800000u, 0x3F800001u, 0x3EAAAAABu,
    0x477FE000u, 0x477FF000u, 0x7F800000u, 0xFF800000u,
    0x7FC00000u, 0x7F800001u,
};
static const u64 DV[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL,
    0x3FF0000000000000ULL, 0xBFF0000000000000ULL,
    0x0000000000000001ULL, 0x3FF0000000000001ULL,
    0x3FD5555555555555ULL, 0x40EFFFC000000000ULL,
    0x40F0000000000000ULL, 0x7FF0000000000000ULL,
    0xFFF0000000000000ULL, 0x7FF8000000000000ULL,
    0x7FF0000000000001ULL,
};
#define NHV (sizeof(HV) / sizeof(HV[0]))
#define NFV (sizeof(FV) / sizeof(FV[0]))
#define NDV (sizeof(DV) / sizeof(DV[0]))

static u32 read_fpsr(void) {
    u32 r;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(r));
    return r;
}

int guest_main(void) {
    for (u32 i = 0; i < NHV; i++) {
        mix(h2s_b(HV[i]));
        mix(h2s_t(HV[i]));
        const u64 d0 = h2d_b(HV[i]);
        mix((u32)d0);
        mix((u32)(d0 >> 32));
        const u64 d1 = h2d_t(HV[i]);
        mix((u32)d1);
        mix((u32)(d1 >> 32));
    }
    for (u32 i = 0; i < NFV; i++) {
        mix(s2h_b(FV[i]));
        mix(s2h_t(FV[i]));
    }
    for (u32 i = 0; i < NDV; i++) {
        mix(d2h_b(DV[i]));
        mix(d2h_t(DV[i]));
    }
    mix(read_fpsr() & 0x9F);
    OUT[0] = g_acc;
    *MAGIC_ADDR = 0x600DC0DEu;
#ifdef LOG_MIX
    OUT[3] = 0x90000;
    OUT[4] = (u32)g_log;
#endif
    return 0;
}
