#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

static bool write_text(std::string_view text, std::ios::openmode mode) {
    std::ofstream out("short.txt", std::ios::binary | mode);
    out.write(text.data(), text.size());
    out.close();
    return static_cast<bool>(out);
}

int main() {
    using namespace std::string_view_literals;
    for (int i = 0; i < 512; ++i) {
        if (!write_text((i & 1) == 0 ? "short\n"sv : "Grüße\0\n"sv, std::ios::trunc) ||
            !write_text("é\0🙂\n"sv, std::ios::app) ||
            !write_text("done\n"sv, std::ios::app)) return 1;
    }
    std::cout << std::filesystem::file_size("short.txt") << '\n';
}
