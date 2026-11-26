#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint64_t state = UINT64_C(1);
    for (int i = 0; i < 500000000; ++i) {
        uint64_t next = state ^ (uint64_t)i;
        state = (next << 9) | (next >> 55);
        state = state ^ UINT64_C(11400714819323198485);
        state = state ^ (state >> 17);
    }
    printf("%" PRIu64 "\n", state);
    return 0;
}
