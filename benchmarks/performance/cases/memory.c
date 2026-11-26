#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_range(const void *a, const void *b, size_t n) {
    if (n && (!a || !b || (uintptr_t)a > UINTPTR_MAX - (n - 1)
                        || (uintptr_t)b > UINTPTR_MAX - (n - 1))) {
        abort();
    }
}

static void checked_copy(void *to, const void *from, size_t n) {
    check_range(to, from, n);
    uintptr_t a = (uintptr_t)to, b = (uintptr_t)from;
    if ((a >= b ? a - b : b - a) < n) abort();
    if (n) memcpy(to, from, n);
}

int main(void) {
    unsigned char *left = calloc(16387, 1), *right = calloc(16387, 1);
    if (!left || !right) return 1;
    unsigned char *a = left + 1, *b = right + 2;
    uint64_t result = 0;
    for (int step = 0; step < 250000; ++step) {
        size_t n = 1 + (size_t)(step & 16383);
        unsigned char value = (unsigned char)((step ^ (int)(result & 255)) & 255);
        check_range(a, a, n);
        memset(a, value, n);
        checked_copy(b, a, n);
        b[n - 1] = (unsigned char)(value ^ 128);
        check_range(a, b, n);
        int cmp = memcmp(a, b, n);
        int sign = (cmp > 0) - (cmp < 0);
        check_range(b + 1, b, n);
        memmove(b + 1, b, n);
        result += (uint64_t)b[n] + (uint64_t)(sign + 1);
    }
    printf("%" PRIu64 "\n", result);
    free(right);
    free(left);
    return 0;
}
