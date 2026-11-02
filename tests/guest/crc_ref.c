// Host reference for crc.c: table-free bitwise CRC32, must produce the
// identical FNV-1a checksum.

#include <stdint.h>
#include <stdio.h>

typedef uint32_t u32;

static u32 g_acc = 2166136261u;
static void mix(u32 v) {
    g_acc = (g_acc ^ v) * 16777619u;
}

static u32 crc_bits(u32 crc, u32 poly, int bits) {
    for (int i = 0; i < bits; i++)
        crc = (crc >> 1) ^ (poly * (crc & 1));
    return crc;
}

static u32 op_crc32b(u32 a, u32 d) { return crc_bits(a ^ (d & 0xFF), 0xEDB88320u, 8); }
static u32 op_crc32h(u32 a, u32 d) { return crc_bits(a ^ (d & 0xFFFF), 0xEDB88320u, 16); }
static u32 op_crc32w(u32 a, u32 d) { return crc_bits(a ^ d, 0xEDB88320u, 32); }
static u32 op_crc32cb(u32 a, u32 d) { return crc_bits(a ^ (d & 0xFF), 0x82F63B78u, 8); }
static u32 op_crc32ch(u32 a, u32 d) { return crc_bits(a ^ (d & 0xFFFF), 0x82F63B78u, 16); }
static u32 op_crc32cw(u32 a, u32 d) { return crc_bits(a ^ d, 0x82F63B78u, 32); }

static const u32 AV[] = {
    0x00000000u, 0xFFFFFFFFu, 0x12345678u, 0x9ABCDEF0u,
    0xA5A5A5A5u, 0x00000001u, 0x80000000u, 0xDEADBEEFu,
};
static const u32 BV[] = {
    0x00000000u, 0xFFFFFFFFu, 0x01020304u, 0xFEFF8081u,
    0x5A5A5A5Au, 0x80000000u, 0x00000001u, 0x0BADF00Du,
};
#define NV (sizeof(AV) / sizeof(AV[0]))

int main(void) {
    for (u32 i = 0; i < NV; i++) {
        const u32 a = AV[i], b = BV[i];
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
    printf("checksum %08x\n", g_acc);
    return 0;
}
