#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const int32_t count = 2097151;
    const size_t bytes = (size_t)count * sizeof(int32_t);
    int32_t *items = malloc(bytes);
    if (!items) return 1;
    for (int32_t i = 0; i < count; ++i) items[i] = (i ^ 123456) & 1023;

    int32_t *snapshot = malloc(bytes);
    if (!snapshot) {
        free(items);
        return 1;
    }
    memcpy(snapshot, items, bytes);
    for (int32_t i = 0; i < count; i += 193) items[i] ^= 511;
    int64_t original = 0, edited = 0;
    for (int32_t i = 0; i < count; ++i) {
        original += snapshot[i];
        edited += items[i];
    }
    printf("%" PRId64 "\n%" PRId64 "\n", original, edited);
    free(snapshot);
    free(items);
    return 0;
}
