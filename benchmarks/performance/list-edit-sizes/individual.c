#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "timer.h"

int main(void) {
    const int size = 64;
    enum { batch = 64 };
    int32_t *lists[batch];
    int insert_at[batch], remove_at[batch];
    for (int k = 0; k < batch; k++) {
        lists[k] = malloc((size + 1) * sizeof(*lists[k]));
        if (!lists[k]) return 1;
        for (int i = 0; i < size; i++) lists[k][i] = i;
    }
    int64_t insert_ticks = 0, remove_ticks = 0, removed = 0;
    for (int round = 0; round < 4096; round++) {
        for (int k = 0; k < batch; k++) {
            int step = round * batch + k;
            insert_at[k] = (step * 117 + 257) % (size + 1);
            remove_at[k] = (step * 251 + 11) % (size + 1);
        }
        int64_t start = bench_now();
        for (int k = 0; k < batch; k++) {
            int p = insert_at[k];
            memmove(lists[k]+p+1, lists[k]+p, (size-p)*sizeof(*lists[k]));
            lists[k][p] = round;
        }
        insert_ticks += bench_now() - start;
        for (int k = 0; k < batch; k++) removed += lists[k][remove_at[k]];
        start = bench_now();
        for (int k = 0; k < batch; k++) {
            int q = remove_at[k];
            memmove(lists[k]+q, lists[k]+q+1, (size-q)*sizeof(*lists[k]));
        }
        remove_ticks += bench_now() - start;
    }
    int64_t final = 0;
    for (int k = 0; k < batch; k++) {
        for (int i = 0; i < size; i++) final += (int64_t)(i+1)*lists[k][i];
        free(lists[k]);
    }
    printf("%" PRId64 "\n%" PRId64 "\n%" PRId64 "\n%" PRId64 "\n",
           bench_to_ticks(insert_ticks), bench_to_ticks(remove_ticks), removed, final);
    return 0;
}
