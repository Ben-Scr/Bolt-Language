#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
    double sum = 0.0;
    for (int i = 1; i <= 8000000; ++i) {
        double x = static_cast<double>(i) * (1.0 / 65536.0);
        sum += std::sin(x) + std::cos(x);
    }
    std::cout << static_cast<std::int64_t>(sum * 1000.0) << '\n';
}
