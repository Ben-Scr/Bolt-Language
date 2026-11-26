#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__AVX2__)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#endif

#ifdef BENCH_WIDE_SEARCH
typedef uint64_t element;
static element key(uint32_t x) { return ((uint64_t)(x & 255) << 40) | x; }
#else
typedef uint32_t element;
static element key(uint32_t x) { return x; }
#endif

static int index_of(const element *values, int length, element needle) {
    int i = 0;
#if defined(__AVX2__)
    const int lanes = 32 / sizeof(element);
#ifdef BENCH_WIDE_SEARCH
    const __m256i wanted = _mm256_set1_epi64x((int64_t)needle);
#define EQUAL_VECTOR _mm256_cmpeq_epi64
#else
    const __m256i wanted = _mm256_set1_epi32((int32_t)needle);
#define EQUAL_VECTOR _mm256_cmpeq_epi32
#endif
    for (; length - i >= 4 * lanes; i += 4 * lanes) {
        const __m256i *p = (const __m256i *)(values + i);
        const __m256i a = EQUAL_VECTOR(_mm256_loadu_si256(p), wanted);
        const __m256i b = EQUAL_VECTOR(_mm256_loadu_si256(p + 1), wanted);
        const __m256i c = EQUAL_VECTOR(_mm256_loadu_si256(p + 2), wanted);
        const __m256i d = EQUAL_VECTOR(_mm256_loadu_si256(p + 3), wanted);
        if (_mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(a,b), _mm256_or_si256(c,d)))) break;
    }
    for (; length - i >= lanes; i += lanes) {
        const __m256i v = _mm256_loadu_si256((const __m256i *)(values + i));
        const unsigned mask = (unsigned)_mm256_movemask_epi8(EQUAL_VECTOR(v, wanted));
        if (mask) {
#ifdef _MSC_VER
            unsigned long bit;
            _BitScanForward(&bit, mask);
            return i + (int)(bit / sizeof(element));
#else
            return i + __builtin_ctz(mask) / (int)sizeof(element);
#endif
        }
    }
#undef EQUAL_VECTOR
#endif
    for (; i < length; i++) if (values[i] == needle) return i;
    return -1;
}

int main(void) {
    element values[8192];
    for (int i = 0; i < 8192; i++) values[i] = key((i * 9751) & 8191);
    int64_t sum = 0;
    for (int i = 0; i < 524288; i++) sum += index_of(values, 8192, key((i * 977) & 16383));
    printf("%" PRId64 "\n", sum);
    return 0;
}
