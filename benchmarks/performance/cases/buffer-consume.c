#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
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
        items[count++] = (i ^ 117) & 65535;
    }
    /* The original buffer has no references after the snapshot. */
    int32_t *copy = items;
    items = NULL;
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum += (uint64_t)copy[i];
    printf("%" PRIu64 "\n", sum);
    free(copy);
    return 0;
}
