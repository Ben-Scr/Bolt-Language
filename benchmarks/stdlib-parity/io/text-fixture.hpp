#pragma once
#include <string>

inline std::string text_fixture() {
    using namespace std::string_literals;
    std::string text = "0123456789abcdef0123456789abcdef\xc3\x96\0\n"s;
    for (int i = 0; i < 12; ++i) text += text;
    return text;
}
