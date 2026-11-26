#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const size_t length = 32768;
    int32_t *items = malloc((length + 1) * sizeof(*items));
    if (!items) return 1;
    for (size_t i = 0; i < length; ++i) items[i] = (int32_t)i;
    uint64_t removed = 0;
    for (int32_t step = 0; step < 12000; ++step) {
        size_t insert_at = (size_t)((step * 117 + 257) & 32767);
        size_t remove_at = (size_t)((step * 251 + 11) & 32767);
        memmove(&items[insert_at + 1], &items[insert_at], (length - insert_at) * sizeof(*items));
        items[insert_at] = step;
        removed += (uint64_t)items[remove_at];
        memmove(&items[remove_at], &items[remove_at + 1], (length - remove_at) * sizeof(*items));
    }
    uint64_t final = 0;
    for (size_t i = 0; i < length; ++i) final += (i + 1) * (uint64_t)items[i];
    printf("%" PRIu64 "\n%" PRIu64 "\n", removed, final);
    free(items);
    return 0;
}
