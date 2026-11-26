#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main() {
    using namespace std::string_view_literals;
    constexpr auto first = "Grüße\0😀"sv, second = "second-é"sv;
    std::ofstream writer("lines.txt", std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 4096; ++i) {
        writer.write(first.data(), first.size()).put('\n');
        writer.write(second.data(), second.size()).put('\n');
        writer.put('\n');
    }
    writer.flush();
    writer.close();
    if (!writer) return 1;

    std::ifstream reader("lines.txt", std::ios::binary);
    unsigned sum = 0;
    for (int i = 0; i < 4096; ++i) {
        std::string a, b, empty;
        if (!std::getline(reader, a) || !std::getline(reader, b) || !std::getline(reader, empty)) return 2;
        // Exact comparison also rejects any malformed UTF-8 in these records.
        if (a != first || b != second || !empty.empty()) return 3;
        sum += static_cast<unsigned>(a.size() + b.size());
    }
    if (reader.peek() != std::char_traits<char>::eof() || reader.bad()) return 4;
    std::cout << sum << '\n';
}
