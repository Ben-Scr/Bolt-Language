#include <stdio.h>

int main(void) {
    int items[256], sum = 0;
    for (int pass = 0; pass < 8192; pass++) {
        int count = 0;
        for (int i = 0; i < 256; i++) items[count++] = (i ^ pass) & 255;
        for (int i = 0; i < count; i++) sum += items[i];
    }
    printf("%d\n", sum);
    return 0;
}
