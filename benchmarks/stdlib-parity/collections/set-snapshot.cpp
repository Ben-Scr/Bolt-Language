#include <cstdint>
#include <iostream>
#include <numeric>
#include <unordered_set>
#include <vector>

int main() {
    std::unordered_set<int> keys;
    std::int64_t sum = 0;
    for (int round = 0; round < 512; ++round) {
        for (int i = 0; i < 4096; ++i) keys.insert((i ^ round) * 7);
        for (int i = 0; i < 4096; i += 3) keys.erase(i * 7);
        std::vector<int> copy(keys.begin(), keys.end());
        keys.clear();
        sum = std::accumulate(copy.begin(), copy.end(), sum);
    }
    std::cout << sum << '\n';
}
