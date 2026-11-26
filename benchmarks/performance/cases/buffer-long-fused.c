#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

/* Same scalar values and order as the non-escaping buffer and its snapshot. */
int main(void) {
    uint64_t total = 0;
    for (int batch = 0; batch < 256; ++batch) {
        for (int i = 0; i < 524288; ++i)
            total += (uint64_t)(((i ^ 117) & 65535) + (batch & 31));
    }
    printf("%" PRIu64 "\n", total);
    return 0;
}
