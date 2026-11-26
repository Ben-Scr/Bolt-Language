#include "../input.h"

int main(void) {
    int length;
    uint8_t *data = load_bytes(&length);
    int *stats = allocate(64 * 3, sizeof(*stats));
    int pos = 0;
    while (pos < length) {
        int service = 0;
        while (data[pos] != 44) {
            service = service * 10 + data[pos] - 48;
            pos++;
        }
        pos++;
        int status = 0;
        while (data[pos] != 44) {
            status = status * 10 + data[pos] - 48;
            pos++;
        }
        pos++;
        int latency = 0;
        while (data[pos] != 10) {
            latency = latency * 10 + data[pos] - 48;
            pos++;
        }
        pos++;
        int slot = service * 3;
        stats[slot]++;
        if (status >= 500) stats[slot + 1]++;
        if (latency >= 250) stats[slot + 2]++;
    }
    uint8_t *report = allocate(64 * 3 * 4, sizeof(*report));
    for (int i = 0; i < 64 * 3; i++) {
        for (int j = 0; j < 4; j++) report[i * 4 + j] = (uint8_t)((stats[i] >> (j * 8)) & 255);
    }
    write_result(report, 64 * 3 * 4);
    free(report);
    free(stats);
    free(data);
    return 0;
}
