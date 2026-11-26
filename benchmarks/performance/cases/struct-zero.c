#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct slot_state { uint32_t owner, flags; };
struct slot { struct slot_state state; uint64_t value; };

int main(void) {
    uint64_t checksum = 0;
    for (int32_t frame = 0; frame < 256; ++frame) {
        struct slot *slots = calloc((size_t)(65536 + (frame & 63)), sizeof(*slots));
        if (!slots) return 1;
        uint32_t cursor = (uint32_t)(frame + 1);
        for (int32_t j = 0; j < 4096; ++j) {
            int32_t k = (int32_t)((cursor ^ (uint32_t)(j * 77)) & 65535u);
            cursor = ((cursor << 3) ^ (cursor >> 13) ^ slots[k].state.owner ^
                      slots[k].state.flags ^ (uint32_t)slots[k].value ^ (uint32_t)j ^ 160171u) & 16777215u;
            slots[k].state.owner = (uint32_t)(j + frame + 1);
            slots[k].state.flags = cursor & 255u;
            slots[k].value = (uint64_t)j;
        }
        checksum += (uint64_t)cursor + slots[frame].value;
        free(slots);
    }
    printf("%" PRIu64 "\n", checksum);
    return 0;
}
