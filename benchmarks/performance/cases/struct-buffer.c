#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct position { uint32_t x, y; };
struct vertex { struct position point; uint32_t color, id; };

int main(void) {
    struct vertex *source = malloc(16384 * sizeof(*source));
    struct vertex *current = malloc(8192 * sizeof(*current));
    struct vertex *saved = malloc(8192 * sizeof(*saved));
    if (!source || !current || !saved) { free(source); free(current); free(saved); return 1; }
    for (int32_t i = 0; i < 16384; ++i) {
        source[i].point.x = (uint32_t)((i * 619) & 65535);
        source[i].point.y = (uint32_t)((i * 317) & 65535);
        source[i].color = (uint32_t)((i * 3) & 255);
        source[i].id = (uint32_t)i;
    }
    uint64_t checksum = 0;
    for (int32_t frame = 0; frame < 1024; ++frame) {
        int32_t first = (frame * 31) & 8191;
        memcpy(current, source + first, 8192 * sizeof(*current));
        memcpy(saved, current, 8192 * sizeof(*saved));
        int32_t changed = (frame * 43) & 8191;
        current[changed].point.x = (uint32_t)frame;
        source[first + changed].point.y ^= (uint32_t)frame;
        checksum += (uint64_t)saved[changed].point.x + (uint64_t)current[changed].point.x;
        for (int32_t j = 0; j < 32; ++j) {
            int32_t k = (frame * 13 + j * 977) & 8191;
            checksum += (uint64_t)saved[k].point.y + current[k].color + saved[k].id;
        }
    }
    printf("%" PRIu64 "\n", checksum);
    free(source);
    free(current);
    free(saved);
    return 0;
}
