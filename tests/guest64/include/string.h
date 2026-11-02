// Minimal freestanding <string.h> for the A64 guests (zig's freestanding
// target ships no libc headers). The guests define what they use.
#pragma once

#include <stddef.h>

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
