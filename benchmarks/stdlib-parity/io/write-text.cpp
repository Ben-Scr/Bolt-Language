#include "text-fixture.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    const auto text = text_fixture();
    for (int i = 0; i < 512; ++i) {
        std::ofstream writer("text.bin", std::ios::binary | std::ios::trunc);
        writer.write(text.data(), text.size());
        writer.close();
        if (!writer) return 1;
    }
    std::cout << std::filesystem::file_size("text.bin") << '\n';
}
