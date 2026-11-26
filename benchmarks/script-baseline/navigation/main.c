#include "../fixture_io.h"

int main(void) {
    size_t expected = 128 * 128 + 256 * 4;
    uint8_t *data = read_input(expected);
    int width = 128;
    int cells = width * width;
    int queries = ((int)expected - cells) / 4;
    uint8_t *lengths = allocate(queries * 2, sizeof(*lengths));
    int *queue = allocate(cells, sizeof(*queue));
    int *distance = allocate(cells, sizeof(*distance));
    int *seen = allocate(cells, sizeof(*seen));
    int *offset = allocate(4, sizeof(*offset));
    offset[0] = 1; offset[1] = -1; offset[2] = width; offset[3] = -width;
    for (int request = 0; request < queries; request++) {
        int pos = cells + request * 4;
        int start = data[pos] + width * data[pos + 1];
        int goal = data[pos + 2] + width * data[pos + 3];
        int stamp = request + 1;
        int head = 0;
        int tail = 1;
        queue[0] = start;
        seen[start] = stamp;
        distance[start] = 0;
        int steps = 65535;
        while (head < tail) {
            int tile = queue[head++];
            if (tile == goal) {
                steps = distance[tile];
                break;
            }
            for (int direction = 0; direction < 4; direction++) {
                int adjacent = tile + offset[direction];
                if (data[adjacent] != 0 && seen[adjacent] != stamp) {
                    seen[adjacent] = stamp;
                    distance[adjacent] = distance[tile] + 1;
                    queue[tail++] = adjacent;
                }
            }
        }
        lengths[request * 2] = (uint8_t)(steps & 255);
        lengths[request * 2 + 1] = (uint8_t)(steps >> 8);
    }
    write_result(lengths, queries * 2);
    free(offset);
    free(seen);
    free(distance);
    free(queue);
    free(lengths);
    free(data);
    return 0;
}
