#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    std::vector<int> values;
    values.reserve(8192);
    for (int i = 0; i < 8192; ++i) values.push_back((i * 9751) & 8191);
    std::int64_t sum = 0;
    for (int i = 0; i < 4096; ++i) {
        auto match = std::find(values.begin(), values.end(), (i * 977) & 16383);
        sum += match == values.end() ? -1 : match - values.begin();
    }
    std::cout << sum << '\n';
}
