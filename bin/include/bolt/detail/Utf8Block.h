#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(__ARM_NEON) && defined(__aarch64__))
#define BOLT_HAS_UTF8_BLOCKS 1
#if defined(__SSE2__) || defined(_M_X64)
typedef __m128i bolt_utf8_block;
static inline bolt_utf8_block bolt_utf8_fill(uint8_t x) { return _mm_set1_epi8((char)x); }
static inline bolt_utf8_block bolt_utf8_eq(bolt_utf8_block a, uint8_t b) { return _mm_cmpeq_epi8(a, bolt_utf8_fill(b)); }
static inline bolt_utf8_block bolt_utf8_bits(bolt_utf8_block a, uint8_t b) { return _mm_and_si128(a, bolt_utf8_fill(b)); }
static inline bolt_utf8_block bolt_utf8_both(bolt_utf8_block a, bolt_utf8_block b) { return _mm_and_si128(a,b); }
static inline bolt_utf8_block bolt_utf8_either(bolt_utf8_block a, bolt_utf8_block b) { return _mm_or_si128(a,b); }
static inline bolt_utf8_block bolt_utf8_different(bolt_utf8_block a, bolt_utf8_block b) { return _mm_xor_si128(a,b); }
static inline bolt_utf8_block bolt_utf8_without(bolt_utf8_block a, bolt_utf8_block b) { return _mm_andnot_si128(b,a); }
static inline bolt_utf8_block bolt_utf8_illegal_high(bolt_utf8_block a) { return _mm_cmpeq_epi8(_mm_max_epu8(a,bolt_utf8_fill(0xf5)),a); }
static inline int bolt_utf8_high_bit(bolt_utf8_block a) { return _mm_movemask_epi8(a) != 0; }
static inline unsigned bolt_utf8_count_set(bolt_utf8_block a) {
    const bolt_utf8_block pair = _mm_sad_epu8(bolt_utf8_bits(a, 1), _mm_setzero_si128());
    return _mm_cvtsi128_si32(pair) + _mm_cvtsi128_si32(_mm_srli_si128(pair, 8));
}
#else
typedef uint8x16_t bolt_utf8_block;
static inline bolt_utf8_block bolt_utf8_fill(uint8_t x) { return vdupq_n_u8(x); }
static inline bolt_utf8_block bolt_utf8_eq(bolt_utf8_block a, uint8_t b) { return vceqq_u8(a, bolt_utf8_fill(b)); }
static inline bolt_utf8_block bolt_utf8_bits(bolt_utf8_block a, uint8_t b) { return vandq_u8(a, bolt_utf8_fill(b)); }
static inline bolt_utf8_block bolt_utf8_both(bolt_utf8_block a, bolt_utf8_block b) { return vandq_u8(a,b); }
static inline bolt_utf8_block bolt_utf8_either(bolt_utf8_block a, bolt_utf8_block b) { return vorrq_u8(a,b); }
static inline bolt_utf8_block bolt_utf8_different(bolt_utf8_block a, bolt_utf8_block b) { return veorq_u8(a,b); }
static inline bolt_utf8_block bolt_utf8_without(bolt_utf8_block a, bolt_utf8_block b) { return vbicq_u8(a,b); }
static inline bolt_utf8_block bolt_utf8_illegal_high(bolt_utf8_block a) { return vcgeq_u8(a, bolt_utf8_fill(0xf5)); }
static inline int bolt_utf8_high_bit(bolt_utf8_block a) { return vmaxvq_u8(a) >= 128; }
static inline unsigned bolt_utf8_count_set(bolt_utf8_block a) { return vaddvq_u8(bolt_utf8_bits(a,1)); }
#endif
static inline bolt_utf8_block bolt_utf8_load_block(const char* p) {
    bolt_utf8_block value;
    memcpy(&value, p, sizeof(value));
    return value;
}

// A continuation is required one position after every leader, two after a
// 3/4-byte leader and three after a 4-byte leader.
static inline int bolt_utf8_count_nonascii(bolt_utf8_block input, bolt_utf8_block before,
                                          bolt_utf8_block before2, bolt_utf8_block before3) {
    const bolt_utf8_block expected = bolt_utf8_either(bolt_utf8_eq(bolt_utf8_bits(before, 0xc0), 0xc0), bolt_utf8_either(
        bolt_utf8_eq(bolt_utf8_bits(before2, 0xe0), 0xe0), bolt_utf8_eq(bolt_utf8_bits(before3, 0xf0), 0xf0)));
    const bolt_utf8_block cont = bolt_utf8_eq(bolt_utf8_bits(input, 0xc0), 0x80);
    bolt_utf8_block bad = bolt_utf8_either(bolt_utf8_different(expected, cont), bolt_utf8_either(bolt_utf8_eq(bolt_utf8_bits(input, 0xfe), 0xc0), bolt_utf8_illegal_high(input)));
    // The second byte after E0/ED/F0/F4 distinguishes overlong encodings,
    // UTF-16 surrogates, and scalar values beyond U+10FFFF.
    const bolt_utf8_block below_a0 = bolt_utf8_eq(bolt_utf8_bits(input, 0x20), 0);
    const bolt_utf8_block below_90 = bolt_utf8_eq(bolt_utf8_bits(input, 0x30), 0);
    bad = bolt_utf8_either(bad, bolt_utf8_either(bolt_utf8_both(bolt_utf8_eq(before, 0xe0), below_a0), bolt_utf8_without(bolt_utf8_eq(before, 0xed), below_a0)));
    bad = bolt_utf8_either(bad, bolt_utf8_either(bolt_utf8_both(bolt_utf8_eq(before, 0xf0), below_90), bolt_utf8_without(bolt_utf8_eq(before, 0xf4), below_90)));
    return bolt_utf8_high_bit(bad) ? -1 : 16 - (int)bolt_utf8_count_set(cont);
}
// The first block has no preceding bytes. Shift an ASCII prefix into the
// vector instead of peeling scalars or reading before the start of the string.
static inline int bolt_utf8_first_block_count(const char* p) {
    const bolt_utf8_block input = bolt_utf8_load_block(p);
    if (!bolt_utf8_high_bit(input)) return 16;
#if defined(__SSE2__) || defined(_M_X64)
    return bolt_utf8_count_nonascii(input, _mm_slli_si128(input,1),
                                   _mm_slli_si128(input,2), _mm_slli_si128(input,3));
#else
    const bolt_utf8_block zero = vdupq_n_u8(0);
    return bolt_utf8_count_nonascii(input, vextq_u8(zero,input,15),
                                   vextq_u8(zero,input,14), vextq_u8(zero,input,13));
#endif
}
// Subsequent blocks have three readable lookbehind bytes.
static inline int bolt_utf8_block_count(const char* p) {
    const bolt_utf8_block input = bolt_utf8_load_block(p);
    if (!bolt_utf8_high_bit(input)) {
        const unsigned char* u = (const unsigned char*)p;
        return (u[-1] >= 0xc0 || u[-2] >= 0xe0 || u[-3] >= 0xf0) ? -1 : 16;
    }
    return bolt_utf8_count_nonascii(input,bolt_utf8_load_block(p-1),bolt_utf8_load_block(p-2),bolt_utf8_load_block(p-3));
}
#else
#define BOLT_HAS_UTF8_BLOCKS 0
static inline int bolt_utf8_first_block_count(const char* p) { (void)p; return -1; }
static inline int bolt_utf8_block_count(const char* p) { (void)p; return -1; }
#endif

#if (defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC)) || (defined(__SSE2__) && (defined(__x86_64__) || defined(__i386__)) && !defined(_MSC_VER) && (defined(__GNUC__) || defined(__clang__)))
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#if defined(__clang__)
#define BOLT_UTF8_AVX2 __attribute__((target("avx2"), noinline))
#else
#define BOLT_UTF8_AVX2 __declspec(noinline)
#endif
#else
#include <cpuid.h>
#define BOLT_UTF8_AVX2 __attribute__((target("avx2")))
#endif
#define BOLT_CAN_VECTOR32 1
static inline int bolt_cpu_vector32(void) {
#if defined(_MSC_VER)
    int regs[4];
    __cpuid(regs,0);
    if (regs[0] < 7) return 0;
    __cpuidex(regs,1,0);
    if ((regs[2] & ((1<<27)|(1<<28))) != ((1<<27)|(1<<28)) || (_xgetbv(0) & 6) != 6) return 0;
    __cpuidex(regs,7,0);
    return (regs[1] & (1<<5)) != 0;
#else
    unsigned a,b,c,d;
    if (__get_cpuid_max(0,0) < 7) return 0;
    __cpuid(1,a,b,c,d);
    if ((c & ((1u<<27)|(1u<<28))) != ((1u<<27)|(1u<<28))) return 0;
    unsigned low,high;
    __asm__ volatile("xgetbv" : "=a"(low),"=d"(high) : "c"(0));
    if ((low & 6u) != 6u) return 0;
    __cpuid_count(7,0,a,b,c,d);
    return (b & (1u<<5)) != 0;
#endif
}
static inline int bolt_utf8_has_vector32(void) {
#ifdef __cplusplus
    static const int supported = bolt_cpu_vector32();
    return supported;
#elif defined(_MSC_VER)
    static volatile long cached = 0;
    long state = _InterlockedCompareExchange(&cached,0,0);
    if (!state) {
        state = 1 + bolt_cpu_vector32();
        _InterlockedExchange(&cached,state);
    }
    return state == 2;
#else
    static int cached = 0;
    int state = __atomic_load_n(&cached,__ATOMIC_RELAXED);
    if (!state) {
        state = 1 + bolt_cpu_vector32();
        __atomic_store_n(&cached,state,__ATOMIC_RELAXED);
    }
    return state == 2;
#endif
}
/* Caller supplies a scalar boundary, three readable bytes before p, and
   at least 32*vectors bytes. SIMD lanes carry continuation requirements
   across adjacent vectors using the preceding three byte positions. */
BOLT_UTF8_AVX2 static int bolt_utf8_vector32(const char* p,size_t vectors,size_t* starts) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i c0 = _mm256_set1_epi8((char)0xc0), e0 = _mm256_set1_epi8((char)0xe0), f0 = _mm256_set1_epi8((char)0xf0);
    const __m256i fe = _mm256_set1_epi8((char)0xfe), f5 = _mm256_set1_epi8((char)0xf5);
    __m256i totals = zero;
    *starts = vectors * 32;
    while (vectors) {
        const size_t group = vectors < 8 ? vectors : 8;
        __m256i errors = zero, counts = zero;
        for (size_t i = 0; i < group; ++i,p += 32) {
            const __m256i in = _mm256_loadu_si256((const __m256i*)p);
            if (!_mm256_movemask_epi8(in)) {
                const unsigned char* u = (const unsigned char*)p;
                if (u[-1] >= 0xc0 || u[-2] >= 0xe0 || u[-3] >= 0xf0) return 0;
                continue;
            }
            const __m256i prev = _mm256_loadu_si256((const __m256i*)(p-1));
            const __m256i two = _mm256_loadu_si256((const __m256i*)(p-2));
            const __m256i three = _mm256_loadu_si256((const __m256i*)(p-3));
            const __m256i expected = _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_and_si256(prev,c0),c0),
                _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_and_si256(two,e0),e0),_mm256_cmpeq_epi8(_mm256_and_si256(three,f0),f0)));
            const __m256i cont = _mm256_cmpgt_epi8(c0,in);
            counts = _mm256_sub_epi8(counts,cont);
            __m256i bad = _mm256_or_si256(_mm256_xor_si256(expected,cont),
                _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_and_si256(in,fe),c0),_mm256_cmpeq_epi8(_mm256_max_epu8(in,f5),in)));
            const __m256i under_a0 = _mm256_cmpeq_epi8(_mm256_and_si256(in,_mm256_set1_epi8(0x20)),zero);
            const __m256i under_90 = _mm256_cmpeq_epi8(_mm256_and_si256(in,_mm256_set1_epi8(0x30)),zero);
            bad = _mm256_or_si256(bad,_mm256_or_si256(_mm256_and_si256(_mm256_cmpeq_epi8(prev,e0),under_a0),
                    _mm256_andnot_si256(under_a0,_mm256_cmpeq_epi8(prev,_mm256_set1_epi8((char)0xed)))));
            bad = _mm256_or_si256(bad,_mm256_or_si256(_mm256_and_si256(_mm256_cmpeq_epi8(prev,f0),under_90),
                    _mm256_andnot_si256(under_90,_mm256_cmpeq_epi8(prev,_mm256_set1_epi8((char)0xf4)))));
            errors = _mm256_or_si256(errors,bad);
        }
        if (!_mm256_testz_si256(errors,errors)) return 0;
        totals = _mm256_add_epi64(totals,_mm256_sad_epu8(counts,zero));
        vectors -= group;
    }
    uint64_t sums[4];
    memcpy(sums,&totals,sizeof(sums));
    *starts -= (size_t)(sums[0]+sums[1]+sums[2]+sums[3]);
    return 1;
}
#undef BOLT_UTF8_AVX2
#else
#define BOLT_CAN_VECTOR32 0
#endif
static inline int bolt_utf8_scan_blocks(const char* p,size_t blocks,size_t* starts) {
    *starts = 0;
#if BOLT_CAN_VECTOR32
    if (blocks >= 32 && bolt_utf8_has_vector32()) {
        const size_t pairs = blocks / 2;
        if (!bolt_utf8_vector32(p,pairs,starts)) return 0;
        p += pairs*32;
        blocks %= 2;
    }
#endif
    for (size_t i=0; i<blocks; ++i) {
        const int count = bolt_utf8_block_count(p+16*i);
        if (count < 0) return 0;
        *starts += (size_t)count;
    }
    return 1;
}
#undef BOLT_CAN_VECTOR32
