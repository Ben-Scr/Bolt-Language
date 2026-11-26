#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    int32_t *input = malloc(16384 * sizeof(*input));
    int32_t *work = malloc(16384 * sizeof(*work));
    if (!input || !work) { free(input); free(work); return 1; }
    for (int32_t i = 0; i < 16384; ++i) input[i] = (i * 17 + 5) & 65535;
    int32_t length = 16384;
    memcpy(work, input, (size_t)length * sizeof(*input));
    uint64_t checksum = 0;
    for (int32_t pass = 0; pass < 4096; ++pass) {
        int32_t retained = 2048 + (pass & 1023);
        int32_t copied = 8192 + ((pass * 31) & 2047);
        int32_t offset = (pass * 13) & 2047;
        length = retained;
        work[retained - 1] = pass;
        input[offset] = pass ^ 37;
        memcpy(work + length, input + offset, (size_t)copied * sizeof(*input));
        length += copied;
        for (int32_t i = 0; i < 32; ++i) {
            int32_t sample = (pass * 53 + i * 233) % length;
            checksum += (uint64_t)work[sample];
        }
    }
    for (int32_t i = 0; i < length; ++i) checksum += (uint64_t)work[i] * (uint64_t)(i + 1);
    printf("%" PRIu64 "\n", checksum);
    free(input);
    free(work);
    return 0;
}
