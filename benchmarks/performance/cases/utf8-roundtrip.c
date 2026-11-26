#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BENCH_UTF8_SIMD
#include "Utf8Block.h"
#endif

/* Validate and count Unicode scalars, including overlong, surrogate and range checks. */
static int decode_length(const unsigned char *text, size_t n, size_t *characters) {
    size_t pos = 0, count = 0;
#if defined(BENCH_UTF8_SIMD) && BOLT_HAS_UTF8_BLOCKS
    int checked_blocks = 0;
#endif
    while (pos < n) {
#if defined(BENCH_UTF8_SIMD) && BOLT_HAS_UTF8_BLOCKS
        if (!checked_blocks && pos >= 3 && n - pos >= 16) {
            checked_blocks = 1;
            const size_t blocks = (n - pos) / 16;
            size_t starts;
            if (!bolt_utf8_scan_blocks((const char *)text + pos,blocks,&starts)) return 0;
            count += starts;
            pos += blocks * 16;
            /* Revalidate the last scalar, including a possible truncated suffix. */
            --pos;
            while ((text[pos] & 0xc0) == 0x80) --pos;
            --count;
        }
#endif
        const unsigned first = text[pos++];
        if (first >= 128) {
            const unsigned width = first >= 0xc2 && first <= 0xdf ? 2 :
                first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
            if (!width || n - pos < width - 1) return 0;
            uint32_t scalar = first & (0x7fu >> width);
            for (unsigned i = 1; i < width; ++i) {
                const unsigned x = text[pos++];
                if ((x & 0xc0u) != 0x80u) return 0;
                scalar = (scalar << 6) | (x & 0x3fu);
            }
            if ((width == 3 && scalar < 0x800) || (width == 4 && scalar < 0x10000) ||
                (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff) return 0;
        }
        ++count;
    }
    *characters = count;
    return 1;
}

int main(void) {
    static const unsigned char part[] = "evt:\xc3\xa4/\xf0\x9f\x98\x80\n";
    const size_t n = 4096 * (sizeof(part) - 1);
    unsigned char *text = malloc(n), *bytes = malloc(n);
    if (!text || !bytes) { free(text); free(bytes); return 1; }
    for (size_t p = 0; p < n; p += sizeof(part) - 1) memcpy(text + p, part, sizeof(part) - 1);
    uint64_t sum = 0;
#ifdef BENCH_UTF8_KNOWN_EDIT
    size_t count;
    if (!decode_length(text, n, &count)) { free(text); free(bytes); return 2; }
#endif
    for (int round = 0; round < 768; ++round) {
        memcpy(bytes, text, n);
        const size_t offset = (size_t)((round * 43) & 4095) * (sizeof(part) - 1);
        bytes[offset] = (unsigned char)(65 + (round & 15));
#ifdef BENCH_UTF8_KNOWN_EDIT
        /* This caller knows the only write since memcpy. ASCII-to-ASCII
           substitutions preserve both validity and the cached scalar count. */
        if ((text[offset] | bytes[offset]) >= 128 && !decode_length(bytes, n, &count)) {
            free(text); free(bytes); return 2;
        }
        text[offset] = bytes[offset];
#else
        size_t count;
        if (!decode_length(bytes, n, &count)) { free(text); free(bytes); return 2; }
        memcpy(text, bytes, n);
#endif
        sum += count + bytes[offset];
    }
    for (size_t i = 0; i < n; ++i) sum += text[i];
    printf("%" PRIu64 "\n", sum);
    free(text);
    free(bytes);
    return 0;
}
