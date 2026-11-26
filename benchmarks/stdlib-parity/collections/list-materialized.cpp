#include <cstdint>
#include <iostream>
#include <numeric>
#include <vector>

int main() {
    constexpr int count = 2097151;
    std::vector<int> items;
    items.reserve(count);
    for (int i = 0; i < count; ++i) items.push_back((i ^ 123456) & 1023);
    auto snapshot = items;
    for (int i = 0; i < count; i += 193) items[i] ^= 511;
    std::cout << std::accumulate(snapshot.begin(), snapshot.end(), std::int64_t{0}) << '\n'
              << std::accumulate(items.begin(), items.end(), std::int64_t{0}) << '\n';
}
