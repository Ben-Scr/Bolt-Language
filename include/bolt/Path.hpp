#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

namespace bolt {
inline std::filesystem::path path_from_utf8(std::string_view value) {
#ifdef _WIN32
    // ASCII code units map directly to UTF-16; keep the full filesystem UTF-8
    // conversion for multibyte text and its platform error handling.
    if (std::all_of(value.begin(),value.end(),[](unsigned char c){return c < 128;}))
        return std::filesystem::path(std::wstring(value.begin(),value.end()));
#endif
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
inline std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
} // namespace bolt
