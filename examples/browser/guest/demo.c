// Demo guest for the browser example: prints through SVC #1 and runs three
// compute kernels whose results are known, so the page can verify the JIT.
// SVC #3 marks a kernel boundary (the host records a timestamp; nothing is
// printed). The same source builds for A32, for A64, and natively (-DHOST)
// to produce the expected output.

#include <stdint.h>

typedef uint32_t u32;

#ifdef HOST
#include <stdio.h>
#include <time.h>
static void put_char(char c) { putchar(c); }
// Kernel boundary timestamps go to stderr (bench.mjs reads them).
static void mark(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "mark %.3f\n", ts.tv_sec * 1e3 + ts.tv_nsec / 1e6);
}
#elif defined(__aarch64__)
static void put_char(char c) {
    register unsigned long x0 __asm__("x0") = (unsigned char)c;
    __asm__ volatile("svc #1" :: "r"(x0) : "memory");
}
static void mark(void) { __asm__ volatile("svc #3" ::: "memory"); }
#else
static void put_char(char c) {
    register u32 r0 __asm__("r0") = (u32)c;
    __asm__ volatile("svc #1" :: "r"(r0) : "memory");
}
static void mark(void) { __asm__ volatile("svc #3" ::: "memory"); }
#endif

static void put_str(const char* s) {
    while (*s) put_char(*s++);
}

static void put_u32(u32 v) {
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) put_char(buf[--n]);
}

// 1. Integer ALU loop: 64M LCG steps (a tight self-loop in the JIT).
static u32 kernel_lcg(void) {
    u32 x = 12345;
    for (u32 i = 0; i < 64u * 1000 * 1000; i++) {
        x = x * 1664525u + 1013904223u;
        x ^= x >> 13;
    }
    return x;
}

// 2. Memory-bound: sieve of Eratosthenes below 4M in guest RAM at 0x200000.
static u32 kernel_sieve(void) {
#ifdef HOST
    static unsigned char host_sieve[4000000];
    volatile unsigned char* s = host_sieve;
#else
    volatile unsigned char* s = (volatile unsigned char*)0x200000;
#endif
    const u32 n = 4000000;
    for (u32 i = 0; i < n; i++) s[i] = 1;
    s[0] = s[1] = 0;
    for (u32 i = 2; (unsigned long long)i * i < n; i++) {
        if (s[i]) {
            for (u32 j = i * i; j < n; j += i) s[j] = 0;
        }
    }
    u32 count = 0;
    for (u32 i = 0; i < n; i++) count += s[i];
    return count;
}

// 3. Floating point (VFP double): Leibniz series for pi.
static u32 kernel_pi(void) {
    double sum = 0.0, sign = 1.0;
    for (u32 k = 0; k < 20u * 1000 * 1000; k++) {
        sum += sign / (double)(2 * k + 1);
        sign = -sign;
    }
    return (u32)(sum * 4.0 * 1000000.0);
}

#ifdef HOST
int main(void) {
#else
int guest_main(void) {
#endif
#ifndef KERNELS
#define KERNELS 7
#endif
    mark();
#if KERNELS & 1
    put_str("lcg   = ");
    put_u32(kernel_lcg());
    put_char('\n');
    mark();
#endif
#if KERNELS & 2
    put_str("sieve = ");
    put_u32(kernel_sieve());
    put_char('\n');
    mark();
#endif
#if KERNELS & 4
    put_str("pi*1e6 = ");
    put_u32(kernel_pi());
    put_char('\n');
    mark();
#endif
    return 0;
}
