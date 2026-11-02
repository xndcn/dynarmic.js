#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
uint64_t ComputeResultFP(void);
int main(void) { printf("%016" PRIx64 "\n", ComputeResultFP()); return 0; }
