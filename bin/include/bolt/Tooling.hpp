#pragma once
#include "Frontend.hpp"
#include "Project.hpp"
#include <iosfwd>
#include <memory>
namespace bolt {
using SourceOverrides = std::unordered_map<std::string, std::string>;
// Keys are canonical absolute UTF-8 paths. No source files are modified.
std::vector<std::filesystem::path> discover_sources(const std::filesystem::path& root);
Program load_project(const std::filesystem::path& root, const SourceOverrides& overrides,
                     bool require_entry = true, bool check_semantics = true, const PreprocessorOptions& preprocessing = {});
PreprocessorOptions project_preprocessor_options(const std::filesystem::path&, const SourceOverrides& = {}, const PreprocessorOptions& = {});
PreprocessorOptions source_preprocessor_options(const std::filesystem::path&, const project::Graph*, const PreprocessorOptions&);
struct Inspection {
    std::unique_ptr<Program> program;
    std::vector<Diagnostic> diagnostics;
    SourceOverrides sources;
    PreprocessorOptions preprocessor;
    std::unordered_map<std::string, PreprocessorOptions> source_preprocessors;
};
std::vector<std::filesystem::path> read_inspection_sources(Inspection&, const std::filesystem::path& root,
        const SourceOverrides& overrides, const PreprocessorOptions& preprocessing);
// Reports at most one syntax error per discovered file, then the first semantic
// error. Does not invent follow-up errors after an invalid type/declaration.
Inspection inspect_project(const std::filesystem::path& root, const SourceOverrides& overrides = {},
                           bool require_entry = false, const PreprocessorOptions& preprocessing = {});
int run_language_server(std::istream& input, std::ostream& output, std::ostream& log);
} // namespace bolt
