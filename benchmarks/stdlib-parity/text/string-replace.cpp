#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

int main() {
    std::string text = "user=id; path=/home/user; group=users; ";
    for (int i = 0; i < 6; ++i) text += text;
    const std::array<std::string_view, 4> words{"record", "x", "", "user-name"};
    std::int64_t total = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string result;
        result.reserve(text.size());
        const auto word = words[i & 3];
        std::size_t start = 0;
        for (auto at = text.find("user"); at != text.npos; at = text.find("user", start)) {
            result.append(text, start, at - start);
            result.append(word);
            start = at + 4;
        }
        result.append(text, start);
        total += static_cast<std::int64_t>(result.size()) + static_cast<unsigned char>(result[i & 15]);
    }
    std::cout << total << '\n';
}
