#include "../input.h"

int main(void) {
    int length;
    uint8_t *data = load_bytes(&length);
    int pixels = data[0] + data[1] * 256 + data[2] * 65536;
    uint8_t *rgba = allocate(pixels * 4, sizeof(*rgba));
    int pos = 3 + 256 * 3;
    int outPos = 0;
    int transparent = 7;
    while (pos < length) {
        int packet = data[pos++];
        int count = (packet & 127) + 1;
        int repeated = 0;
        if (packet >= 128) repeated = data[pos++];
        for (int i = 0; i < count; i++) {
            int color = repeated;
            if (packet < 128) color = data[pos++];
            int palette = 3 + color * 3;
            rgba[outPos++] = data[palette];
            rgba[outPos++] = data[palette + 1];
            rgba[outPos++] = data[palette + 2];
            if (color == transparent) rgba[outPos++] = (uint8_t)0;
            else rgba[outPos++] = (uint8_t)255;
        }
    }
    write_result(rgba, pixels * 4);
    free(rgba);
    free(data);
    return 0;
}
