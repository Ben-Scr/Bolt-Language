#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

int main() {
    std::array<unsigned char, 16387> left{}, right{};
    auto a = left.data() + 1, b = right.data() + 2;
    std::uint64_t sum = 0;
    for (int step = 0; step < 250000; ++step) {
        const std::size_t n = 1 + (step & 16383);
        auto value = static_cast<unsigned char>((step ^ (sum & 255)) & 255);
        std::memset(a, value, n);
        std::memcpy(b, a, n);
        b[n - 1] = value ^ 128;
        int cmp = std::memcmp(a, b, n);
        std::memmove(b + 1, b, n);
        sum += b[n] + ((cmp > 0) - (cmp < 0) + 1);
    }
    std::cout << sum << '\n';
}
