#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct text { size_t length; unsigned char bytes[127]; };

int main(void) {
    struct text left[128], right[128];
    for (int i = 0; i < 128; ++i) {
        const int n = (i * 17) & 127;
        left[i].length = right[i].length = (size_t)n;
        for (int j = 0; j < n; ++j)
            left[i].bytes[j] = right[i].bytes[j] = (unsigned char)((j+i) & 63);
        if (n && (i & 3) == 0) right[i].bytes[n-1] ^= 1;
        if (n && (i & 3) == 1) right[i].bytes[0] ^= 1;
    }
    uint32_t hash = 1;
    for (int32_t i = 0; i < 4000000; ++i) {
        const size_t k = (hash + (uint32_t)(i & 127)) & 127u;
        const int equal = left[k].length == right[k].length &&
            !memcmp(left[k].bytes, right[k].bytes, left[k].length);
        hash = ((hash << 3) ^ (hash >> 18) ^ (equal ? 17u : 51u)) & 16777215u;
    }
    printf("%" PRIu32 "\n", hash);
    return 0;
}
