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
        if ((*b & 1u) != 0u) {
            uint32_t *temp = a;
            a = b;
            b = temp;
        }
    }
    printf("%" PRIu32 "\n%" PRIu32 "\n", first, second);
    return 0;
}
