#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
    double sum = 0.0;
    for (int i = 1; i <= 4000000; ++i) {
        double x = static_cast<double>(i) * (1.0 / 1048576.0);
        sum += std::log(1.0 + x) + std::exp(-x);
    }
    std::cout << static_cast<std::int64_t>(sum * 1000.0) << '\n';
}
