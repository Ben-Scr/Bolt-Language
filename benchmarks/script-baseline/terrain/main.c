#include "../fixture_io.h"

int main(void) {
    size_t expected = 2048 * 2048;
    uint8_t *height = read_input(expected);
    int width = 2048;
    int rows = (int)expected / width;
    uint8_t *mask = allocate((int)expected, sizeof(*mask));
    int slopeLimit = 32;
    for (int y = 1; y < rows - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            int p = y * width + x;
            int dx = (int)height[p + 1] - (int)height[p - 1];
            int dy = (int)height[p + width] - (int)height[p - width];
            int slope = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            int material = height[p] < 64 ? 1 : (slope > slopeLimit ? 3 : 2);
            mask[p] = (uint8_t)material;
        }
    }
    write_result(mask, expected);
    free(mask);
    free(height);
    return 0;
}
