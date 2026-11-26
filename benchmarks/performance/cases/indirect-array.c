#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int32_t *links = calloc(65536, sizeof *links);
    if (!links) return 1;
    for (int i = 0; i < 65536; ++i) links[i] = (i * 5 + 12345) & 65535;
    int32_t cursor = 1;
    uint64_t sum = 0;
    for (int i = 0; i < 48000000; ++i) {
        if ((uint32_t)cursor >= 65536) return 2;
        cursor = links[cursor];
        if (cursor < 0) return 3;
        sum += (uint32_t)cursor;
    }
    printf("%" PRIu64 "\n", sum);
    free(links);
    return 0;
}
