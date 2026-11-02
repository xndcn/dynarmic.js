/* Five scalar C workloads. One source is compiled to ARM, x86 and Wasm.
 * Unsigned arithmetic wraps modulo 2^32. There is no libc, allocation or I/O.
 * Each timed call returns a checksum consumed and verified by the host. */
#include "kernels.h"

enum { WORDS = 8192, WORD_MASK = WORDS - 1 };
static uint32_t data[WORDS], order[WORDS];
static uint32_t selected_case, count, initial_seed;

static uint32_t next(uint32_t value) {
    return value * 1664525u + 1013904223u;
}

uint32_t bench_init(uint32_t case_index, uint32_t iterations, uint32_t seed) {
    if (case_index >= BENCH_CASES || iterations == 0 || iterations > 32000000u) return 1;
    selected_case = case_index;
    count = iterations;
    initial_seed = seed;
    if (case_index == 2) {
        uint32_t state = seed;
        for (uint32_t i = 0; i < WORDS; i++) {
            state = next(state);
            data[i] = state;
        }
    } else if (case_index == 3) {
        /* Shuffle indices, then connect them into one complete pointer cycle.
         * The power-of-two mask avoids a library division helper. */
        for (uint32_t i = 0; i < WORDS; i++) order[i] = i;
        uint32_t state = seed;
        for (uint32_t i = 0; i < WORDS; i++) {
            state = next(state);
            uint32_t j = (state >> 8) & WORD_MASK;
            uint32_t temporary = order[i];
            order[i] = order[j];
            order[j] = temporary;
        }
        for (uint32_t i = 0; i < WORDS; i++) data[order[i]] = order[(i + 1) & WORD_MASK];
    }
    return 0;
}

__attribute__((noinline)) static uint32_t integer_mix(uint32_t n, uint32_t seed) {
    uint32_t value = seed;
    for (uint32_t i = 0; i < n; i++) {
        value = next(value);
        value ^= value >> 13;
        value = (value << 7) | (value >> 25);
    }
    return value;
}

/* Distinct non-inlined callees retain actual data-dependent control flow;
 * the build audits conditional branches and calls in this kernel's assembly. */
__attribute__((noinline)) static uint32_t branch_left(uint32_t value) {
    return (value ^ (value >> 7)) * 3u + 0x9e3779b9u;
}
__attribute__((noinline)) static uint32_t branch_right(uint32_t value) {
    return ((value << 5) | (value >> 27)) + 0x7f4a7c15u;
}
__attribute__((noinline)) static uint32_t branch_mix(uint32_t n, uint32_t seed) {
    uint32_t value = seed, state = seed;
    for (uint32_t i = 0; i < n; i++) {
        state = next(state);
        if (state & 0x80000000u) value = branch_left(value ^ state);
        else value = branch_right(value + state);
    }
    return value ^ state;
}

__attribute__((noinline)) static uint32_t stream_array(uint32_t n, uint32_t seed) {
    uint32_t sum = seed;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t index = i & WORD_MASK;
        uint32_t value = data[index] + i;
        data[index] = value ^ (value >> 11);
        sum += value;
    }
    return sum ^ data[(n - 1) & WORD_MASK];
}

__attribute__((noinline)) static uint32_t pointer_chase(uint32_t n, uint32_t seed) {
    uint32_t index = seed & WORD_MASK, sum = seed;
    for (uint32_t i = 0; i < n; i++) {
        index = data[index];
        sum = (sum ^ index) + 0x9e3779b9u;
    }
    return sum ^ index;
}

__attribute__((noinline)) static uint32_t float_dyadic(uint32_t n, uint32_t seed) {
    uint32_t state = seed;
    double squares = 0.0, offsets = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        state = next(state);
        double value = (double)(state >> 22) * (1.0 / 1024.0);
        squares += value * value;
        offsets += (value + 0.5) * 0.25;
    }
    /* Every operation is exact in binary64 for the accepted count range:
     * squares has denominator 2^20, offsets has denominator 2^12. Reduce
     * before converting so both results fit uint32 even at the upper bound. */
    return (uint32_t)(squares * 64.0) ^ (uint32_t)(offsets * 16.0) ^ state;
}

uint32_t bench_run(void) {
    switch (selected_case) {
    case 0: return integer_mix(count, initial_seed);
    case 1: return branch_mix(count, initial_seed);
    case 2: return stream_array(count, initial_seed);
    case 3: return pointer_chase(count, initial_seed);
    case 4: return float_dyadic(count, initial_seed);
    default: return 0;
    }
}
