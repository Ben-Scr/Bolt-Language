#include <array>
#include <bit>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

int main() {
    const std::array<std::string_view, 8> input = {
        "-1513.125", "0.125", "3.4028234e30", "-1.1754944e-30",
        "2.2250738585072014", "17.00000095367431640625", "0.10000000149011612", "-2345.6789012345678"
    };
    std::uint64_t sum = 17;
    for (int i = 0; i < 1000000; ++i) {
        auto text = input[(i ^ (i >> 3)) & 7];
        auto end = text.data() + text.size();
        double a;
        float b;
        auto da = std::from_chars(text.data(), end, a);
        auto fb = std::from_chars(text.data(), end, b);
        if (da.ec != std::errc{} || da.ptr != end || fb.ec != std::errc{} || fb.ptr != end) return 1;
        sum = std::rotl(sum, 7) ^ std::bit_cast<std::uint64_t>(a)
              ^ (std::uint64_t(std::bit_cast<std::uint32_t>(b)) << 24) ^ i;
    }
    std::cout << sum << '\n';
}
