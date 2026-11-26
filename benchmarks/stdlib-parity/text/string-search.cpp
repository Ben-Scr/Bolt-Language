#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

int main() {
    std::string block = "the quick brown fox jumps over the lazy dog. ";
    for (int i = 0; i < 5; ++i) block += block;
    const std::string text = block + "key-a-" + block + "key-b-" + block;
    const std::array<std::string_view, 4> keys{"key-a-", "key-b-", "key-c-", "dog"};
    std::uint32_t state = 13;
    std::int64_t total = 0;
    for (int i = 0; i < 750000; ++i) {
        state = state * 1664525u + 1013904223u;
        const auto index = text.find(keys[state >> 30], state & 1023u);
        total += index == text.npos ? -1 : static_cast<std::int64_t>(index);
    }
    std::cout << total << '\n';
}
