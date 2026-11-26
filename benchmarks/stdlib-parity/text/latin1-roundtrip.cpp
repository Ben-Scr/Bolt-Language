#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::vector<unsigned char> source(512);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = static_cast<unsigned char>(i);
    std::uint64_t sum = 0;
    for (int i = 0; i < 8192; ++i) {
        source[i & 511] = (i >> 2) & 255;
        std::string utf8;
        utf8.reserve(source.size() * 2);
        for (auto c : source) {
            if (c < 128) utf8.push_back(static_cast<char>(c));
            else {
                utf8.push_back(static_cast<char>(192 | (c >> 6)));
                utf8.push_back(static_cast<char>(128 | (c & 63)));
            }
        }
        std::vector<unsigned char> copy;
        copy.reserve(utf8.size());
        for (std::size_t j = 0; j < utf8.size(); ++j) {
            auto first = static_cast<unsigned char>(utf8[j]);
            if (first < 128) copy.push_back(first);
            else {
                if (first < 194 || first > 195 || ++j == utf8.size()) return 1;
                auto second = static_cast<unsigned char>(utf8[j]);
                if ((second & 192) != 128) return 1;
                copy.push_back(static_cast<unsigned char>(((first & 3) << 6) | (second & 63)));
            }
        }
        sum += utf8.size() + copy[(i * 29) & 511];
    }
    std::cout << sum << '\n';
}
