#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#endif

// Isolate AVX2 code so the runtime itself can still start on baseline x64.
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#define BOLT_AVX2_KERNEL __declspec(noinline)
#define BOLT_AVX2_INLINE __forceinline
#else
#define BOLT_AVX2_KERNEL __attribute__((target("avx2"), noinline))
#define BOLT_AVX2_INLINE inline __attribute__((target("avx2"), always_inline))
#endif
#endif

namespace bolt::native_abi {
#ifdef BOLT_AVX2_KERNEL
inline bool has_avx2() noexcept {
#if defined(_MSC_VER)
    static const bool available = [] {
        int cpu[4]{};
        __cpuid(cpu, 0);
        if (cpu[0] < 7) return false;
        __cpuidex(cpu, 1, 0);
        constexpr int avx_osxsave = (1 << 28) | (1 << 27);
        if ((cpu[2] & avx_osxsave) != avx_osxsave || (_xgetbv(0) & 6) != 6) return false;
        __cpuidex(cpu, 7, 0);
        return (cpu[1] & (1 << 5)) != 0;
    }();
    return available;
#else
    return __builtin_cpu_supports("avx2");
#endif
}

template<class T> BOLT_AVX2_INLINE __m256i search_block(const std::byte* data, __m256i key) noexcept {
    const auto values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
    if constexpr (sizeof(T) == 2) return _mm256_cmpeq_epi16(values, key);
    else if constexpr (sizeof(T) == 4) return _mm256_cmpeq_epi32(values, key);
    else return _mm256_cmpeq_epi64(values, key);
}

// Skip groups without a match. The narrow scan below resolves the first
// matching element and handles the tail without reading past the sequence.
template<class T> BOLT_AVX2_KERNEL std::size_t skip_unmatched_groups(
    const std::byte* data, std::size_t count, T wanted) noexcept {
    constexpr auto group = 128 / sizeof(T);
    __m256i key;
    if constexpr (sizeof(T) == 2) key = _mm256_set1_epi16(std::bit_cast<std::int16_t>(wanted));
    else if constexpr (sizeof(T) == 4) key = _mm256_set1_epi32(std::bit_cast<std::int32_t>(wanted));
    else key = _mm256_set1_epi64x(std::bit_cast<std::int64_t>(wanted));
    std::size_t i = 0;
    for (; count - i >= group; i += group) {
        const auto* p = data + i * sizeof(T);
        const auto low = _mm256_or_si256(search_block<T>(p, key), search_block<T>(p + 32, key));
        const auto high = _mm256_or_si256(search_block<T>(p + 64, key), search_block<T>(p + 96, key));
        if (_mm256_movemask_epi8(_mm256_or_si256(low, high))) break;
    }
    return i;
}
#endif
template<class T> std::size_t find_integer(const std::byte* data, std::size_t count, T wanted) noexcept {
    std::size_t i = 0;
    if constexpr (sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8) {
#ifdef BOLT_AVX2_KERNEL
        if (count >= 256 / sizeof(T) && has_avx2()) i = skip_unmatched_groups(data, count, wanted);
#endif
        constexpr auto lanes = 16 / sizeof(T);
#if defined(__SSE2__) || defined(_M_X64)
        __m128i key;
        if constexpr (sizeof(T) == 2) key = _mm_set1_epi16(std::bit_cast<std::int16_t>(wanted));
        else if constexpr (sizeof(T) == 4) key = _mm_set1_epi32(std::bit_cast<std::int32_t>(wanted));
        else key = _mm_set1_epi64x(std::bit_cast<std::int64_t>(wanted));
        const auto matches = [&](std::size_t at) {
            __m128i values;
            std::memcpy(&values, data + at * sizeof(T), sizeof(values));
            if constexpr (sizeof(T) == 2) return _mm_cmpeq_epi16(values, key);
            else if constexpr (sizeof(T) == 4) return _mm_cmpeq_epi32(values, key);
            else {
                const auto halves = _mm_cmpeq_epi32(values, key);
                return _mm_and_si128(halves, _mm_shuffle_epi32(halves, _MM_SHUFFLE(2, 3, 0, 1)));
            }
        };
        // One branch covers a full cache line. On a hit, the narrow loop
        // below resolves the first block and first matching lane.
        for (; count - i >= 4 * lanes; i += 4 * lanes) {
            const auto lower = _mm_or_si128(matches(i), matches(i + lanes));
            const auto upper = _mm_or_si128(matches(i + 2 * lanes), matches(i + 3 * lanes));
            if (_mm_movemask_epi8(_mm_or_si128(lower, upper))) break;
        }
        for (; count - i >= lanes; i += lanes) {
            const auto mask = static_cast<unsigned>(_mm_movemask_epi8(matches(i)));
            if (mask) return i + std::countr_zero(mask) / sizeof(T);
        }
#elif defined(__ARM_NEON) && defined(__aarch64__)
        const auto matches = [&](std::size_t at) {
            uint8x16_t raw;
            std::memcpy(&raw, data + at * sizeof(T), sizeof(raw));
            if constexpr (sizeof(T) == 2)
                return vreinterpretq_u8_u16(vceqq_u16(vreinterpretq_u16_u8(raw), vdupq_n_u16(wanted)));
            else if constexpr (sizeof(T) == 4)
                return vreinterpretq_u8_u32(vceqq_u32(vreinterpretq_u32_u8(raw), vdupq_n_u32(wanted)));
            else
                return vreinterpretq_u8_u64(vceqq_u64(vreinterpretq_u64_u8(raw), vdupq_n_u64(wanted)));
        };
        for (; count - i >= 4 * lanes; i += 4 * lanes) {
            const auto lower = vorrq_u8(matches(i), matches(i + lanes));
            const auto upper = vorrq_u8(matches(i + 2 * lanes), matches(i + 3 * lanes));
            if (vmaxvq_u8(vorrq_u8(lower, upper))) break;
        }
        for (; count - i >= lanes; i += lanes)
            if (vmaxvq_u8(matches(i))) break;
#endif
    }
    for (; i < count; ++i) {
        T item;
        std::memcpy(&item, data + i * sizeof(T), sizeof(T));
        if (item == wanted) break;
    }
    return i;
}
}
#ifdef BOLT_AVX2_KERNEL
#undef BOLT_AVX2_KERNEL
#undef BOLT_AVX2_INLINE
#endif
