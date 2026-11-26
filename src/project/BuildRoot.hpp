#pragma once
#include <bolt/Path.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace bolt::build_paths {
inline std::string hash_text(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

// Each source directory gets its own bin, obj, locks and successful-build receipts.
// The canonical identity is shared by recursive CLI invocations for dependencies.
inline std::filesystem::path root_for(const std::filesystem::path& source_root) {
    namespace fs = std::filesystem;
#ifdef _WIN32
    const auto* configured = _wgetenv(L"BOLT_BUILD_ROOT");
#else
    const auto* configured = std::getenv("BOLT_BUILD_ROOT");
#endif
    if (!configured || !*configured) return source_root;
    const auto base = fs::weakly_canonical(fs::absolute(fs::path(configured)));
    const auto source = fs::weakly_canonical(fs::absolute(source_root));
    const auto root = base / ("bolt-" + hash_text(path_to_utf8(source)));
    if (fs::is_symlink(root)) throw std::runtime_error("build root must not be a symlink: " + path_to_utf8(root));
    return root;
}
} // namespace bolt::build_paths
