#pragma once
#include <bolt/Tooling.hpp>

namespace bolt {
// Source discovery is shared by validation, loading and error recovery for one
// inspection. A new inspection always observes current files and open overlays.
struct ProjectInputs {
    std::optional<project::Graph> graph;
    std::vector<std::filesystem::path> files;
};
ProjectInputs project_inputs(const std::filesystem::path& root, const SourceOverrides& overrides);
Program load_project(const ProjectInputs&, const std::filesystem::path& root,
        const SourceOverrides&, bool require_entry, bool check_semantics, const PreprocessorOptions&);
void read_inspection_sources(Inspection&, const ProjectInputs&, const std::filesystem::path& root,
        const SourceOverrides&, const PreprocessorOptions&);
Inspection inspect_project(const ProjectInputs&, const std::filesystem::path& root,
        const SourceOverrides&, bool require_entry, const PreprocessorOptions&);
}
