// CoreMark platform layer, see core_portme.h.
#include "coremark.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 default_num_contexts = 1;

#ifdef HOST
#include <stdio.h>
#include <time.h>
static ee_u32 clock_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ee_u32)(ts.tv_sec * 1000000ull + ts.tv_nsec / 1000);
}
void portme_putchar(char c) {
    putchar(c);
}
#elif defined(__aarch64__)
static ee_u32 clock_us(void) {
    register unsigned long x0 __asm__("x0") = 0;
    __asm__ volatile("svc #4" : "+r"(x0)::"memory");
    return (ee_u32)x0;
}
void portme_putchar(char c) {
    register unsigned long x0 __asm__("x0") = (unsigned char)c;
    __asm__ volatile("svc #1" ::"r"(x0) : "memory");
}
#else
static ee_u32 clock_us(void) {
    register ee_u32 r0 __asm__("r0") = 0;
    __asm__ volatile("svc #4" : "+r"(r0)::"memory");
    return r0;
}
void portme_putchar(char c) {
    register ee_u32 r0 __asm__("r0") = (unsigned char)c;
    __asm__ volatile("svc #1" ::"r"(r0) : "memory");
}
#endif

#ifndef HOST
// The freestanding guests have no libc.
void* memset(void* dst, int c, size_t n) {
    unsigned char* d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

// Used by barebones/cvt.c (number formatting in ee_printf).
double modf(double x, double* iptr) {
    if (!(x == x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) {
        *iptr = x;  // NaN, infinities and values without a fraction part
        return x == x ? 0.0 * x : x;
    }
    const double i = (double)(long long)x;
    *iptr = i;
    return x - i;
}
#endif

static CORETIMETYPE start_time_val, stop_time_val;

void start_time(void) {
    start_time_val = clock_us();
}

void stop_time(void) {
    stop_time_val = clock_us();
}

CORE_TICKS get_time(void) {
    return stop_time_val - start_time_val;
}

secs_ret time_in_secs(CORE_TICKS ticks) {
    return (secs_ret)ticks / 1000000.0;
}

void portable_init(core_portable* p, int* argc, char* argv[]) {
    (void)argc;
    (void)argv;
    if (sizeof(ee_ptr_int) != sizeof(ee_u8*)) {
        ee_printf("ERROR! Please define ee_ptr_int to a type that holds a pointer!\n");
    }
    if (sizeof(ee_u32) != 4) {
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    }
    p->portable_id = 1;
}

void portable_fini(core_portable* p) {
    p->portable_id = 0;
}
