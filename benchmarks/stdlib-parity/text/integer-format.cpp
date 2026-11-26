#include <cstdint>
#include <iostream>
#include <string>

int main() {
    std::uint64_t state = 917, total = 0;
    for (int i = 0; i < 1500000; ++i) {
        state = state * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        const auto value = std::to_string(state);
        total += value.size() + static_cast<unsigned char>(value.front());
    }
    std::cout << total << '\n';
}
