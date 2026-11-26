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
        for (int32_t i = 0; i < 524288; ) {
            if (count == capacity) {
                size_t grown = capacity ? 2 * capacity : 4;
                int32_t *next = realloc(items, grown * sizeof *items);
                if (!next) { free(items); return 1; }
                items = next;
                capacity = grown;
            }
            /* Expose the same geometric capacity boundaries to GCC as Bolt's
               counted append optimization does for ordinary element-wise Add. */
            const size_t remaining = (size_t)(524288 - i);
            const size_t available = capacity - count;
            const size_t take = available < remaining ? available : remaining;
            for (size_t k = 0; k < take; ++k) {
                const int32_t index = i + (int32_t)k;
                items[count + k] = ((index ^ 117) & 65535) + (batch & 31);
            }
            count += take;
            i += (int32_t)take;
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
