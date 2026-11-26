#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    uint32_t first = 1, second = 7;
    uint32_t *a = &first, *b = &second;
    for (int i = 0; i < 64000000; ++i) {
        uint32_t x = *a ^ (uint32_t)i;
        *a = (x << 7) | (x >> 25);
        *b ^= *a >> 1;
        uintptr_t mask = 0u - (uintptr_t)(*b & 1u);
        uintptr_t changed = ((uintptr_t)a ^ (uintptr_t)b) & mask;
        a = (uint32_t *)((uintptr_t)a ^ changed);
        b = (uint32_t *)((uintptr_t)b ^ changed);
    }
    printf("%" PRIu32 "\n%" PRIu32 "\n", first, second);
    return 0;
}
