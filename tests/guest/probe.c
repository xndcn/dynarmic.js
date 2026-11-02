// Probe: exercise suspect ops with known inputs, store results to 0x80000+.

#include <stdint.h>

volatile uint32_t* out = (volatile uint32_t*)0x80000;

static uint32_t rotl7(uint32_t x) { return (x << 7) | (x >> 25); }
static uint32_t clz1(uint32_t x) { return __builtin_clz(x | 1); }
static uint32_t pc(uint32_t x) { return __builtin_popcount(x); }
static uint32_t bs(uint32_t x) { return __builtin_bswap32(x); }
static uint32_t rbit_loop(uint32_t x) {
    uint32_t r = 0;
    for (int b = 0; b < 32; b++)
        r |= ((x >> b) & 1u) << (31 - b);
    return r;
}
static uint32_t rsb_test(uint32_t a) {
    uint32_t r;
    __asm__("rsb %0, %1, #31" : "=r"(r) : "r"(a));
    return r;
}
static uint32_t orr_lsl_reg(uint32_t a, uint32_t b, uint32_t sh) {
    uint32_t r;
    __asm__("orr %0, %1, %2, lsl %3" : "=r"(r) : "r"(a), "r"(b), "r"(sh));
    return r;
}
static uint32_t lsr_reg(uint32_t a, uint32_t sh) {
    uint32_t r;
    __asm__("lsr %0, %1, %2" : "=r"(r) : "r"(a), "r"(sh));
    return r;
}
static uint32_t mulw(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("mul %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

int guest_main(void) {
    uint32_t x = 0x12345678;
    out[0] = rotl7(x);            // 0x1A2B3C09
    out[1] = clz1(0x00000001);    // 31
    out[2] = clz1(0x80000000);    // 0
    out[3] = clz1(0x00F00000);    // 8
    out[4] = pc(0x12345678);      // 13
    out[5] = pc(0xFFFFFFFF);      // 32
    out[6] = bs(x);               // 0x78563412
    out[7] = rbit_loop(x);        // 0x1E6A2C48
    out[8] = rsb_test(5);         // 26
    out[9] = orr_lsl_reg(0x100, 3, 8);   // 0x100 | 0x300 = 0x300... wait 3<<8=0x300, orr 0x100 -> 0x300
    out[10] = lsr_reg(0x80000000, 4);    // 0x08000000
    out[11] = lsr_reg(0x12345678, 0);    // 0x12345678
    out[12] = mulw(0x1010101, 0x12345678);
    out[13] = rotl7(rotl7(1));    // (1<<14)|(1>>18)=0x4000
    *((volatile uint32_t*)0x80040) = 0x600DC0DEu;
    return 0;
}
