#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void *allocate(size_t count, size_t size) {
    void *p = calloc(count, size);
    if (!p) exit(2);
    return p;
}

static uint8_t *read_input(size_t length) {
    FILE *f = fopen("input.bin", "rb");
    uint8_t *data = allocate(length, 1);
    if (!f || fread(data, 1, length, f) != length || fgetc(f) != EOF || fclose(f)) exit(3);
    return data;
}

static void write_result(const uint8_t *data, size_t length) {
    FILE *f = fopen("result.bin", "wb");
    if (!f || fwrite(data, 1, length, f) != length || fclose(f)) exit(4);
}
