#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static const char *const parts[] = {"assets", "main", "textures", "ui", "icon", "editor", "buttons", "line"};
    size_t sizes[8];
    for (int i = 0; i < 8; ++i) sizes[i] = strlen(parts[i]);

    uint32_t hash = 17;
    for (int i = 0; i < 500000; ++i) {
        int a = (int)(hash & 7u), b = (i >> 2) & 7, c = (i + 3) & 7;
        char path[3 * (sizeof("textures") - 1) + 1];
        size_t length = sizes[a];
        memcpy(path, parts[a], length);
        path[length++] = '/';
        memcpy(path + length, parts[b], sizes[b]);
        length += sizes[b];
        memcpy(path + length, parts[c], sizes[c]);
        length += sizes[c];
        uint32_t sample = (unsigned char)path[hash % length];
        hash = (hash << 5) ^ (hash >> 2) ^ sample ^ (uint32_t)length;
    }
    printf("%" PRIu32 "\n", hash);
    return 0;
}
