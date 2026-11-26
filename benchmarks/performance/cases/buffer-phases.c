#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t nanoseconds(void) {
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) exit(2);
    return (int64_t)stamp.tv_sec * 1000000000 + stamp.tv_nsec;
}

int main(void) {
    uint64_t total = 0;
    int64_t add = 0, copy_time = 0, read = 0, release = 0;
    for (int batch = 0; batch < 256; ++batch) {
        int64_t start = nanoseconds();
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
        int64_t after_add = nanoseconds();
        int32_t *copy = malloc(count * sizeof *copy);
        if (!copy) { free(items); return 1; }
        memcpy(copy, items, count * sizeof *copy);
        int64_t after_copy = nanoseconds();
        for (size_t i = 0; i < count; ++i) total += (uint64_t)copy[i];
        int64_t after_read = nanoseconds();
        free(copy);
        free(items);
        int64_t end = nanoseconds();
        add += after_add - start;
        copy_time += after_copy - after_add;
        read += after_read - after_copy;
        release += end - after_read;
    }
    printf("%" PRIu64 "\n%" PRId64 "\n%" PRId64 "\n%" PRId64 "\n%" PRId64 "\n",
           total, add, copy_time, read, release);
    return 0;
}
