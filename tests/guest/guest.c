// Guest main: store the checksum where the harness can read it, return it in
// r0 (truncated) via _start's svc.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint64_t ComputeResult(void);
uint64_t ComputeResultStage(int stage);

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
    for (int s = 0; s < 6; s++)
        out[s] = ComputeResultStage(s);
    uint64_t r = ComputeResult();
    out[6] = r;
    *MAGIC_ADDR = 0x600DC0DEu;
    return (int)(r & 0x7FFFFFFF);
}
