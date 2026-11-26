#include "text-fixture.hpp"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>

// File.ReadAllText rejects invalid UTF-8, strips an initial BOM, and retains embedded NULs.
static bool valid_utf8(std::string_view text) {
    auto data = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.size() - i >= 8) {
            std::uint64_t word;
            std::memcpy(&word, data + i, sizeof(word));
            if ((word & 0x8080808080808080ull) == 0) { i += 8; continue; }
        }
        const unsigned first = data[i++];
        if (first < 0x80) continue;
        if (first < 0xc2 || first > 0xf4) return false;
        const unsigned count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
        if (count > text.size() - i) return false;
        const unsigned second = data[i];
        if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) return false;
        for (unsigned j = 0; j < count; ++j)
            if ((data[i++] & 0xc0) != 0x80) return false;
    }
    return true;
}

int main() {
    auto text = text_fixture();
    std::ofstream out("text.bin", std::ios::binary | std::ios::trunc);
    out.write(text.data(), text.size());
    out.close();
    if (!out) return 1;
    std::int64_t sum = 0;
    for (int i = 0; i < 256; ++i) {
        std::ifstream in("text.bin", std::ios::binary | std::ios::ate);
        auto size = in.tellg();
        if (size <= 0) return 2;
        std::string content(static_cast<std::size_t>(size), '\0');
        in.seekg(0);
        if (!in.read(content.data(), size) || !valid_utf8(content)) return 3;
        if (content.starts_with("\xef\xbb\xbf")) content.erase(0, 3);
        if (content.empty()) return 4;
        sum += content.size() + static_cast<unsigned char>(content.front());
    }
    std::cout << sum << '\n';
}
