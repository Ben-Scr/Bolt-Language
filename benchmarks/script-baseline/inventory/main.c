#include "../fixture_io.h"

int main(void) {
    size_t expected = 750000 * 4;
    uint8_t *events = read_input(expected);
    int items = 4096;
    int *stock = allocate(items, sizeof(*stock));
    int *rejected = allocate(items, sizeof(*rejected));
    for (int i = 0; i < items; i++) stock[i] = 24;
    for (int pos = 0; pos < (int)expected; pos += 4) {
        int id = events[pos] | ((int)events[pos + 1] << 8);
        int quantity = events[pos + 3];
        if (events[pos + 2] == 0) {
            stock[id] += quantity;
        } else if (stock[id] >= quantity) {
            stock[id] -= quantity;
        } else {
            rejected[id]++;
        }
    }
    uint8_t *report = allocate(items * 4, sizeof(*report));
    for (int i = 0; i < items; i++) {
        int p = i * 4;
        report[p] = (uint8_t)(stock[i] & 255);
        report[p + 1] = (uint8_t)(stock[i] >> 8);
        report[p + 2] = (uint8_t)(rejected[i] & 255);
        report[p + 3] = (uint8_t)(rejected[i] >> 8);
    }
    write_result(report, items * 4);
    free(report);
    free(rejected);
    free(stock);
    free(events);
    return 0;
}
