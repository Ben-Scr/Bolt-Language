#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>

int main() {
    std::array<char, 8192> block;
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>(i & 127);
    std::int64_t sum = 0;
    for (int round = 0; round < 32; ++round) {
        std::ofstream out("stream.bin", std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 128; ++i) {
            block[0] = static_cast<char>((round + i) & 127);
            out.write(block.data(), block.size());
        }
        out.close();
        if (!out) return 1;
        std::ifstream in("stream.bin", std::ios::binary);
        for (int i = 0; i < 128; ++i) {
            if (!in.read(block.data(), block.size())) return 2;
            sum += block.front() + block.back();
        }
    }
    std::cout << sum << '\n';
}
