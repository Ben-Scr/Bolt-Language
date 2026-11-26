#include <cmath>
#include <cstdint>
#include <iostream>

int main() {
    double sum = 0.0;
    for (int i = 1; i <= 32000000; ++i)
        sum += std::sqrt(static_cast<double>(i) + 0.25);
    std::cout << static_cast<std::int64_t>(sum) << '\n';
}
