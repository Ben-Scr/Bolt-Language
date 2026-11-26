#pragma once
#include "Diagnostics.hpp"
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
namespace bolt::project {
namespace fs=std::filesystem;
using Overrides=std::unordered_map<std::string,std::string>;
struct Profile {std::string optimization="none";bool debug_info=true;};
struct Model {
    fs::path file,root,working_directory;
    std::string name,version,kind="executable",entry,default_profile="Debug",target="host",identity;
    bool prelude=true,incremental=true,parallel=true;
    std::map<std::string,Profile> profiles{{"Debug",{"none",true}},{"Release",{"aggressive",false}}};
    std::vector<fs::path> roots,sources,config_files;
    std::vector<fs::path> native_include_dirs,native_library_dirs,native_runtime_dirs;
    std::vector<std::string> native_libraries;
    std::map<std::string,fs::path> dependencies;
    std::vector<std::string> arguments;
    std::vector<std::string> defines;
};
struct Graph {Model main;std::vector<Model> dependencies;std::vector<fs::path> sources;std::string identity;};
std::optional<fs::path> locate(const fs::path& input);
Model read(const fs::path& file,const Overrides& overrides={});
std::optional<Graph> resolve(const fs::path& input,const Overrides& overrides={});
bool contains_source(const Model&,const fs::path&);
std::string describe(const Graph&,const std::string& host,const std::string& profile="");
fs::path output_directory(const Model&,const std::string& host,const std::string& profile);
fs::path intermediate_directory(const Model&,const std::string& host,const std::string& profile);
void init(const fs::path& destination);
} // namespace bolt::project
