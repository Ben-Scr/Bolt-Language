#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(BENCH_VECTOR_SEARCH) && (defined(__SSE2__) || defined(_M_X64))
#include <emmintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#elif defined(BENCH_VECTOR_SEARCH) && defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

static int32_t index_of(const int32_t *values, int32_t length, int32_t needle) {
    int32_t i = 0;
#if defined(BENCH_VECTOR_SEARCH) && (defined(__SSE2__) || defined(_M_X64))
    const __m128i wanted = _mm_set1_epi32(needle);
#ifdef BENCH_SEARCH_UNROLL4
    for (; length - i >= 16; i += 16) {
        __m128i a = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(values+i)), wanted);
        __m128i b = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(values+i+4)), wanted);
        __m128i c = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(values+i+8)), wanted);
        __m128i d = _mm_cmpeq_epi32(_mm_loadu_si128((const __m128i *)(values+i+12)), wanted);
        if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(a,b),_mm_or_si128(c,d)))) break;
    }
#endif
    for (; length - i >= 4; i += 4) {
        const __m128i block = _mm_loadu_si128((const __m128i *)(values + i));
        const __m128i equal = _mm_cmpeq_epi32(block, wanted);
        const unsigned mask = (unsigned)_mm_movemask_epi8(equal);
        if (mask) {
#if defined(_MSC_VER) && !defined(__clang__)
            unsigned long bit;
            _BitScanForward(&bit,mask);
            return i + (int32_t)(bit / 4);
#elif defined(__GNUC__) || defined(__clang__)
            return i + (int32_t)(__builtin_ctz(mask) / 4);
#else
            for (int32_t lane=0; lane<4; ++lane) if (mask & (1u << (4*lane))) return i+lane;
#endif
        }
    }
#elif defined(BENCH_VECTOR_SEARCH) && defined(__ARM_NEON) && defined(__aarch64__)
    const int32x4_t wanted = vdupq_n_s32(needle);
#ifdef BENCH_SEARCH_UNROLL4
    for (; length - i >= 16; i += 16) {
        uint32x4_t a = vceqq_s32(vld1q_s32(values+i), wanted);
        uint32x4_t b = vceqq_s32(vld1q_s32(values+i+4), wanted);
        uint32x4_t c = vceqq_s32(vld1q_s32(values+i+8), wanted);
        uint32x4_t d = vceqq_s32(vld1q_s32(values+i+12), wanted);
        if (vmaxvq_u32(vorrq_u32(vorrq_u32(a,b),vorrq_u32(c,d)))) break;
    }
#endif
    for (; length - i >= 4; i += 4)
        if (vmaxvq_u32(vceqq_s32(vld1q_s32(values + i), wanted))) break;
#endif
    for (; i < length; ++i) if (values[i] == needle) return i;
    return -1;
}

int main(void) {
    const int32_t length = 8192;
    int32_t *values = malloc((size_t)length * sizeof(*values));
    if (!values) return 1;
    for (int32_t i = 0; i < length; ++i) values[i] = (i * 9751) & 8191;
    int64_t sum = 0;
    for (int32_t i = 0; i < 4096; ++i) sum += index_of(values, length, (i * 977) & 16383);
    printf("%" PRId64 "\n", sum);
    free(values);
    return 0;
}
