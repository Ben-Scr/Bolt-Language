#include <chrono>
#include <cstdint>
#include <iostream>

using ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;

int main() {
    using clock = std::chrono::steady_clock;
    auto start = std::chrono::duration_cast<ticks>(clock::now().time_since_epoch());
    ticks previous{};
    for (int i = 0; i < 6000000; ++i) {
        auto current = std::chrono::duration_cast<ticks>(clock::now().time_since_epoch());
        auto elapsed = current - start;
        if (elapsed < previous) return 1;
        previous = elapsed;
    }
    if (previous == ticks::zero()) return 2;
    std::cout << 6000000 << '\n';
}
