#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const unsigned char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint32_t hash_bytes(uint32_t hash, const unsigned char *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT32_C(16777619);
    return hash;
}

static void encode_hex(const unsigned char *data, size_t size, int uppercase, unsigned char *out) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
}

static void encode_base64(const unsigned char *data, size_t size, unsigned char *out) {
    for (size_t i = 0; i < size; i += 3) {
        unsigned n = (unsigned)data[i] << 16;
        if (i + 1 < size) n |= (unsigned)data[i + 1] << 8;
        if (i + 2 < size) n |= data[i + 2];
        *out++ = alphabet[n >> 18];
        *out++ = alphabet[(n >> 12) & 63];
        *out++ = i + 1 < size ? alphabet[(n >> 6) & 63] : '=';
        *out++ = i + 2 < size ? alphabet[n & 63] : '=';
    }
}

static int decode_base64(const unsigned char *text, size_t len, const signed char *values,
                         unsigned char *out, size_t capacity) {
    unsigned char quartet[4];
    size_t count = 0, written = 0;
    int padded = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (padded || (c != '=' && values[c] < 0)) return 0;
        quartet[count++] = c;
        if (count < 4) continue;
        count = 0;
        if (quartet[0] == '=' || quartet[1] == '=' || (quartet[2] == '=' && quartet[3] != '=')) return 0;
        size_t bytes = 3u - (quartet[2] == '=') - (quartet[3] == '=');
        if (capacity - written < bytes) return 0;
        unsigned n = (unsigned)values[quartet[0]] << 18 | (unsigned)values[quartet[1]] << 12;
        if (bytes >= 2) n |= (unsigned)values[quartet[2]] << 6;
        if (bytes == 3) n |= (unsigned)values[quartet[3]];
        out[written++] = (unsigned char)(n >> 16);
        if (bytes >= 2) out[written++] = (unsigned char)(n >> 8);
        if (bytes == 3) out[written++] = (unsigned char)n;
        padded = bytes < 3;
    }
    return !count && written == capacity;
}

int main(void) {
    signed char values[256];
    for (int i = 0; i < 256; ++i) values[i] = -1;
    for (int i = 0; i < 64; ++i) values[alphabet[i]] = (signed char)i;
    uint32_t hash = UINT32_C(2166136261);
    for (int pass = 0; pass < 18; ++pass) {
        size_t size = 2046u + (unsigned)pass % 3u, b64size = 4 * ((size + 2) / 3);
        unsigned char *data = malloc(size), *hex = malloc(2 * size), *b64 = malloc(b64size);
        unsigned char *decoded = malloc(size);
        if (!data || !hex || !b64 || !decoded) return 1;
        for (size_t i = 0; i < size; ++i)
            data[i] = (unsigned char)((i * 131 + (i >> 3) + (unsigned)pass * 17) & 255);
        encode_hex(data, size, !(pass & 1), hex);
        encode_base64(data, size, b64);
        if (!decode_base64(b64, b64size, values, decoded, size)) return 2;
        hash = hash_bytes(hash, hex, 2 * size);
        hash = hash_bytes(hash, b64, b64size);
        hash = hash_bytes(hash, decoded, size);
        free(decoded);
        free(b64);
        free(hex);
        free(data);
    }
    printf("%" PRIu32 "\n", hash);
    return 0;
}
