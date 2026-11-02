// Host reference: compile with the same compute.c and print the checksum.

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

uint64_t ComputeResult(void);
uint64_t ComputeResultStage(int stage);

int main(void) {
    for (int s = 0; s < 6; s++)
        printf("stage%d %016" PRIx64 "\n", s, ComputeResultStage(s));
    printf("all    %016" PRIx64 "\n", ComputeResult());
    return 0;
}
