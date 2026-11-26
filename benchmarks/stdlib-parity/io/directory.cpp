#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const fs::path root = "entries";
    if (!fs::create_directory(root)) return 1;
    for (int i = 0; i < 64; ++i) {
        std::ofstream file(root / ("f" + std::to_string(i) + ".dat"), std::ios::binary);
        file.close();
        if (!file) return 1;
    }
    unsigned sum = 0;
    for (int round = 0; round < 512; ++round) {
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.is_regular_file()) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        if (files.size() != 64 || files.back().filename() != "f9.dat") return 2;
        const auto& selected = files[round & 63];
        if (!fs::is_regular_file(selected) || selected.extension() != ".dat") return 3;
        sum += static_cast<unsigned>(selected.filename().native().size());
    }
    for (int i = 0; i < 64; ++i)
        fs::remove(root / ("f" + std::to_string(i) + ".dat"));
    fs::remove(root);
    std::cout << sum << '\n';
}
