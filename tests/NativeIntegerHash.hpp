#pragma once
#include <bolt/native/ABI.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <memory>

namespace bolt::tests {
namespace integer_hash {
inline std::size_t reference(const unsigned char* value, std::size_t width) {
    std::size_t result = 0;
    for (std::size_t i = 0; i < width; ++i) {
        result ^= std::size_t{value[i]} + std::size_t{0x9e3779b9U} + (result << 6) + (result >> 2);
    }
    return result;
}
inline std::uint8_t equal_callback(BrContext*, const void*, const void*) { return 1; }
inline std::uint64_t hash_callback(BrContext*, const void*) { return 0x1728394051627384ULL; }
}

inline void verify_native_integer_hash() {
    using namespace integer_hash;
    std::unique_ptr<BrContext, decltype(&br_destroy)> context(br_create(0, 64), br_destroy);
    if (!context) throw std::runtime_error("cannot create native hash test context");
    auto* ctx = context.get();
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    std::array<unsigned char, 9> storage{};
    unsigned char* p = storage.data() + 1;
    for (auto kind : {bolt::native_abi::Signed, bolt::native_abi::Unsigned}) {
        for (std::size_t width : {1, 2, 4, 8}) {
            auto* type = br_type(ctx, "integer-hash-test", kind, width);
            if (!type || ctx->failed) throw std::runtime_error("cannot create integer type descriptor");
            const unsigned limit = width == 1 ? 256 : width == 2 ? 65536 : 4096;
            for (unsigned index = 0; index < limit; ++index) {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                std::uint64_t bits = width <= 2 ? index : state;
                if (width > 2 && index < 4) {
                    const unsigned top = static_cast<unsigned>(width * 8 - 1);
                    const std::uint64_t sign = std::uint64_t{1} << top;
                    const std::array<std::uint64_t, 4> boundaries{0, sign - 1, sign, UINT64_MAX};
                    bits = boundaries[index];
                }
                for (std::size_t i = 0; i < width; ++i) p[i] = static_cast<unsigned char>(bits >> (i * 8));
                if (br_value_hash(ctx, nullptr, type, p) != reference(p, width) || ctx->failed)
                    throw std::runtime_error("integer hash differs from byte-wise reference");
            }
            // A registered contract takes precedence even on a scalar descriptor.
            br_value_contract(ctx, type, reinterpret_cast<void*>(equal_callback), nullptr);
            if (br_value_hash(ctx, nullptr, type, p) != 0)
                throw std::runtime_error("equality-only contract must produce a constant hash");
            br_value_contract(ctx, type, nullptr, reinterpret_cast<void*>(hash_callback));
            if (br_value_hash(ctx, nullptr, type, p) != hash_callback(ctx, p))
                throw std::runtime_error("custom hash contract was bypassed");
        }
    }
    if (br_value_hash(nullptr, nullptr, nullptr, nullptr) != 0)
        throw std::runtime_error("null context must skip native hashing");
    br_fail(ctx, nullptr, "test pending error");
    if (br_value_hash(ctx, nullptr, nullptr, nullptr) != 0)
        throw std::runtime_error("pending failure must skip native hashing");
}
} // namespace bolt::tests
