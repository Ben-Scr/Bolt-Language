#pragma once
#include <bolt/Path.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace bolt::linux_target {
inline std::string gnu_prefix(const std::string& target) {
    return target.substr(0,target.find('-'))+"-linux-gnu-";
}
// A symlink such as /lib64/ld-linux-x86-64.so.2 may point to an absolute
// pathname inside the target system. Resolving it with the host's realpath
// can silently pick up the host loader or libraries instead.
inline std::filesystem::path in_sysroot(const std::filesystem::path& root,const std::filesystem::path& candidate) {
    namespace fs=std::filesystem;
    if(root=="/")return candidate;
    auto remaining=candidate.lexically_relative(root);
    auto directory=root;
    unsigned links=0;
    while(!remaining.empty()) {
        const auto part=*remaining.begin();
        remaining=remaining.lexically_relative(part);
        if(remaining==".")remaining.clear();
        if(part==".")continue;
        if(part=="..") {if(directory!=root)directory=directory.parent_path();continue;}
        auto next=directory/part;
        std::error_code ec;
        if(fs::is_symlink(fs::symlink_status(next,ec))) {
            if(++links>40)throw std::runtime_error("too many symbolic links in target sysroot: "+bolt::path_to_utf8(candidate));
            auto destination=fs::read_symlink(next);
            if(destination.is_absolute())directory=root;
            auto rest=destination.relative_path();
            if(!remaining.empty())rest/=remaining;
            remaining=std::move(rest);
        }else directory=std::move(next);
    }
    return directory;
}
// Read the target's ELF header without executing a foreign binary. The
// compiler, runtime and linker may all be in different SDK/sysroot trees.
inline void require_elf(const std::filesystem::path& path,const std::string& target) {
    std::array<unsigned char,20> header{};
    std::ifstream file(path,std::ios::binary);
    file.read(reinterpret_cast<char*>(header.data()),header.size());
    const unsigned expected=target.starts_with("aarch64-")?183u:target.starts_with("riscv64-")?243u:62u;
    if(!file || header[0]!=0x7f || header[1]!='E' || header[2]!='L' || header[3]!='F' ||
       header[4]!=2 || header[5]!=1 || header[6]!=1 || (header[7]!=0 && header[7]!=3) ||
       (header[18]|(unsigned(header[19])<<8))!=expected)
        throw std::runtime_error("expected a 64-bit ELF file for "+target+" at "+bolt::path_to_utf8(path));
}
}
