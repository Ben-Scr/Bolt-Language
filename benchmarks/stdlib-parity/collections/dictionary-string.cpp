#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

int main() {
    std::array<std::string, 1024> names;
    std::unordered_map<std::string, int> values;
    for (int i = 0; i < 1024; ++i) {
        names[i] = "scene/entity/component-" + std::to_string(i);
        values.emplace(names[i], i * 7 + 3);
    }
    std::uint64_t sum = 0;
    for (int round = 0; round < 1024; ++round)
        for (int i = 0; i < 1024; ++i)
            sum += values.at(names[(i * 31 + round) & 1023]);
    std::cout << sum << '\n';
}
