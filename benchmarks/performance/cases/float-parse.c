#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *text[] = {
        "-1513.125", "0.125", "3.4028234e30", "-1.1754944e-30",
        "2.2250738585072014", "17.00000095367431640625",
        "0.10000000149011612", "-2345.6789012345678"
    };
    uint64_t sum = 17;
    for (unsigned i = 0; i < 1000000; ++i) {
        const char *item = text[(i ^ (i >> 3)) & 7];
        char *end;
        double d = strtod(item, &end);
        if (*end) return 1;
        float f = strtof(item, &end);
        if (*end) return 1;
        uint64_t a;
        uint32_t b;
        memcpy(&a, &d, sizeof(a));
        memcpy(&b, &f, sizeof(b));
        sum = ((sum << 7) | (sum >> 57)) ^ a ^ ((uint64_t)b << 24) ^ i;
    }
    printf("%" PRIu64 "\n", sum);
    return 0;
}
