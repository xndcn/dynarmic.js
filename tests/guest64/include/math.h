// Minimal freestanding <math.h> for the A64 guests (zig's freestanding
// target ships no libc headers). The guests define what they use.
#pragma once

double modf(double x, double* iptr);
