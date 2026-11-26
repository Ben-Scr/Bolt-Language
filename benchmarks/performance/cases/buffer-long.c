#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    uint64_t total = 0;
    for (int batch = 0; batch < 256; ++batch) {
        int32_t *items = NULL;
        size_t capacity = 0, count = 0;
        for (int32_t i = 0; i < 524288; ++i) {
            if (count == capacity) {
                size_t grown = capacity ? 2 * capacity : 4;
                int32_t *next = realloc(items, grown * sizeof *items);
                if (!next) { free(items); return 1; }
                items = next;
                capacity = grown;
            }
            items[count++] = ((i ^ 117) & 65535) + (batch & 31);
        }
        int32_t *copy = malloc(count * sizeof *copy);
        if (!copy) { free(items); return 1; }
        memcpy(copy, items, count * sizeof *copy);
        for (size_t i = 0; i < count; ++i) total += (uint64_t)copy[i];
        free(copy);
        free(items);
    }
    printf("%" PRIu64 "\n", total);
    return 0;
}
