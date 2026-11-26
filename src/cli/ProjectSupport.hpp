#pragma once
#include <bolt/Project.hpp>
#include "../project/BuildRoot.hpp"
#include <bolt/Path.hpp>
#include <bolt/native/IR.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace bolt::project {
// Held only while building. OS locks release on process death, unlike stale
// directory locks. Never follow a user-provided symlink to an unrelated file.
class BuildLock {
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
#else
    int fd=-1;
#endif
public:
    BuildLock(const Model& m,const std::string& profile){
        const auto root=build_paths::root_for(m.root);
        auto dir=root/".bolt"/"locks";
        for(auto p=dir;p!=root;p=p.parent_path())if(fs::is_symlink(p))throw Error({path_to_utf8(p),1,1},"build lock directory must not be a symlink","BLT4102");
        fs::create_directories(dir);auto file=dir/(m.parallel?profile+".lock":"project.lock");
        if(fs::is_symlink(file))throw Error({path_to_utf8(file),1,1},"build lock must not be a symlink","BLT4102");
#ifdef _WIN32
        handle=CreateFileW(file.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot open project build lock");
        OVERLAPPED operation{};if(!LockFileEx(handle,LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&operation)){CloseHandle(handle);handle=INVALID_HANDLE_VALUE;throw std::runtime_error("cannot acquire project build lock");}
#else
        fd=::open(file.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);if(fd<0)throw std::runtime_error("cannot open project build lock");
        while(flock(fd,LOCK_EX)<0){if(errno==EINTR)continue;close(fd);fd=-1;throw std::runtime_error("cannot acquire project build lock");}
#endif
    }
    ~BuildLock(){
#ifdef _WIN32
        if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
#else
        if(fd>=0)close(fd);
#endif
    }
    BuildLock(const BuildLock&)=delete;BuildLock& operator=(const BuildLock&)=delete;
};
struct WorkingDirectory {
    fs::path previous;
    explicit WorkingDirectory(const fs::path& value){if(!value.empty()){previous=fs::current_path();fs::current_path(value);}}
    ~WorkingDirectory(){if(!previous.empty()){std::error_code ec;fs::current_path(previous,ec);}}
};
inline fs::path artifact(const Model& m,const std::string& host,const std::string& profile){
    std::string name=m.name;
#ifdef _WIN32
    name+=m.kind=="static-library"?".lib":m.kind=="shared-library"?".dll":".exe";
#elif defined(__APPLE__)
    if(m.kind=="static-library")name="lib"+name+".a";else if(m.kind=="shared-library")name="lib"+name+".dylib";
#else
    if(m.kind=="static-library")name="lib"+name+".a";else if(m.kind=="shared-library")name="lib"+name+".so";
#endif
    return output_directory(m,host,profile)/path_from_utf8(name);
}
} // namespace bolt::project

#include "BuildState.hpp"
namespace bolt::project {
inline bool generated_path(const Model& m,const fs::path& file){
    auto p=fs::absolute(file).lexically_normal();
    const auto root=build_paths::root_for(m.root);
    for(const auto& base:{root/"bin",root/"obj",root/".bolt"}){
        auto rel=p.lexically_relative(base);if(rel.empty()||rel.is_absolute()||*rel.begin()=="..")continue;
        for(auto d=p;d!=root;d=d.parent_path())if(fs::is_symlink(d))return false;
        return p.extension()!=".bolt"&&p.filename()!="bolt.proj"&&p.filename()!="bolt.conf";
    }return false;
}
inline fs::path inventory_file(const Model& m,const std::string& profile){return build_paths::root_for(m.root)/".bolt"/"artifacts"/(profile+".json");}
struct LibraryDeployment {
    fs::path source, destination;
    bool needs_copy;
};
using LibraryDeployments = std::vector<LibraryDeployment>;

inline LibraryDeployments plan_libraries(const Graph& graph,const std::string& profile,const std::string& host,const fs::path& target,bool adjacent,const fs::path& image_dir={}){
    LibraryDeployments deployed;
#if defined(__linux__)
    if(!adjacent&&host==native_backend::host_target())return deployed;
#elif !defined(_WIN32)
    if(!adjacent)return deployed;
#else
    (void)adjacent;
#endif

    using J=tooling::Json;J previous;
    auto marker=inventory_file(graph.main,profile);
    if(fs::is_regular_file(marker))try{std::ifstream in(marker);previous=J::parse(std::string((std::istreambuf_iterator<char>(in)),{}));}catch(const std::exception&){}
    for(const auto& dep:graph.dependencies)if(dep.kind=="shared-library"){
        auto source=artifact(dep,host,profile);
        if(!image_dir.empty())source=image_dir/source.filename();
        const auto dest=target.parent_path()/source.filename();
        if(fs::is_symlink(dest))throw std::runtime_error("refusing to overwrite a symlink with a dependency library");
        const auto digest=fs::is_regular_file(dest)?cli_state::file_hash(dest):std::string{};
        const bool needs_copy=digest!=cli_state::file_hash(source);
        if(needs_copy && fs::exists(dest)){
            const auto key=path_to_utf8(dest.lexically_relative(build_paths::root_for(graph.main.root)));
            const bool owned=previous["product"].string()=="bolt-project-artifacts-v1" && previous["root"].string()==path_to_utf8(graph.main.root) &&
                !digest.empty() && (previous["files"][key].string()==digest || previous["deployed"][path_to_utf8(dest)].string()==digest);
            if(!owned)throw std::runtime_error("unowned or modified dependency library at "+path_to_utf8(dest));
        }
        deployed.push_back({source,dest,needs_copy});
    }

    return deployed;
}
inline void record_artifacts(const Model& m,const std::string& profile,const fs::path& target,const fs::path& scratch,std::vector<fs::path> candidates={}){
    using J=tooling::Json;using O=J::Object;
    auto marker=inventory_file(m,profile);O files,deployed;
    if(fs::is_regular_file(marker)&&!fs::is_symlink(marker))try{std::ifstream in(marker);auto j=J::parse(std::string((std::istreambuf_iterator<char>(in)),{}));if(j["product"].string()=="bolt-project-artifacts-v1"&&j["root"].string()==path_to_utf8(m.root)){files=j["files"].object();deployed=j["deployed"].object();}}catch(const std::exception&){}
    // Custom -o destinations need ownership checks on subsequent builds, but clean
    // only removes files inside the project's generated directories.
    for(const auto& file:candidates)if(!generated_path(m,file))deployed[path_to_utf8(file)]=cli_state::file_hash(file);
    for(const auto& suffix:{"",".bolt.o",".bolt.obj",".bolt.native",".bolt.cpp",".obj",".pdb",".compiler.pdb",".lib",".exp"})candidates.push_back(path_from_utf8(path_to_utf8(target)+suffix));
    auto import=target;import.replace_extension(".lib");candidates.push_back(import);auto exp=target;exp.replace_extension(".exp");candidates.push_back(exp);
    for(const auto& file:{"BoltRuntime.dll","libBoltRuntime.so","libBoltRuntime.dylib"})candidates.push_back(target.parent_path()/file);
    if(!scratch.empty())for(const auto& suffix:{".bolt.cpp",".bolt.o",".bolt.obj",".bolt.obj.def",".obj",".pdb"})candidates.push_back(scratch/path_from_utf8(path_to_utf8(target.filename())+suffix));
    #ifdef _WIN32
    if(auto graph=resolve(m.file))for(const auto& dep:graph->dependencies)if(dep.kind=="shared-library")candidates.push_back(target.parent_path()/path_from_utf8(dep.name+".dll"));
#endif
    for(const auto& file:candidates)if(generated_path(m,file)&&fs::is_regular_file(file))files[path_to_utf8(file.lexically_relative(build_paths::root_for(m.root)))]=cli_state::file_hash(file);
    fs::create_directories(marker.parent_path());if(!generated_path(m,marker))throw std::runtime_error("unsafe generated artifact inventory path");
    auto temp=marker;temp+=".tmp";if(fs::is_symlink(temp))throw std::runtime_error("unsafe artifact inventory staging path");
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<J(O{{"product","bolt-project-artifacts-v1"},{"root",path_to_utf8(m.root)},{"files",files},{"deployed",deployed}}).dump();if(!out)throw std::runtime_error("cannot write artifact inventory");}
    cli_state::replace_file(temp,marker);
}
inline std::vector<fs::path> publish_project_artifacts(const Graph& graph,const std::string& profile,const std::string& host,const fs::path& target,const fs::path& scratch,bool adjacent=false,const LibraryDeployments* planned=nullptr){
    const auto libraries=planned?*planned:plan_libraries(graph,profile,host,target,adjacent);
    std::vector<fs::path> deployed;
    for(const auto& library:libraries){
        if(library.needs_copy){
            // A running process may still map the previous library.
            auto staged=library.destination;
            staged+=".deploying-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            fs::copy_file(library.source,staged);
            try {cli_state::replace_file(staged,library.destination);}
            catch(...){std::error_code ec;fs::remove(staged,ec);throw;}
        }
        deployed.push_back(library.destination);
    }
    record_artifacts(graph.main,profile,target,scratch,deployed);
    return deployed;
}
inline int clean_owned(const Model& m){
    using J=tooling::Json;std::size_t removed=0,kept=0;
    // Always acquire both locks in the same order. Do not acquire a parallel=false
    // project-wide lock twice within one process.
    BuildLock debug(m,"Debug");std::unique_ptr<BuildLock> release;if(m.parallel)release=std::make_unique<BuildLock>(m,"Release");
    for(const auto& profile:{"Debug","Release"}){
        auto marker=inventory_file(m,profile);if(!fs::exists(marker))continue;
        if(!generated_path(m,marker)||fs::file_size(marker)>4*1024*1024)throw std::runtime_error("invalid artifact inventory");
        std::ifstream in(marker);auto j=J::parse(std::string((std::istreambuf_iterator<char>(in)),{}));in.close();
        if(j["product"].string()!="bolt-project-artifacts-v1"||j["root"].string()!=path_to_utf8(m.root))throw std::runtime_error("unowned artifact inventory");
        for(const auto& [relative,checksum]:j["files"].object()){
            auto p=(build_paths::root_for(m.root)/path_from_utf8(relative)).lexically_normal();if(!generated_path(m,p)){++kept;continue;}
            if(!fs::exists(p))continue;if(fs::is_regular_file(p)&&cli_state::file_hash(p)==checksum.string()){fs::remove(p);++removed;}else ++kept;
        }
        fs::remove(marker);
    }
    std::cout<<"Removed "<<removed<<" recorded Bolt artifacts; preserved "<<kept<<" changed or unowned files.\n";return 0;
}
}
