#include <cstdint>
#include <iostream>
#include <random>

int main() {
    thread_local std::mt19937_64 random(42);
    std::uniform_int_distribution<int> uniform(0, 999);
    std::int64_t sum = 0;
    int lower_half = 0;
    for (int i = 0; i < 16000000; ++i) {
        int draw = uniform(random);
        if (draw < 0 || draw >= 1000) return 1;
        sum += draw;
        if (draw < 500) ++lower_half;
    }
    if (sum < 499LL * 16000000 || sum > 500LL * 16000000) return 2;
    if (lower_half < 7984000 || lower_half > 8016000) return 3;
    std::cout << 16000000 << '\n';
}
