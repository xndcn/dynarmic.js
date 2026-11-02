#include "kernels.h"

static void mark(uint32_t phase) {
#if defined(__arm__)
    (void)phase;
    __asm__ volatile("svc #3" ::: "memory");
#elif defined(__i386__)
    __asm__ volatile("outl %0, %w1" :: "a"(phase), "d"((uint16_t)0xe200) : "memory");
#else
#error Unsupported guest architecture
#endif
}

uint32_t guest_main(void) {
    volatile uint32_t *mailbox = (volatile uint32_t *)0xf000;
    if (mailbox[0] != BENCH_MAGIC) { mailbox[4] = 3; return 1; }
    const uint32_t case_index = mailbox[1], iterations = mailbox[2], seed = mailbox[3];
    mailbox[4] = 1;
    mailbox[5] = 0;
    for (uint32_t sample = 0; sample < BENCH_RUNS; sample++) {
        if (bench_init(case_index, iterations, seed)) { mailbox[4] = 3; return 2; }
        mark(0);
        uint32_t checksum = bench_run();
        mark(1);
        mailbox[6 + sample] = checksum;
        mailbox[5] = sample + 1;
    }
    mailbox[4] = 2;
    return 0;
}
