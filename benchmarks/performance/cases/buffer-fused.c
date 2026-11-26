#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

/* No reader can observe the original storage, and the element depends only on
   its index. Feed that value straight into the same reduction. */
int main(void) {
    uint64_t sum = 0;
    for (int32_t i = 0; i < 524288; ++i)
        sum += (uint64_t)((i ^ 117) & 65535);
    printf("%" PRIu64 "\n", sum);
    return 0;
}
