#include <stdio.h>
#include <string.h>

int main(void) {
    const char *names[] = {"ui/menu.bin", "ui/menu.txt", "map/1.bin", "data", "", ".bin", "ui/", "ui/a.bin"};
    size_t lengths[8];
    for (int i = 0; i < 8; i++) lengths[i] = strlen(names[i]);
    int score = 0;
    for (int i = 0; i < 2000000; i++) {
        int k = (i ^ (i >> 4)) & 7;
        if (lengths[k] >= 3 && memcmp(names[k], "ui/", 3) == 0) score++;
        if (lengths[k] >= 4 && memcmp(names[k] + lengths[k] - 4, ".bin", 4) == 0) score += 2;
    }
    printf("%d\n", score);
    return 0;
}
