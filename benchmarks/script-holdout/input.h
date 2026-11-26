#include "../script-baseline/fixture_io.h"

/* The full run and editor validation use different record counts. */
static uint8_t *load_bytes(int *length) {
    FILE *file = fopen("input.bin", "rb");
    if (!file || fseek(file, 0, SEEK_END)) exit(3);
    long size = ftell(file);
    if (size < 0 || size > INT32_MAX || fseek(file, 0, SEEK_SET)) exit(3);
    uint8_t *bytes = allocate((size_t)size, 1);
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size || fclose(file)) exit(3);
    *length = (int)size;
    return bytes;
}
