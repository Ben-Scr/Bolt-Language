#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep the accumulator unchanged on the first failing element. */
static int add_checked(uint64_t *total, int32_t value) {
    if (value < 0) return 2;
    uint64_t converted = (uint64_t)value;
    if (UINT64_MAX - *total < converted) return 3;
    *total += converted;
    return 0;
}

int main(void) {
    uint64_t total = 0;
    for (int batch = 0; batch < 256; ++batch) {
        int32_t *items = NULL;
        size_t capacity = 0, count = 0;
        for (int32_t i = 0; i < 524288; ++i) {
            if (count == capacity) {
                /* Match Bolt's signed Length and saturating capacity doubling. */
                if (count >= INT32_MAX) { free(items); return 1; }
                size_t grown = capacity > INT32_MAX / 2 ? INT32_MAX : capacity * 2;
                if (grown < 4) grown = 4;
                if (grown > SIZE_MAX / sizeof *items) { free(items); return 1; }
                int32_t *next = realloc(items, grown * sizeof *items);
                if (!next) { free(items); return 1; }
                items = next;
                capacity = grown;
            }
            items[count++] = ((i ^ 117) & 65535) + (batch & 31);
        }
        /* count <= capacity and capacity * sizeof *items was checked at growth. */
        int32_t *copy = malloc(count * sizeof *copy);
        if (!copy) { free(items); return 1; }
        memcpy(copy, items, count * sizeof *copy);
        for (size_t i = 0; i < count; ++i) {
            int status = add_checked(&total, copy[i]);
            if (status) { free(copy); free(items); return status; }
        }
        free(copy);
        free(items);
    }
    printf("%" PRIu64 "\n", total);
    return 0;
}
