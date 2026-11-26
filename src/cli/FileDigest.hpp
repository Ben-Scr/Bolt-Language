#pragma once
#include <bolt/Path.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bolt::cli_state {
// XXH64 with seed zero. The lanes consume complete 32-byte stripes only.
// File reads use a multiple of that size so the tail is finalized exactly once.
class Xxh64 {
    using Word = std::uint64_t;
    static constexpr Word p1 = 11400714785074694791ull, p2 = 14029467366897019727ull;
    static constexpr Word p3 = 1609587929392839161ull, p4 = 9650029242287828579ull;
    static constexpr Word p5 = 2870177450012600261ull;
    std::array<Word, 4> lanes_{p1 + p2, p2, 0, Word{0} - p1};
    Word consumed_ = 0;

    template<class T> static T little_endian(const char* p) {
        T value;
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(&value, p, sizeof(value));
        } else {
            value = 0;
            for (std::size_t i = 0; i < sizeof(value); ++i)
                value |= T(static_cast<unsigned char>(p[i])) << (i * 8);
        }
        return value;
    }
    static Word round(Word state, Word input) {
        return std::rotl(state + input * p2, 31) * p1;
    }
public:
    std::string_view stripes(std::string_view bytes) {
        while (bytes.size() >= 32) {
            for (std::size_t i = 0; i < lanes_.size(); ++i)
                lanes_[i] = round(lanes_[i], little_endian<Word>(bytes.data() + i * 8));
            consumed_ += 32;
            bytes.remove_prefix(32);
        }
        return bytes;
    }
    Word finish(std::string_view tail) const {
        Word h = p5;
        if (consumed_) {
            h = std::rotl(lanes_[0], 1) + std::rotl(lanes_[1], 7)
              + std::rotl(lanes_[2], 12) + std::rotl(lanes_[3], 18);
            for (auto lane : lanes_) h = (h ^ round(0, lane)) * p1 + p4;
        }
        h += consumed_ + tail.size();
        while (tail.size() >= 8) {
            h = std::rotl(h ^ round(0, little_endian<Word>(tail.data())), 27) * p1 + p4;
            tail.remove_prefix(8);
        }
        if (tail.size() >= 4) {
            h = std::rotl(h ^ little_endian<std::uint32_t>(tail.data()) * p1, 23) * p2 + p3;
            tail.remove_prefix(4);
        }
        for (unsigned char byte : tail) h = std::rotl(h ^ (byte * p5), 11) * p1;
        h ^= h >> 33;
        h *= p2;
        h ^= h >> 29;
        h *= p3;
        return h ^ (h >> 32);
    }
};

inline std::string format_digest(std::uint64_t value, bool legacy = false) {
    std::ostringstream out;
    if (!legacy) out << "xxh64-";
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}
inline std::string bytes_digest(std::string_view bytes) {
    Xxh64 hash;
    const auto tail = hash.stripes(bytes);
    return format_digest(hash.finish(tail));
}
inline std::string file_hash(const std::filesystem::path& path, bool legacy = false) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read build file: " + bolt::path_to_utf8(path));
    std::array<char, 32768> buffer;
    Xxh64 hash;
    std::string_view tail;
    std::uint64_t previous = 14695981039346656037ull;
    while (in.read(buffer.data(), buffer.size()) || in.gcount()) {
        std::string_view bytes(buffer.data(), static_cast<std::size_t>(in.gcount()));
        if (legacy) {
            for (unsigned char byte : bytes) previous = (previous ^ byte) * 1099511628211ull;
        } else {
            tail = hash.stripes(bytes);
        }
    }
    if (!in.eof() || in.bad()) throw std::runtime_error("Cannot finish reading build file: " + bolt::path_to_utf8(path));
    return format_digest(legacy ? previous : hash.finish(tail), legacy);
}
inline bool matches_recorded_digest(const std::filesystem::path& path, const std::string& expected) {
    // Unprefixed FNV-1a receipts from earlier compilers can still start a saved build.
    return file_hash(path, expected.size() == 16) == expected;
}
} // namespace bolt::cli_state
