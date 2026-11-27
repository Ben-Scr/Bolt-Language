#pragma once
#include "library/IO.hpp"
namespace bolt::platform {
struct DirectoryEntry {
    std::string path; bool directory=false,file=false,link=false;
    native::String Path() const {return path;}
    bool IsDirectory() const noexcept{return directory;}
    bool IsFile() const noexcept{return file;}
    bool IsLink() const noexcept{return link;}
};
struct DirectoryOps {
    static bool IsDirectory(const native::String& path){return library::Directory::Exists(path);}
    static bool IsLink(const native::String& name){
        const auto path=library::io_detail::path(name);std::error_code ec;
        const auto yes=std::filesystem::is_symlink(std::filesystem::symlink_status(path,ec));
        if(ec)library::io_detail::error(path,ec,"Inspect entry");
        return yes;
    }
    static void CreateOne(const native::String& name){
        const auto path=library::io_detail::path(name);std::error_code ec;
        std::filesystem::create_directory(path,ec);if(ec)library::io_detail::error(path,ec,"Create directory");
    }
    static void RemoveOne(const native::String& name){
        const auto path=library::io_detail::path(name);std::error_code ec;
        const auto removed=std::filesystem::remove(path,ec);
        if(ec)library::io_detail::error(path,ec,"Remove entry");
        if(!removed)throw library::FileNotFoundException("Entry no longer exists: "+name);
    }
    static void RemoveTree(const native::String& name){library::io_detail::remove_tree(library::io_detail::path(name));}
    static void Move(const native::String& a,const native::String& b){library::Directory::Move(a,b);}
    static native::String Current(){return library::Directory::GetCurrentDirectory();}
    static void Current(const native::String& path){library::Directory::SetCurrentDirectory(path);}
    static native::Array<DirectoryEntry> Entries(const native::String& name){
        const auto root=library::io_detail::path(name);std::error_code ec;
        if(!std::filesystem::is_directory(root,ec)){
            if(ec)library::io_detail::error(root,ec,"List directory");
            throw library::DirectoryNotFoundException("Directory not found: "+name);
        }
        std::vector<DirectoryEntry> entries;
        for(std::filesystem::directory_iterator it(root,ec),end;it!=end&&!ec;it.increment(ec)){
            DirectoryEntry entry;entry.path=path_to_utf8(it->path());
            const auto link=it->symlink_status(ec);if(ec)break;
            entry.link=std::filesystem::is_symlink(link);
            auto status=entry.link?it->status(ec):link;
            // Enumerating a dangling link still returns that entry; never recurse through it.
            if(ec==std::errc::no_such_file_or_directory){ec.clear();status=std::filesystem::file_status(std::filesystem::file_type::not_found);}
            if(ec)break;
            entry.directory=std::filesystem::is_directory(status);entry.file=std::filesystem::is_regular_file(status);
            entries.push_back(std::move(entry));
        }
        if(ec)library::io_detail::error(root,ec,"List directory");
        return native::Array<DirectoryEntry>::FromRange(entries.begin(),entries.end());
    }
};
}
