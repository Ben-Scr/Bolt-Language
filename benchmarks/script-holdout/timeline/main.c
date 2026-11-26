#include "../input.h"

static void Merge(const int *source, int *dest, const int *times, int size, int width) {
    for (int start = 0; start < size; start += width * 2) {
        int mid = start + width;
        if (mid > size) mid = size;
        int end = mid + width;
        if (end > size) end = size;
        int left = start;
        int right = mid;
        for (int k = start; k < end; k++) {
            if (right == end || (left < mid && times[source[left]] <= times[source[right]])) {
                dest[k] = source[left++];
            } else {
                dest[k] = source[right++];
            }
        }
    }
}

int main(void) {
    int length;
    uint8_t *data = load_bytes(&length);
    int size = length / 2;
    int *times = allocate(size, sizeof(*times));
    int *order = allocate(size, sizeof(*order));
    int *scratch = allocate(size, sizeof(*scratch));
    for (int i = 0; i < size; i++) {
        times[i] = data[2 * i] + data[2 * i + 1] * 256;
        order[i] = i;
    }
    int pass = 0;
    for (int width = 1; width < size; width *= 2) {
        if (pass % 2 == 0) Merge(order, scratch, times, size, width);
        else Merge(scratch, order, times, size, width);
        pass++;
    }
    uint8_t *report = allocate(size * 2, sizeof(*report));
    for (int i = 0; i < size; i++) {
        int id = 0;
        if (pass % 2 == 0) id = order[i];
        else id = scratch[i];
        report[i * 2] = (uint8_t)(id & 255);
        report[i * 2 + 1] = (uint8_t)(id >> 8);
    }
    write_result(report, size * 2);
    free(report);
    free(scratch);
    free(order);
    free(times);
    free(data);
    return 0;
}
