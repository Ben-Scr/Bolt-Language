#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    enum { depth = 4096 };
    uint32_t *incoming = malloc(depth * sizeof(*incoming));
    if (!incoming) return 1;
    for (int32_t i = 0; i < depth; ++i) incoming[i] = (uint32_t)(i * 7);
#ifdef BENCH_RING_QUEUE
    size_t head = 0;
#else
    uint32_t *outgoing = malloc(depth * sizeof(*outgoing));
    if (!outgoing) { free(incoming); return 1; }
    size_t added = depth, ready = 0;
#endif
    uint64_t sum = 0;
    for (int32_t turn = 0; turn < 8000000; ++turn) {
#ifdef BENCH_RING_QUEUE
        uint32_t value = incoming[head];
#else
        if (!ready) while (added) outgoing[ready++] = incoming[--added];
        uint32_t value = outgoing[--ready];
#endif
        uint32_t next = ((value << 7) ^ (value >> 13) ^ (uint32_t)(turn * 17)) & 65535u;
#ifdef BENCH_RING_QUEUE
        incoming[head] = next;
        head = (head + 1) & (depth - 1);
#else
        incoming[added++] = next;
#endif
        sum += value;
    }
#ifdef BENCH_RING_QUEUE
    sum += (uint64_t)incoming[head] + depth;
#else
    if (!ready) while (added) outgoing[ready++] = incoming[--added];
    sum += (uint64_t)outgoing[ready - 1] + depth;
    free(outgoing);
#endif
    printf("%" PRIu64 "\n", sum);
    free(incoming);
    return 0;
}
