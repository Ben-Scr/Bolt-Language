#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> items;
    items.reserve(32769);
    for (int i = 0; i < 32768; ++i) items.push_back(i);
    std::uint64_t removed = 0;
    for (int step = 0; step < 12000; ++step) {
        items.insert(items.begin() + ((step * 117 + 257) & 32767), step);
        int at = (step * 251 + 11) & 32767;
        removed += items[at];
        items.erase(items.begin() + at);
    }
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < items.size(); ++i) sum += std::uint64_t(i + 1) * items[i];
    std::cout << removed << '\n' << sum << '\n';
}
