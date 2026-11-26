#pragma once
#include "FileDigest.hpp"
#include "Process.hpp"
#include "../tooling/Json.hpp"
#include <bolt/Path.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bolt::cpp_inputs {
namespace fs = std::filesystem;
enum class Driver { gnu, msvc, clang_cl };
enum class Mode { expanded, directives, preamble };

// Preprocessed files can be large. Let the driver write them directly and hash
// them incrementally. A private directory also keeps two target builds apart.
class Workspace {
    fs::path directory_;
public:
    explicit Workspace(const fs::path& parent) {
        fs::create_directories(parent);
        static std::atomic<unsigned> sequence{0};
        for(unsigned attempt=0;attempt<32;++attempt) {
            const auto tick=std::chrono::steady_clock::now().time_since_epoch().count();
            auto candidate=parent/("probe-"+std::to_string(static_cast<long long>(tick))+"-"+std::to_string(sequence++));
            if(fs::create_directory(candidate)){directory_=std::move(candidate);return;}
        }
        throw std::runtime_error("Cannot create C++ dependency probe directory");
    }
    ~Workspace(){clear();}
    void clear() noexcept {
        if(directory_.empty())return;
        std::error_code ec;
        fs::remove_all(directory_,ec);
        if(!ec)directory_.clear();
    }
    Workspace(const Workspace&)=delete;
    Workspace& operator=(const Workspace&)=delete;
    fs::path file(const char* name)const{return directory_/name;}
};

// The driver receives a fixed -MT target and produces one Make rule. Preserve
// literal backslashes in Windows paths. GCC quotes spaces and # with a slash,
// doubles $, and folds the rule across lines with backslash-newline.
inline std::optional<std::vector<fs::path>> make_inputs(std::string_view text) {
    constexpr std::string_view target="bolt_inputs:";
    if(!text.starts_with(target)||text.find('\0')!=std::string_view::npos)return std::nullopt;
    text.remove_prefix(target.size());
    std::vector<fs::path> paths;
    std::string word;
    const auto flush=[&]{if(!word.empty()){paths.push_back(path_from_utf8(word));word.clear();}};
    for(std::size_t at=0;at<text.size();) {
        const char c=text[at++];
        if(c=='\\') {
            const auto first=at-1;
            while(at<text.size()&&text[at]=='\\')++at;
            const auto count=at-first;
            if(at<text.size()&&(text[at]==' '||text[at]=='\t')) {
                // The driver doubles each original slash before escaped space.
                word.append(count/2,'\\');
                if(count%2)word+=text[at];else flush();
                ++at;continue;
            }
            if(at<text.size()&&text[at]=='#'){word.append(count-1,'\\');word+='#';++at;continue;}
            if(at<text.size()&&(text[at]=='\n'||(text[at]=='\r'&&at+1<text.size()&&text[at+1]=='\n'))) {
                word.append(count-1,'\\');flush();
                at+=text[at]=='\r'?2:1;continue;
            }
            word.append(count,'\\');continue;
        }
        if(c=='$'&&at<text.size()&&text[at]=='$'){word+='$';++at;continue;}
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){flush();continue;}
        word+=c;
    }
    flush();
    return paths.empty()?std::nullopt:std::optional{std::move(paths)};
}

inline std::optional<std::vector<fs::path>> msvc_inputs(std::string_view text) {
    if(text.starts_with("\xef\xbb\xbf"))text.remove_prefix(3);
    const auto document=tooling::Json::parse(text);
    const auto version=document["Version"].string();
    if(version!="1.0"&&version!="1.1"&&version!="1.2")return std::nullopt;
    const auto& data=document["Data"];
    if(!data["Source"].is_string()||!data["Includes"].is_array())return std::nullopt;
    // Module and header-unit inputs use separate compiled artifacts.
    for(const auto* key:{"ImportedModules","ImportedHeaderUnits"})
        if(data.contains(key)&&(!data[key].is_array()||!data[key].array().empty()))return std::nullopt;
    if(!data["ProvidedModule"].string().empty())return std::nullopt;
    std::vector<fs::path> paths{path_from_utf8(data["Source"].string())};
    if(data.contains("PCH")) {
        if(!data["PCH"].is_string())return std::nullopt;
        if(!data["PCH"].string().empty())paths.push_back(path_from_utf8(data["PCH"].string()));
    }
    for(const auto& item:data["Includes"].array()) {
        if(!item.is_string())return std::nullopt;
        paths.push_back(path_from_utf8(item.string()));
    }
    return paths;
}

// A list of previously opened headers cannot detect a newly shadowing header
// or a conditional __has_include that never actually includes the probed file.
// Ask the actual target driver to resolve both on every cache lookup.
inline std::optional<std::string> signature(const std::string& compiler,const std::vector<std::string>& flags,
                                           const fs::path& source,const fs::path& expanded,Driver driver,bool hash_sources,Mode mode=Mode::expanded) {
    auto dependencies=expanded;
    dependencies+=driver==Driver::msvc?".json":".d";
    // A driver rejecting an initial probe may have produced a partial file.
    // A successful retry must create its own fresh output and dependency list.
    fs::remove(expanded);
    fs::remove(dependencies);
    std::vector<std::string> args{compiler};
    args.insert(args.end(),flags.begin(),flags.end());
    if(driver!=Driver::gnu) {
        args.insert(args.end(),{"/P","/Fi"+path_to_utf8(expanded)});
        if(driver==Driver::msvc)args.insert(args.end(),{"/sourceDependencies",path_to_utf8(dependencies)});
        if(hash_sources&&driver==Driver::clang_cl)args.insert(args.end(),{"/clang:-MD","/clang:-MF","/clang:"+path_to_utf8(dependencies),"/clang:-MT","/clang:bolt_inputs"});
    } else {
        args.insert(args.end(),{"-E","-o",path_to_utf8(expanded)});
        if(mode==Mode::directives)args.push_back("-fdirectives-only");
        // A PCH exports macros even if its own preamble never expands them.
        if(mode==Mode::preamble)args.push_back("-dD");
        if(hash_sources)args.insert(args.end(),{"-MD","-MF",path_to_utf8(dependencies),"-MT","bolt_inputs"});
    }
    args.push_back(path_to_utf8(source));
    const auto result=cli_process::run(args,true);
    if(result.exit_code!=0||!fs::is_regular_file(expanded)||fs::file_size(expanded)==0)return std::nullopt;
    if(mode==Mode::directives) {
        // Some clang drivers are installed as g++. Their directives-only output
        // omits command-line and builtin macro definitions. Use full preprocessing
        // unless this stream actually contains the GNU macro preamble.
        std::array<char,8192> prefix{};
        std::ifstream in(expanded,std::ios::binary);
        in.read(prefix.data(),static_cast<std::streamsize>(prefix.size()));
        const std::string_view predefines(prefix.data(),static_cast<std::size_t>(in.gcount()));
        if(predefines.find("#define __GNUC__ ")==std::string_view::npos||
           predefines.find("#define __cplusplus ")==std::string_view::npos)return std::nullopt;
    }
    auto digest=cli_state::file_hash(expanded);
    if(!hash_sources&&driver!=Driver::msvc)return digest;
    // DWARF/CodeView may record source checksums even if an edit (for example
    // a comment) leaves all preprocessed tokens and their locations unchanged.
    try {
        std::ifstream input(dependencies,std::ios::binary);
        if(!input)return std::nullopt;
        const std::string manifest{std::istreambuf_iterator<char>(input),{}};
        if(manifest.size()!=fs::file_size(dependencies))return std::nullopt;
        auto paths=driver==Driver::msvc?msvc_inputs(manifest):make_inputs(manifest);
        if(!paths)return std::nullopt;
        for(const auto& path:*paths){digest+=cli_state::bytes_digest(path_to_utf8(path));digest+=cli_state::file_hash(path);}
        return cli_state::bytes_digest(digest);
    } catch(const std::runtime_error&) {
        // An unsupported dependency format or a disappearing input only makes
        // this build ineligible for cache reuse. The normal compiler diagnoses it.
        return std::nullopt;
    }
}

// With GCC, -E -fdirectives-only resolves includes and all conditional groups
// and retains the complete active token stream and macro definitions. GCC's
// second preprocessing phase normally expands time-dependent builtin macros.
// Reject those streams for reuse, including macros assembled by token pasting.
// The check is needed only once when publishing a candidate artifact.
inline bool deterministic_directives(const std::string& compiler,const std::vector<std::string>& flags,
                                     const fs::path& expanded,const Workspace& workspace) {
    const auto checked=workspace.file("eligibility.ii");
    std::ifstream input(expanded,std::ios::binary);
    std::ofstream output(checked,std::ios::binary);
    if(!input||!output)return false;
    // __BASE_FILE__ names the compiler input itself, so it also precludes
    // compiling a saved directives file under a temporary name.
    constexpr std::string_view guard="#pragma GCC poison __DATE__ __TIME__ __TIMESTAMP__ __BASE_FILE__ __COUNTER__ __INCLUDE_LEVEL__\n";
    output<<guard<<input.rdbuf();
    output.close();
    if(!output||fs::file_size(checked)!=guard.size()+fs::file_size(expanded))return false;
    std::vector<std::string> args{compiler};
    args.insert(args.end(),flags.begin(),flags.end());
    args.insert(args.end(),{"-E","-x","c++","-fpreprocessed","-fdirectives-only",path_to_utf8(checked),
                            "-o",path_to_utf8(workspace.file("eligibility.out"))});
    return cli_process::run(args,true).exit_code==0;
}
} // namespace bolt::cpp_inputs
