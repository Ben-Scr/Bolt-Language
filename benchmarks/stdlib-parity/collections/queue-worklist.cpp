#include <cstdint>
#include <iostream>
#include <queue>

int main() {
    std::queue<std::uint32_t> pending;
    for (int i = 0; i < 4096; ++i) pending.push(i * 7);
    std::uint64_t sum = 0;
    for (int turn = 0; turn < 8000000; ++turn) {
        auto value = pending.front();
        pending.pop();
        pending.push(((value << 7) ^ (value >> 13) ^ (turn * 17u)) & 65535u);
        sum += value;
    }
    std::cout << sum + pending.front() + pending.size() << '\n';
}
