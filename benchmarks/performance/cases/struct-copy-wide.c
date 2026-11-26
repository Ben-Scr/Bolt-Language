#include <inttypes.h>
#include <stdio.h>

typedef struct {
    int64_t F0;
    int64_t F1;
    int64_t F2;
    int64_t F3;
    int64_t F4;
    int64_t F5;
    int64_t F6;
    int64_t F7;
    int64_t F8;
    int64_t F9;
    int64_t F10;
    int64_t F11;
} Record;

int main(void) {
    Record data[1024];
    for (int i = 0; i < 1024; i++) {
        data[i].F0 = i + 1;
        data[i].F1 = i + 2;
        data[i].F2 = i + 3;
        data[i].F3 = i + 4;
        data[i].F4 = i + 5;
        data[i].F5 = i + 6;
        data[i].F6 = i + 7;
        data[i].F7 = i + 8;
        data[i].F8 = i + 9;
        data[i].F9 = i + 10;
        data[i].F10 = i + 11;
        data[i].F11 = i + 12;
    }
    for (int pass = 0; pass < 2048; pass++) {
        for (int i = 0; i < 1024; i++) {
            int k = (i + pass) & 1023;
            data[i] = data[k];
            data[i].F0 = data[i].F0 ^ i;
        }
    }
    int64_t sum = 0;
    for (int i = 0; i < 1024; i++) sum += data[i].F0 + data[i].F1 + data[i].F2 + data[i].F3 + data[i].F4 + data[i].F5 + data[i].F6 + data[i].F7 + data[i].F8 + data[i].F9 + data[i].F10 + data[i].F11;
    printf("%" PRId64 "\n", sum);
    return 0;
}
