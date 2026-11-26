#pragma once
#include "Ast.hpp"
#include <filesystem>
#include <string_view>

namespace bolt {
    // Bind parser-retained source annotations using compiler scope rules without
    // executing body analysis (also used for recovered editor declarations).
    void bind_navigation(Program& program);
std::vector<Token> tokenize(std::string_view source, const std::string& filename = "<source>");
Program parse(std::string_view source, const std::string& filename = "<source>", const PreprocessorOptions& preprocessing = {});
// Resolves imports relative to each importing file. Cycles are diagnosed.
Program load(const std::filesystem::path& path, bool require_entry = true, const PreprocessorOptions& preprocessing = {});
// In-memory code permits built-in/project namespaces, but not quoted filesystem imports.
Program compile_source(std::string_view source, const std::string& filename = "<runtime>", bool require_entry = true, const PreprocessorOptions& preprocessing = {});
void append_memory_source(Program& program, std::string_view source, const std::string& filename);
void analyze(Program& program, bool require_entry = true);
// Check an executable generation without constructing the editor reference index.
void analyze_for_execution(Program& program, bool require_entry = true);
std::string interpreter_requirement(const Program& program);
std::string emit_cpp(const Program& program);
std::string emit_mobile_cpp(const Program& program);
    // Shared compiler-owned declarations for tolerant editor recovery.
    void add_standard_library(Program&);
} // namespace bolt
