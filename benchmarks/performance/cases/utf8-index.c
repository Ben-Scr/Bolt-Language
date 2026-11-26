#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build the same UTF-8 string and decode once before repeated scalar indexing. */
static int next_scalar(const unsigned char *text, size_t length, size_t *at, uint32_t *out) {
    unsigned c = text[(*at)++];
    if (c < 128) { *out = c; return 1; }
    unsigned width = c >= 0xc2 && c <= 0xdf ? 2 :
                     c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!width || length - *at < width - 1) return 0;
    uint32_t value = c & (0x7fu >> width);
    for (unsigned i = 1; i < width; ++i) {
        c = text[(*at)++];
        if ((c & 0xc0u) != 0x80u) return 0;
        value = (value << 6) | (c & 0x3fu);
    }
    if ((width == 3 && value < 0x800) || (width == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) return 0;
    *out = value;
    return 1;
}

int main(void) {
    static const unsigned char part[] = "level=Gr\xc3\xbcn/\xf0\x9f\x98\x80;";
    size_t length = 128 * (sizeof(part) - 1);
    unsigned char *text = malloc(length);
    uint32_t *chars = malloc(length * sizeof(*chars));
    if (!text || !chars) { free(text); free(chars); return 1; }
    for (size_t i = 0; i < length; i += sizeof(part) - 1) memcpy(text + i, part, sizeof(part) - 1);
    int32_t count = 0;
    for (size_t pos = 0; pos < length; ++count)
        if (!next_scalar(text, length, &pos, &chars[count])) { free(text); free(chars); return 2; }
    uint64_t sum = 0;
    for (int32_t round = 0; round < 384; ++round)
        for (int32_t i = 0; i < count; ++i)
            sum += chars[(i * 29 + round) % count];
    printf("%" PRIu64 "\n", sum);
    free(chars);
    free(text);
    return 0;
}
