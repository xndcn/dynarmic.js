// Coprocessor interface test: exercises MRC/MCR (inline u32* and Callback
// paths), MRRC/MCRR, CDP, and the unimplemented-coprocessor exception path
// against the TestCoproc registered in jit_runbin.

#include <stdint.h>

typedef uint32_t u32;

#define OUT ((volatile u32*)0x00080000)
#define MAGIC_ADDR ((volatile u32*)0x00080040)

static u32 mrc_tls(void) {
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(v));
    return v;
}
static void mcr_tls(u32 v) {
    __asm__ volatile("mcr p15, 0, %0, c13, c0, 2" ::"r"(v));
}
static u32 mrc_cb(void) {
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}
static void mcr_cb(u32 v) {
    __asm__ volatile("mcr p15, 0, %0, c14, c0, 0" ::"r"(v));
}
static void mrrc_pair(u32* lo, u32* hi) {
    u32 l, h;
    __asm__ volatile("mrrc p15, 0, %0, %1, c5" : "=r"(l), "=r"(h));
    *lo = l;
    *hi = h;
}
static void mcrr_pair(u32 lo, u32 hi) {
    __asm__ volatile("mcrr p15, 1, %0, %1, c6" ::"r"(lo), "r"(hi));
}
static void cdp_op(void) {
    __asm__ volatile("cdp p15, 2, c7, c8, c9, 3");
}
static u32 mrc_bad(void) {
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

int guest_main(void) {
    u32 i = 0;
    OUT[i++] = mrc_tls();  // expect 0x11111111
    mcr_tls(0xA5A5A5A5u);
    OUT[i++] = mrc_tls();  // expect 0xA5A5A5A5
    OUT[i++] = mrc_cb();   // expect 0xC0FFEEA5
    mcr_cb(0x12345678u);   // host records cb_sent
    u32 lo, hi;
    mrrc_pair(&lo, &hi);
    OUT[i++] = lo;  // expect 0xA5A5A5A5
    OUT[i++] = hi;  // expect 0x22222222
    mcrr_pair(0xDEADBEEFu, 0x0BADF00Du);  // host records mcrr_lo/hi
    cdp_op();                             // host bumps cdp_count
    OUT[i++] = mrc_bad();                 // UndefinedInstruction -> result 0
    *MAGIC_ADDR = 0x600DC0DEu;
    return 0;
}
