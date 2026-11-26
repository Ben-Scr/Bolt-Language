#pragma once
#include <bolt/Unicode.hpp>
#include <array>
#include <stdexcept>
#include <string>

namespace bolt::tests {
inline void verify_utf8_block_boundaries() {
    auto outcome = [](const std::string& bytes, bool blocks) {
        try {
            std::size_t count = 0;
            if (blocks) count = unicode::length(bytes);
            else for (std::size_t pos = 0; pos < bytes.size(); ++count) unicode::decode(bytes, pos);
            return std::to_string(count);
        } catch (const std::exception& e) { return std::string(e.what()); }
    };
    // Cover all vector alignments. Both the number of scalars and the first
    // diagnostic for malformed input must exactly match the checked decoder.
    const std::array<std::string,18> sequences{
        "\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80",
        "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf", "\x80", "\xc0\x80", "\xc1\xbf",
        "\xe0\x9f\xbf", "\xed\xa0\x80", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xfe", "\xf0\x90\x80", "\xe0\xa0\x7f"
    };
    // The first vector has no readable lookbehind. Exercise malformed
    // prefixes and a scalar which ends, or is truncated, at its right edge.
    for (std::size_t offset=0; offset<17; ++offset) for (const auto& part : sequences) {
        for (const auto length : {15u,16u,17u,31u,32u}) {
            auto bytes=std::string(offset,'a')+part+std::string(32,'b');
            bytes.resize(length);
            if (outcome(bytes,true)!=outcome(bytes,false)) throw std::runtime_error("UTF-8 first block mismatch");
        }
    }
    // Exercise the wide AVX2 path and the SSE2/NEON fallback on long input.
    // Vary boundaries at 16-byte vectors and 256-byte validation groups.
    for (const std::size_t boundary : {std::size_t{16},std::size_t{256},std::size_t{512},std::size_t{768}}) {
        for (int delta=-4; delta<5; ++delta) for (const auto& part : sequences) {
            const auto pos = static_cast<std::size_t>(static_cast<int>(boundary)+delta);
            std::string text(1059,'A');
            text.replace(pos,part.size(),part);
            if (outcome(text,true) != outcome(text,false)) throw std::runtime_error("UTF-8 long block mismatch");
        }
    }
    std::string multi;
    for (int i=0; i<300; ++i) multi += sequences[static_cast<std::size_t>(i)%7];
    for (std::size_t tail=0; tail<20; ++tail) {
        const auto shortened = multi.substr(0,multi.size()-tail);
        if (outcome(shortened,true) != outcome(shortened,false)) throw std::runtime_error("UTF-8 long suffix mismatch");
    }
    for (std::size_t offset=0; offset<32; ++offset) for (const auto& part : sequences) {
        const std::string prefix(48+offset,'x');
        for (const auto& suffix : {std::string{}, std::string(40,'x'), std::string("\x80\x80\x80")}) {
            const auto bytes=prefix+part+suffix;
            if (outcome(bytes,true)!=outcome(bytes,false))
                throw std::runtime_error("UTF-8 block validator disagreed with scalar decoder");
        }
    }
}
}
