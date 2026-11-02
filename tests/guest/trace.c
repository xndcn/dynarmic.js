// Trace: one iteration of test_bits, storing x after each step.

#include <stdint.h>

volatile uint32_t* out = (volatile uint32_t*)0x80000;

static uint32_t rotl7(uint32_t x) { return (x << 7) | (x >> 25); }

__attribute__((noinline)) static uint32_t pc_call(uint32_t x) {
    return __builtin_popcount(x);
}

int guest_main(void) {
    uint32_t x = 0x12345678;
    x = rotl7(x);
    out[0] = x;
    x ^= __builtin_clz(x | 1);
    out[1] = x;
    x += pc_call(x);
    out[2] = x;
    x ^= __builtin_bswap32(x) >> 3;
    out[3] = x;
    uint32_t r = 0;
    for (int b = 0; b < 32; b++)
        r |= ((x >> b) & 1u) << (31 - b);
    out[4] = r;
    x += r >> 5;
    out[5] = x;

    // second iteration (loop-carried state)
    x = rotl7(x);
    out[6] = x;
    x ^= __builtin_clz(x | 1);
    out[7] = x;
    x += pc_call(x);
    out[8] = x;
    x ^= __builtin_bswap32(x) >> 3;
    out[9] = x;
    r = 0;
    for (int b = 0; b < 32; b++)
        r |= ((x >> b) & 1u) << (31 - b);
    out[10] = r;
    x += r >> 5;
    out[11] = x;

    *((volatile uint32_t*)0x80040) = 0x600DC0DEu;
    return 0;
}
