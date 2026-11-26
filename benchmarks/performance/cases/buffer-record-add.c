#include <stdio.h>

typedef struct { int x, y; } Pair;

int main(void) {
    Pair items[512];
    int sum = 0;
    for (int pass = 0; pass < 4096; pass++) {
        int count = 0;
        for (int i = 0; i < 512; i++) {
            Pair item = {(i ^ pass) & 255, (i + pass) & 255};
            items[count++] = item;
        }
        for (int i = 0; i < count; i++) sum += items[i].x + items[i].y;
    }
    printf("%d\n", sum);
    return 0;
}
