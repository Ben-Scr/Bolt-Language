#pragma once
// Local successful-build receipts for `run --no-compile`.
// These are consistency checks, not signatures or a sandbox. They deliberately
// do not read .bolt sources, probe a compiler/linker, or load LLVM when running.
#include "../tooling/Json.hpp"
#include "FileDigest.hpp"
#include "../project/BuildRoot.hpp"
#include <bolt/Version.hpp>
#include <bolt/Path.hpp>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bolt::cli_state {
namespace fs = std::filesystem;
using Json = bolt::tooling::Json;
inline constexpr const char* product = "bolt-successful-aot-build-v1";
inline fs::path canonical_path(const fs::path& path) {
    return fs::weakly_canonical(fs::absolute(path));
}
inline std::string path_text(const fs::path& path) {
    return bolt::path_to_utf8(canonical_path(path));
}
using bolt::build_paths::hash_text;
inline Json read_receipt(const fs::path& path) {
    // Bound malformed/corrupt metadata before parsing it; never follow a metadata symlink.
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path)>128*1024)
        throw std::runtime_error("Missing or invalid successful-build record: "+bolt::path_to_utf8(path));
    std::ifstream in(path,std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    auto record=Json::parse(text);
    if (record["product"].string()!=product) throw std::runtime_error("Unrecognized successful-build record: "+bolt::path_to_utf8(path));
    return record;
}
inline void replace_file(const fs::path& source,const fs::path& target) {
#ifdef _WIN32
    // Replace atomically where the filesystem supports it; do not delete the old
    // receipt before publishing its replacement (a failed build must preserve it).
    if (!MoveFileExW(source.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish build record (Windows error "+std::to_string(GetLastError())+"): "+bolt::path_to_utf8(target));
#else
    fs::rename(source,target);
#endif
}
inline void write_receipt(const fs::path& path,const Json& record) {
    const auto folder=path.parent_path();
    for (const auto& dir : {folder.parent_path(),folder}) {
        if (fs::is_symlink(dir)) throw std::runtime_error("Refusing a symlinked build-record directory: "+bolt::path_to_utf8(dir));
        fs::create_directories(dir);
    }
    if (fs::is_symlink(path)) throw std::runtime_error("Refusing a symlinked build record: "+bolt::path_to_utf8(path));
    if (fs::exists(path)) (void)read_receipt(path); // Do not overwrite unrelated user files.
    // Independent staging directories allow concurrent builds with different
    // outputs to publish their shared "last" index without colliding on .tmp.
    static std::atomic<std::uint64_t> sequence{0};
    fs::path staging;
    for(unsigned attempt=0;attempt<32;++attempt) {
        const auto tick=static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
        auto candidate=folder/(".stage-"+std::to_string(tick)+"-"+std::to_string(sequence.fetch_add(1)));
        if(fs::create_directory(candidate)) {staging=std::move(candidate);break;}
    }
    if(staging.empty())throw std::runtime_error("Cannot create a build-record staging directory");
    const auto temporary=staging/"record.json";
    try {
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        out<<record.dump()<<'\n';out.close();
        if (!out) throw std::runtime_error("Cannot write build record: "+bolt::path_to_utf8(temporary));
        replace_file(temporary,path);
        fs::remove(staging);
    } catch (...) { std::error_code ec;fs::remove(temporary,ec);fs::remove(staging,ec);throw; }
}

class BuildIdentity {
    fs::path input_, root_;
    std::string configuration_;
    fs::path directory()const{return root_/".bolt"/"run";}
    fs::path last_path()const{return directory()/("last-"+hash_text(path_text(input_))+"-"+configuration_+".json");}
    fs::path output_path(const fs::path& target)const{return directory()/("output-"+hash_text(path_text(target))+".json");}
    std::string rebuild_hint()const {
        return " Run 'bolt run <project-or-file>"+std::string(configuration_=="Release"?" --release":"")+"' or 'bolt compile' first (0.10+ records are required).";
    }
public:
    BuildIdentity(fs::path input,fs::path root,bool release)
        :input_(canonical_path(input)),root_(canonical_path(root)),configuration_(release?"Release":"Debug"){}
    void remember(const fs::path& target,const std::string& backend,const std::string& host,const fs::path& runtime={},
                  const std::string& target_digest={},const std::string& runtime_digest={},bool runtime_free=false,const std::vector<fs::path>& libraries={})const {
        Json::Array dependencies;
        if (!runtime.empty()) dependencies.emplace_back(Json::Object{{"path",path_text(runtime)},{"hash",runtime_digest.empty()?file_hash(runtime):runtime_digest}});
        for(const auto& library:libraries)dependencies.emplace_back(Json::Object{{"path",path_text(library)},{"hash",file_hash(library)}});
        Json record=Json::Object{{"product",product},{"compilerVersion",BOLT_VERSION_STRING},
            {"input",path_text(input_)},{"configuration",configuration_},{"backend",backend},{"host",host},{"runtimeFree",runtime_free},
            {"target",path_text(target)},{"hash",target_digest.empty()?file_hash(target):target_digest},{"dependencies",std::move(dependencies)}};
        write_receipt(output_path(target),record);
        write_receipt(last_path(),record);
    }
    fs::path previous(const fs::path& explicit_target,const std::string& selected_backend,
                      bool require_native,const std::string& host,bool require_runtime_free=false)const {
        const auto index=explicit_target.empty()?last_path():output_path(explicit_target);
        if (!fs::is_regular_file(index))
            throw std::runtime_error("No previous "+configuration_+" executable build is recorded for '"+path_text(input_)+"'."+rebuild_hint());
        const auto record=read_receipt(index);
        if (record["input"].string()!=path_text(input_) || record["configuration"].string()!=configuration_)
            throw std::runtime_error("The recorded executable belongs to a different project or build configuration."+rebuild_hint());
        if (record["host"].string()!=host)
            throw std::runtime_error("The previous executable was built for a different target platform."+rebuild_hint());
        const auto backend=record["backend"].string();
        if(require_runtime_free&&!record["runtimeFree"].boolean())
            throw std::runtime_error("--no-runtime: rebuild this executable with --no-runtime to verify its dependencies.");
        if (backend!="fast" && backend!="llvm" && backend!="cpp") throw std::runtime_error("Invalid backend in successful-build record."+rebuild_hint());
        if (require_native && backend=="cpp") throw std::runtime_error("--require-native rejects the last build: it used the C++ compatibility backend.");
        if (selected_backend!="auto" && selected_backend!=backend)
            throw std::runtime_error("The last build used --backend "+backend+", not "+selected_backend+". --no-compile cannot change its backend.");
        const auto target=bolt::path_from_utf8(record["target"].string());
        if (target.empty() || !target.is_absolute() || fs::is_symlink(target) || !fs::is_regular_file(target))
            throw std::runtime_error("The previous "+configuration_+" executable is missing or was replaced: "+bolt::path_to_utf8(target)+"."+rebuild_hint());
        if (!explicit_target.empty() && path_text(target)!=path_text(explicit_target))
            throw std::runtime_error("The successful-build record does not match -o OUTPUT."+rebuild_hint());
        // A Debug build at a custom -o must not be mistaken for an older Release
        // build at the same path (even when their byte contents happen to match).
        if (read_receipt(output_path(target)).dump()!=record.dump())
            throw std::runtime_error("The previous executable was replaced by another build/configuration."+rebuild_hint());
        if (!matches_recorded_digest(target,record["hash"].string()))
            throw std::runtime_error("The previous executable changed after its successful build."+rebuild_hint());
        for (const auto& dependency : record["dependencies"].array()) {
            const auto path=bolt::path_from_utf8(dependency["path"].string());
            if (!path.is_absolute() || !fs::is_regular_file(path) || !matches_recorded_digest(path,dependency["hash"].string()))
                throw std::runtime_error("A runtime file of the previous build is missing or changed: "+bolt::path_to_utf8(path)+"."+rebuild_hint());
        }
        return target;
    }
};
} // namespace bolt::cli_state
