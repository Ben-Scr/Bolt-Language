#include <array>
#include <cstdint>
#include <iostream>
#include <sstream>

int main() {
    std::array<char, 4096> block;
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>(i & 127);
    std::int64_t sum = 0;
    for (int round = 0; round < 512; ++round) {
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        for (int i = 0; i < 256; ++i) {
            block[0] = static_cast<char>((round + i) & 127);
            stream.write(block.data(), block.size());
        }
        stream.seekg(0);
        for (int i = 0; i < 256; ++i) {
            if (!stream.read(block.data(), block.size())) return 1;
            sum += block.front() + block.back();
        }
    }
    std::cout << sum << '\n';
}
