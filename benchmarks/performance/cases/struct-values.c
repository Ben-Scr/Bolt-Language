#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { uint32_t x, y; } Pair;
static Pair advance(Pair value, uint32_t salt) {
    uint32_t mixed = value.x ^ salt ^ (value.y >> 3);
    value.x = (mixed << 7) | (mixed >> 25);
    value.y ^= value.x;
    return value;
}
int main(void) {
    Pair state = {1, 7};
    for (int i = 0; i < 96000000; ++i) {
        Pair previous = state;
        state = advance(previous, (uint32_t)i);
        state.y ^= previous.x >> 1;
    }
    printf("%" PRIu32 "\n%" PRIu32 "\n", state.x, state.y);
    return 0;
}
