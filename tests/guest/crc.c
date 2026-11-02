// CRC32 guest test: CRC32B/H/W and CRC32CB/H/W over assorted accumulator/data
// pairs, mixed into an FNV-1a checksum; crc_ref must produce the same value.

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define OUT ((volatile u32*)0x00080000)
#define MAGIC_ADDR ((volatile u32*)0x00080040)

static u32 g_acc = 2166136261u;
static void mix(u32 v) {
    g_acc = (g_acc ^ v) * 16777619u;
}

#define DEFCRC(nm, insn)                                                 \
    static u32 op_##nm(u32 a, u32 d) {                                   \
        u32 r;                                                           \
        __asm__ volatile(insn " %0, %1, %2" : "=r"(r) : "r"(a), "r"(d)); \
        return r;                                                        \
    }

DEFCRC(crc32b, "crc32b")
DEFCRC(crc32h, "crc32h")
DEFCRC(crc32w, "crc32w")
DEFCRC(crc32cb, "crc32cb")
DEFCRC(crc32ch, "crc32ch")
DEFCRC(crc32cw, "crc32cw")

static const u32 AV[] = {
    0x00000000u, 0xFFFFFFFFu, 0x12345678u, 0x9ABCDEF0u,
    0xA5A5A5A5u, 0x00000001u, 0x80000000u, 0xDEADBEEFu,
};
static const u32 BV[] = {
    0x00000000u, 0xFFFFFFFFu, 0x01020304u, 0xFEFF8081u,
    0x5A5A5A5Au, 0x80000000u, 0x00000001u, 0x0BADF00Du,
};
#define NV (sizeof(AV) / sizeof(AV[0]))

int guest_main(void) {
    for (u32 i = 0; i < NV; i++) {
        const u32 a = AV[i], b = BV[i];
        // chained: result feeds the next accumulator
        u32 c = op_crc32b(a, b);
        mix(c);
        c = op_crc32h(c, b);
        mix(c);
        c = op_crc32w(c, b);
        mix(c);
        u32 d = op_crc32cb(a, b);
        mix(d);
        d = op_crc32ch(d, b);
        mix(d);
        d = op_crc32cw(d, b);
        mix(d);
    }
    OUT[0] = g_acc;
    *MAGIC_ADDR = 0x600DC0DEu;
    return 0;
}
