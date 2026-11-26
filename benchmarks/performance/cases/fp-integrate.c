#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    double state = 0.125;
    for (int i = 0; i < 128000000; ++i) {
        double impulse = (double)(i & 7) * 0.125;
        state = state * 0.984375 + impulse;
    }
    uint64_t bits;
    memcpy(&bits, &state, sizeof bits);
    printf("%" PRIu64 "\n", bits);
    return 0;
}
