#include <atomic>
#include <cstdint>
#include <iostream>

// Use an externally visible cell so LLVM retains the atomic memory accesses.
std::atomic<std::int64_t> counter{};

int main() {
    std::int64_t sum = 0;
    for (int i = 0; i < 8000000; ++i) {
        auto next = counter.fetch_add(1) + 1;
        auto loaded = counter.load();
        if (loaded != next) return 1;
        sum += loaded;
    }
    std::cout << sum << '\n';
}
