// FP guest main.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint64_t ComputeResultFP(void);

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = dst;
    while (n--)
        *d++ = (unsigned char)c;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = dst;
    const unsigned char* s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

#define RESULT_ADDR ((volatile uint64_t*)0x00080000)
#define MAGIC_ADDR  ((volatile uint32_t*)0x00080040)

int guest_main(void) {
    volatile uint64_t* out = RESULT_ADDR;
    uint64_t r = ComputeResultFP();
    out[0] = r;
    *MAGIC_ADDR = 0x600DC0DEu;
    return (int)(r & 0x7FFFFFFF);
}
