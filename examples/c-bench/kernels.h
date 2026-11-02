#ifndef DYNARMIC_C_BENCH_KERNELS_H
#define DYNARMIC_C_BENCH_KERNELS_H
#include <stdint.h>

enum { BENCH_CASES = 5, BENCH_RUNS = 6, BENCH_MAGIC = 0x43424e31 };
/* All allocation and input preparation are outside bench_run's timed work. */
uint32_t bench_init(uint32_t case_index, uint32_t iterations, uint32_t seed);
uint32_t bench_run(void);
#endif
