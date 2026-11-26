#pragma once
#include "Diagnostics.hpp"
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bolt {
// Empty target/profile fields select the host and Debug. These are source-target
// choices, never the configuration used to build the Bolt compiler itself.
struct PreprocessorOptions {
    std::string platform, architecture, configuration;
    std::map<std::string, bool> defines;
};
struct PreprocessedSource {
    std::string text;
    std::vector<Location> directives, inactive;
};
const std::vector<std::string_view>& builtin_define_names();
PreprocessorOptions normalized_preprocessor_options(PreprocessorOptions options = {});
std::map<std::string, bool> preprocessor_symbols(const PreprocessorOptions& options = {});
std::string preprocessor_identity(const PreprocessorOptions& options);
// Does NOT expand/rewrite identifiers. Removed bytes become spaces, preserving
// newlines and byte offsets for compiler diagnostics, debug info, and LSP ranges.
PreprocessedSource preprocess(std::string_view source, const std::string& filename,
                              const PreprocessorOptions& options = {});
} // namespace bolt
