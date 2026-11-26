#include <inttypes.h>
#include <stdio.h>

typedef struct {
    int32_t F0;
    int32_t F1;
} Record;

int main(void) {
    Record data[1024];
    for (int i = 0; i < 1024; i++) {
        data[i].F0 = i + 1;
        data[i].F1 = i + 2;
    }
    for (int pass = 0; pass < 2048; pass++) {
        for (int i = 0; i < 1024; i++) {
            int k = (i + pass) & 1023;
            data[i] = data[k];
            data[i].F0 = data[i].F0 ^ i;
        }
    }
    int64_t sum = 0;
    for (int i = 0; i < 1024; i++) sum += data[i].F0 + data[i].F1;
    printf("%" PRId64 "\n", sum);
    return 0;
}
