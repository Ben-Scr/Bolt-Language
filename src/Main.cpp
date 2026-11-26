#include <bolt/Bolt.hpp>
#include <bolt/Version.hpp>
#include <bolt/CompilerHost.h>
#include <bolt/native/ABI.hpp>
#include <bolt/native/IR.hpp>
#include <bolt/native/Library.hpp>
#include <cstdio>
#include <bolt/Tooling.hpp>
#include <bolt/Path.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <iomanip>
#include <optional>
#include <sstream>
#include <charconv>
#include <cctype>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include "cli/BuildState.hpp"
#include "cli/ProjectSupport.hpp"
#include "cli/RuntimeFreeLibraries.hpp"
#include "cli/Process.hpp"
#include "cli/CppInputs.hpp"
#include "cli/LinuxTarget.hpp"
#ifdef __APPLE__
#include "cli/AppleTarget.hpp"
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifndef BOLT_INCLUDE_DIR
#define BOLT_INCLUDE_DIR "include"
#endif
namespace {
    namespace fs = std::filesystem;
    bolt::DiagnosticFormat diagnostic_format=bolt::DiagnosticFormat::Text;
    // Opt-in IDE status protocol. Keep application stdout/stderr and ordinary CLI
    // output untouched. A stream is not a severity classification.
    void application_phase() {
        const char* enabled=std::getenv("BOLT_IDE_CONSOLE");
        if(enabled&&std::string_view(enabled)=="1") {std::cout.flush();std::cerr<<"\x1e" "BOLT1 {\"phase\":\"run\"}\n";std::cerr.flush();}
    }
    void status_message(const std::string& severity,const std::string& text) {
        const auto* protocol=std::getenv("BOLT_IDE_CONSOLE");
        if(protocol&&std::string_view(protocol)=="1") {
            using J=bolt::tooling::Json;
            std::cerr<<"\x1e" "BOLT1 "<<J(J::Object{{"severity",severity},{"message",text}}).dump()<<'\n';
        } else std::cerr<<text<<'\n';
        std::cerr.flush();
    }

    fs::path resolve_project_native_library(const bolt::project::Model& project,const std::string& library) {
        auto raw=bolt::path_from_utf8(library);
        if(raw.is_absolute()||raw.has_parent_path()) {
            auto path=raw.is_absolute()?raw:project.root/raw;
            if(fs::is_regular_file(path))return fs::weakly_canonical(path);
            throw std::runtime_error("Native library not found: "+bolt::path_to_utf8(path));
        }
        std::vector<fs::path> names{raw};
        if(!raw.has_extension()) {
#ifdef _WIN32
            names.emplace_back(library+".lib");
#elif defined(__APPLE__)
            names.emplace_back("lib"+library+".dylib");
            names.emplace_back("lib"+library+".a");
            names.emplace_back(library+".dylib");
            names.emplace_back(library+".a");
#else
            names.emplace_back("lib"+library+".so");
            names.emplace_back("lib"+library+".a");
            names.emplace_back(library+".so");
            names.emplace_back(library+".a");
#endif
        }
        for(const auto& directory:project.native_library_dirs)for(const auto& name:names){auto candidate=directory/name;if(fs::is_regular_file(candidate))return fs::weakly_canonical(candidate);}
        std::ostringstream message;message<<"Native library '"<<library<<"' was not found";
        if(project.native_library_dirs.empty())message<<"; add a directory to [native].library_dirs";
        else {message<<" in [native].library_dirs:";for(const auto& directory:project.native_library_dirs)message<<" "<<bolt::path_to_utf8(directory);}
        throw std::runtime_error(message.str());
    }

    void usage(const std::string& topic = {}) {
        if(!topic.empty()) {
            if(topic=="compile"||topic=="build")std::cout<<"Usage: bolt compile [file-or-folder] [--debug|--release] [-o OUTPUT]\n"
                "  --backend auto|fast|llvm|cpp  Select code generator (auto is default).\n"
                "  --llvm PATH     Shared LLVM 19+ library (or BOLT_LLVM_LIBRARY).\n"
                "  --linker PATH   Native linker (or BOLT_LINKER).\n"
                "  --runtime-dir PATH  Directory with the matching BoltRuntime/startup files.\n"
                "  --sysroot PATH  Override Windows SDK libraries, Linux library root or macOS SDK.\n"
                "  --target TRIPLE  Linux AOT accepts x86_64-linux-gnu, aarch64-linux-gnu and riscv64-linux-gnu.\n"
                "  --object-only   Emit an object file without linking.\n"
                "  --cxx COMPILER  Select the compatibility C++20 backend/toolchain.\n"
                "  --include PATH  Override the Bolt header directory.\n"
                "  --library PATH  Override the matching BoltCore static archive.\n"
                "  --header-dir DIR  Add a C/C++ header directory (repeatable).\n"
                "  --link-lib FILE   Link a native library file (repeatable).\n"
                "  --lto           C++ backend only: request link-time optimization (release).\n"
                "  --rebuild       Ignore the unchanged-build cache.\n"
                "  --no-pch        Disable the GCC/Clang header cache.\n"
                "  --verbose       Show invoked compiler/linker commands.\n"
                "  --require-native Reject C++ fallback; use precompiled runtime + native backend.\n"
                "  --no-runtime     Build an executable or shared library without the Bolt runtime.\n"
                "  --timings       Report compilation time on stderr (default for compile/run).\n"
                "  --no-timings    Disable build/preparation timing.\n"
                "  --diagnostics text|msbuild  Choose error output format.\n"
                "  BOLT_BUILD_ROOT  Place outputs, intermediates and cache in an external directory per source root.\n"
                "Output: <project>/bin/Debug|Release/<project>[.exe].\n"
                "Example: bolt compile . --release\n";
            else if(topic=="run")std::cout<<"Usage: bolt run [file-or-folder] [--debug|--release] [-- application-args]\n"
                "  Default: compile a native Debug executable and launch it. Path defaults to .\n"
                "  --release       Optimize an AOT executable and launch it (auto: LLVM O3).\n"
                "  --no-compile    Run the last successful executable of the selected configuration.\n"
                "                  Skip source loading, compilation, LLVM/toolchain probing and linking.\n"
                "                  Ignore source changes (even errors); never auto-build a missing binary.\n"
                "                  With -o OUTPUT, use that recorded output; otherwise use the last one.\n"
                "  --jit           Optional native in-memory code generation; no output executable.\n"
                "  --runtime       Optional interpreter. No --debug/--release or compiler options.\n"
                "  --steps N       Runtime/JIT instruction budget; 0 disables it. Not an AOT flag.\n"
                "  --native        Compatibility alias for the default AOT run mode.\n"
                "  --no-timings    Suppress the default build/preparation timing.\n"
                "  --backend auto|fast|llvm|cpp  Select/require a backend; --jit excludes cpp.\n"
                "  --require-native Reject any C++ compatibility fallback (also checks saved builds).\n"
                "  --no-runtime     Require an executable built without the Bolt runtime.\n"
                "  AOT accepts compile options. --no-compile rejects rebuild/toolchain options.\n"
                "  Application arguments after -- are passed unchanged; the child exit code is returned.\n"
                "Examples: bolt run . | bolt run . --release | bolt run . --release --no-compile\n"
                "          bolt run . --jit | bolt run . --runtime | bolt run . -- one \"two words\"\n";
            else if(topic=="check")std::cout<<"Usage: bolt check <file-or-folder> [--no-entry] [--diagnostics text|msbuild|json]\n"
                "  Check source without producing a native executable.\n  --define NAME   Add a boolean conditional-compilation symbol (repeatable).\n  --debug/--release Select conditional source profile without building.\n"
                "  --no-entry      Allow a library without Program.Main.\n"
                "  JSON is written to stdout; human/MSBuild errors go to stderr.\n"
                "Example: bolt check . --diagnostics json\n";
            else if(topic=="init"||topic=="create")std::cout<<"Usage: bolt init <new-folder>\nCreate bolt.proj and src/Main.bolt without overwriting files.\n";
            else if(topic=="project")std::cout<<"Usage: bolt project info <file-or-folder> --format json [--profile Debug|Release] [--define NAME]\nResolve configuration and source/entry metadata; no build is performed.\n";
            else if(topic=="new")std::cout<<"Usage: bolt new <new-folder>\nCreate Program.bolt, bolt.project, and .gitignore. The destination must not exist.\n";
            else if(topic=="clean")std::cout<<"Usage: bolt clean <project-folder>\nRemove known generated artifacts and caches; keep sources and unrelated files.\n";
            else if(topic=="lsp")std::cout<<"Usage: bolt lsp\nRun the Language Server Protocol over binary stdin/stdout. For editor clients, not an interactive REPL.\n";
            else if(topic=="emit")std::cout<<"Usage: bolt emit <file-or-folder> -o Program.cpp [--target web|android-arm64|android-x64|android-armv7|android-x86|ios-arm64|ios-sim-arm64|ios-sim-x64]\nEmit C++20 source without invoking a native compiler.\n";
            else if(topic=="emit-ir")std::cout<<"Usage: bolt emit-ir <file-or-folder> -o Program.ll\nEmit the typed native IR in LLVM-readable form without invoking code generation.\n";
            else if(topic=="backends")std::cout<<"Usage: bolt backends\nShow host target and availability of fast/LLVM code generation.\n";
            else if(topic=="exec")std::cout<<"Usage: bolt exec \"Console.WriteLine(1 + 2);\" [--steps N]\nInterpret a statement sequence; --jit selects native JIT execution.\n";
            else throw std::runtime_error("unknown help topic '"+topic+"'; use bolt help");
            return;
        }
        std::cout<<"Bolt " BOLT_VERSION_STRING R"( -- native compiler, JIT, interpreter, and language server

Usage: bolt <command> [arguments] [options]

Commands:
  init <folder>   Create a TOML bolt.proj project with src/Main.bolt
  project info . --format json   Inspect the shared project model
  compile [path]  Build a native executable from a .bolt file or project folder
  build [path]    Alias for compile
  run [path]      Build a native Debug/Release executable, then run it
  check <path>     Check syntax and types without building an executable
  emit <path>     Generate C++20 (compatibility backend); requires -o OUTPUT.cpp
  emit-ir <path>  Inspect typed native IR; requires -o OUTPUT.ll
  backends        Report available native backends
  exec "code"     Interpret a sequence of Bolt statements
  new <folder>    Create a new Bolt project (never overwrite an existing folder)
  clean <folder>  Remove recognized Bolt build outputs/cache, not source files
  lsp             Start the shared editor language server over stdin/stdout
  help [command]  Show help, flags, and examples (-h and --help also work)
  version [--json]  Show SDK version, or machine-readable version, binary path, target and ABIs

Examples:
  bolt new MyGame
  bolt compile MyGame
  bolt compile MyGame --release
  bolt run MyGame --release
  bolt run MyGame --release --no-compile
  bolt run MyGame --jit
  bolt run MyGame --runtime
  bolt check MyGame --diagnostics json
  bolt help compile

Debug: Fast on supported hosts. Linux Fast AOT emits source lines; LLVM O0 also emits debug information.
Release: LLVM O3. Native builds need a linker + matching BoltRuntime, not a C++ frontend.
Files, streams, Random, time, conversions, strings and collections support direct codegen.
Async/await, reflection and JSON use the shared native backend in JIT and release builds.
Unsupported features (e.g. abstract classes, ECS and some FFI signatures) select C++20.
Auto AOT also falls back to C++ if the required LLVM library is unavailable.
Use --require-native to reject any C++ fallback. --backend cpp selects compatibility explicitly.
--define NAME adds a conditional-compilation symbol (repeatable); no text macros.
Built-ins: Windows/Linux/MacOS/FreeBSD/IOS/Android/Web, Debug/Release, X64/ARM64/WASM64 (one of each active).
Project [language] defines and file-local #define/#undef are supported.
compile/build/run report time by default; --no-timings suppresses it.
--no-compile runs the last successful Debug/Release AOT build, even with edited/invalid sources.
--runtime explicitly interprets supported programs and rejects Debug/Release options.
--jit explicitly compiles in memory; it never creates the AOT executable used by --no-compile.
Build configuration and execution mode are independent. No command silently selects an interpreter.
--lto applies only to the C++ compatibility backend.
Omitting a source path means the current directory. Main(string[] args) is supported.
Runtime.Execute remains interpreted; Runtime.ExecuteJit compiles native code in memory.
)";
    }
#ifdef _WIN32
    std::string utf8(const std::wstring& input) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), result.data(), n, nullptr, nullptr);
        return result;
    }
    std::wstring utf16(const std::string& input) {
        const int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
        if(!input.empty()&&!n)throw std::runtime_error("invalid UTF-8 text while configuring MSVC");
        std::wstring result(static_cast<std::size_t>(n),L'\0');
        if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),result.data(),n);
        return result;
    }
    fs::path find_command(const std::vector<std::string>& names);
    std::wstring windows_environment(const wchar_t* name) {
        const auto required=GetEnvironmentVariableW(name,nullptr,0);
        if(!required)return {};
        std::wstring value(required,L'\0');
        const auto written=GetEnvironmentVariableW(name,value.data(),required);
        if(!written||written>=required)return {};
        value.resize(written);
        return value;
    }
    fs::path command_processor() {
        if(auto value=windows_environment(L"ComSpec");!value.empty())return fs::path(value);
        std::wstring directory(32768,L'\0');
        const auto size=GetSystemDirectoryW(directory.data(),static_cast<UINT>(directory.size()));
        if(!size||size>=directory.size())throw std::runtime_error("could not locate cmd.exe while configuring MSVC");
        directory.resize(size);
        return fs::path(directory)/L"cmd.exe";
    }
    fs::path vswhere_path() {
        if(auto found=find_command({"vswhere.exe"});!found.empty())return found;
        for(const auto* variable:{L"ProgramFiles(x86)",L"ProgramFiles"}) {
            auto root=windows_environment(variable);
            if(root.empty())continue;
            auto candidate=fs::path(root)/L"Microsoft Visual Studio"/L"Installer"/L"vswhere.exe";
            if(fs::is_regular_file(candidate))return candidate;
        }
        return {};
    }
    std::string trim_process_line(std::string value) {
        while(!value.empty()&&(value.back()=='\r'||value.back()=='\n'||value.back()==' '||value.back()=='\t'))value.pop_back();
        std::size_t begin=0;while(begin<value.size()&&(value[begin]==' '||value[begin]=='\t'||value[begin]=='\r'||value[begin]=='\n'))++begin;
        return value.substr(begin);
    }
#if defined(_M_ARM64) || defined(__aarch64__)
    constexpr auto msvc_component = "Microsoft.VisualStudio.Component.VC.Tools.ARM64";
    constexpr auto msvc_setup = L"vcvarsarm64.bat";
    constexpr auto msvc_target = L"arm64";
    constexpr auto coff_machine = "/MACHINE:ARM64";
#else
    constexpr auto msvc_component = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64";
    constexpr auto msvc_setup = L"vcvars64.bat";
    constexpr auto msvc_target = L"x64";
    constexpr auto coff_machine = "/MACHINE:X64";
#endif
    fs::path discover_vcvars() {
        if(auto where=vswhere_path();!where.empty()) {
            auto result=bolt::cli_process::run({
                bolt::path_to_utf8(where),"-latest","-products","*",
                "-requires",msvc_component,
                "-property","installationPath","-utf8"
            },true);
            if(result.exit_code==0) {
                auto installation=trim_process_line(result.output);
                if(!installation.empty()) {
                    auto candidate=bolt::path_from_utf8(installation)/"VC"/"Auxiliary"/"Build"/msvc_setup;
                    if(fs::is_regular_file(candidate))return candidate;
                }
            }
        }
        // Fallback for installations where vswhere is unavailable but Visual
        // Studio uses its standard install root. Choose the newest matching tree.
        std::vector<fs::path> candidates;
        for(const auto* variable:{L"ProgramFiles",L"ProgramFiles(x86)"}) {
            auto root=windows_environment(variable);
            if(root.empty())continue;
            auto visual_studio=fs::path(root)/L"Microsoft Visual Studio";
            std::error_code ec;
            if(!fs::is_directory(visual_studio,ec))continue;
            for(const auto& version:fs::directory_iterator(visual_studio,ec)) {
                if(ec||!version.is_directory())continue;
                for(const auto& edition:fs::directory_iterator(version.path(),ec)) {
                    if(ec||!edition.is_directory())continue;
                    auto candidate=edition.path()/L"VC"/L"Auxiliary"/L"Build"/msvc_setup;
                    if(fs::is_regular_file(candidate))candidates.push_back(candidate);
                }
            }
        }
        if(candidates.empty())return {};
        std::sort(candidates.begin(),candidates.end());
        return candidates.back();
    }
    void apply_environment_dump(const std::string& dump) {
        std::size_t begin=0;
        while(begin<dump.size()) {
            auto end=dump.find('\n',begin);if(end==std::string::npos)end=dump.size();
            auto line=dump.substr(begin,end-begin);if(!line.empty()&&line.back()=='\r')line.pop_back();
            begin=end+1;
            // cmd.exe exposes drive-current-directory pseudo variables such as
            // '=C:=C:\path'. They cannot be set through SetEnvironmentVariable.
            if(line.empty()||line.front()=='=')continue;
            const auto separator=line.find('=');
            if(separator==std::string::npos||separator==0)continue;
            const auto name=utf16(line.substr(0,separator));
            const auto value=utf16(line.substr(separator+1));
            if(!SetEnvironmentVariableW(name.c_str(),value.c_str()))
                throw std::runtime_error("could not apply MSVC environment variable '"+line.substr(0,separator)+"'");
        }
    }
    bool msvc_environment_ready(const fs::path& compiler) {
        if(compiler.empty()||windows_environment(L"INCLUDE").empty()||windows_environment(L"LIB").empty())return false;
        auto lower=[](std::wstring value){for(auto& c:value)c=static_cast<wchar_t>(std::towlower(c));return value;};
        const auto configured=lower(windows_environment(L"VSCMD_ARG_TGT_ARCH"));
        const auto target=lower(compiler.parent_path().filename().wstring());
        // Hostx86\x64 is also a valid x64-targeting compiler.  The final
        // directory names the target architecture, independent of host tools.
        // If Visual Studio already supplied an explicit target architecture,
        // trust it instead of accepting a mismatched environment just because
        // the discovered compiler binary happens to live in an x64 folder.
        if(!configured.empty())return configured==msvc_target;
        return target==msvc_target;
    }
    fs::path ensure_msvc_toolchain() {
        if(auto existing=find_command({"cl.exe"});!existing.empty()&&msvc_environment_ready(existing))return existing;
        const auto vcvars=discover_vcvars();
        if(vcvars.empty())throw std::runtime_error(
            "MSVC C++ tools were not found. Install the Visual Studio C++ workload (build tools for the target CPU and a Windows SDK)."
        );
        const auto temp=fs::temp_directory_path()/("bolt-msvc-env-"+std::to_string(GetCurrentProcessId())+".cmd");
        {
            std::ofstream out(temp,std::ios::binary|std::ios::trunc);
            if(!out)throw std::runtime_error("could not create temporary MSVC environment script");
            out<<"@echo off\r\n";
            out<<"call \""<<bolt::path_to_utf8(vcvars)<<"\" >nul\r\n";
            out<<"if errorlevel 1 exit /b %errorlevel%\r\n";
            // `set` writes through cmd.exe's active code page. Force UTF-8 so
            // non-ASCII Visual Studio/SDK paths survive the process boundary.
            out<<"chcp 65001 >nul 2>nul\r\n";
            out<<"set\r\n";
        }
        bolt::cli_process::Result result;
        try {
            // /S changes cmd.exe's quote stripping rules for /C and can make a
            // quoted batch-file path fail with "The filename, directory name, or
            // volume label syntax is incorrect".  The temporary script already
            // contains all required quoting, so execute it with ordinary /C.
            result=bolt::cli_process::run({
                bolt::path_to_utf8(command_processor()),
                "/d","/q","/c",bolt::path_to_utf8(temp)
            },true);
        } catch(...) {
            std::error_code ignored;fs::remove(temp,ignored);throw;
        }
        std::error_code ignored;fs::remove(temp,ignored);
        if(result.exit_code!=0) {
            auto detail=trim_process_line(result.error);
            if(detail.empty())detail=trim_process_line(result.output);
            throw std::runtime_error("Visual Studio vcvars64.bat failed"+(detail.empty()?std::string{}:": "+detail));
        }
        apply_environment_dump(result.output);
        if(auto configured=find_command({"cl.exe"});!configured.empty()&&msvc_environment_ready(configured))return configured;
        throw std::runtime_error("Visual Studio environment was loaded, but a matching MSVC compiler with INCLUDE/LIB configured is still unavailable");
    }
#endif
    class ScopedLibrarySearchPath {
        bool active=false;
#ifdef _WIN32
        std::wstring previous;
#else
        std::string variable,previous;
        bool had_previous=false;
#endif
    public:
        explicit ScopedLibrarySearchPath(const std::vector<fs::path>& directories) {
            if(directories.empty())return;
#ifdef _WIN32
            previous=windows_environment(L"PATH");
            std::wstring combined;
            for(const auto& directory:directories){if(!combined.empty())combined+=L';';combined+=directory.wstring();}
            if(!previous.empty()){if(!combined.empty())combined+=L';';combined+=previous;}
            if(!SetEnvironmentVariableW(L"PATH",combined.c_str()))throw std::runtime_error("could not configure native runtime search path");
#else
#ifdef __APPLE__
            variable="DYLD_LIBRARY_PATH";
#else
            variable="LD_LIBRARY_PATH";
#endif
            if(const char* old=std::getenv(variable.c_str())){had_previous=true;previous=old;}
            std::string combined;
            for(const auto& directory:directories){if(!combined.empty())combined+=':';combined+=bolt::path_to_utf8(directory);}
            if(had_previous&&!previous.empty()){if(!combined.empty())combined+=':';combined+=previous;}
            if(::setenv(variable.c_str(),combined.c_str(),1)!=0)throw std::runtime_error("could not configure native runtime search path");
#endif
            active=true;
        }
        ~ScopedLibrarySearchPath(){
            if(!active)return;
#ifdef _WIN32
            SetEnvironmentVariableW(L"PATH",previous.empty()?nullptr:previous.c_str());
#else
            if(had_previous)::setenv(variable.c_str(),previous.c_str(),1);else ::unsetenv(variable.c_str());
#endif
        }
        ScopedLibrarySearchPath(const ScopedLibrarySearchPath&)=delete;
        ScopedLibrarySearchPath& operator=(const ScopedLibrarySearchPath&)=delete;
    };
    int process(const std::vector<std::string>& arguments,const std::string& tool={}) {
        std::cout.flush();std::cerr.flush();
        const auto result=bolt::cli_process::run(arguments,!tool.empty());
        if(!result.output.empty()){std::cout<<result.output;std::cout.flush();}
        if(!result.error.empty()){std::cerr<<result.error;std::cerr.flush();}
        if(!result.termination.empty())status_message("error","[bolt] "+(tool.empty()?"Application":tool)+" "+result.termination+".");
        else if(result.exit_code!=0&&!tool.empty())status_message("error","[bolt] "+tool+" failed with exit code "+std::to_string(result.exit_code)+".");
        return result.exit_code;
    }
    fs::path executable_path(const std::string& argv0, bool current_process = true) {
#ifdef __linux__
        if(current_process) {
            std::error_code error;
            auto self=fs::read_symlink("/proc/self/exe",error);
            if(!error&&!self.empty())return self;
        }
#endif
        (void)current_process;
#ifdef _WIN32
        std::wstring buffer(32768, L'\0');
        const auto n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (current_process && n != 0 && n < buffer.size()) {
            buffer.resize(n);
            return fs::path(buffer);
        }
#endif
        auto path = bolt::path_from_utf8(argv0);
        if (fs::exists(path)) return fs::canonical(path);
        const char* env = std::getenv("PATH");
        if (env) {
#ifdef _WIN32
            const char delimiter = ';';
#else
            const char delimiter = ':';
#endif
            std::string search(env);
            std::size_t begin = 0;
            while (begin <= search.size()) {
                auto end = search.find(delimiter, begin);
                if (end == std::string::npos) end = search.size();
                auto candidate = bolt::path_from_utf8(search.substr(begin, end - begin)) / path;
                if (fs::exists(candidate)) return fs::canonical(candidate);
                begin = end + 1;
            }
        }
        return fs::absolute(path);
    }
    void write_file(const fs::path& path, const std::string& data) {
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || !(out << data)) throw std::runtime_error("cannot write '" + bolt::path_to_utf8(path) + "'");
        out.close();
        if (!out) throw std::runtime_error("failed to finish writing '" + bolt::path_to_utf8(path) + "'");
    }
    void protect_sources(const bolt::Program& program, const fs::path& output) {
        auto extension = bolt::path_to_utf8(output.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (extension == ".bolt") throw std::runtime_error("refusing to overwrite a .bolt source with build output");
        const auto target = fs::weakly_canonical(fs::absolute(output));
        for (const auto& dependency : program.dependencies) {
            const auto source = fs::weakly_canonical(bolt::path_from_utf8(dependency));
            std::error_code ec;
            // Equivalent also detects hard links and case-insensitive aliases.
            if (target == source || fs::equivalent(target, source, ec)) throw std::runtime_error("output would overwrite an input source");
        }
    }
    std::uint64_t hash_bytes(std::uint64_t hash,std::string_view bytes) {
        for(unsigned char c:bytes) {
            hash^=c;
            hash*=1099511628211ull;
        }
        return hash;
    }
    std::string hex_hash(std::uint64_t hash) {
        std::ostringstream out;
        out<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;
        return out.str();
    }
    std::uint64_t hash_file(std::uint64_t hash,const fs::path& path) {
        return hash_bytes(hash,bolt::cli_state::file_hash(path));
    }
    // The load step has already resolved imports, file-specific preprocessor
    // options and their order. Use exactly those bytes, including line breaks,
    // when a native artifact contains source-level debug information.
    std::uint64_t debug_source_fingerprint(std::uint64_t key,const bolt::Program& program) {
        for(const auto& path:program.dependencies) {
            key=hash_bytes(key,bolt::cli_state::bytes_digest(path));
            key=hash_bytes(key,bolt::cli_state::bytes_digest(program.sources.at(path)));
            key=hash_bytes(key,bolt::cli_state::bytes_digest(bolt::preprocessor_identity(program.source_preprocessors.at(path))));
        }
        return key;
    }
    bool write_if_changed(const fs::path& path,const std::string& data) {
        if(fs::is_regular_file(path)&&bolt::cli_state::file_hash(path)==bolt::cli_state::bytes_digest(data))return false;
        write_file(path,data);
        return true;
    }
    void verbose_command(const std::vector<std::string>& args,bool verbose) {
        if(!verbose)return;
        std::ostringstream message;message<<"[bolt]";
        for(const auto& s:args)message<<' '<<std::quoted(s);
        status_message("info",message.str());
    }
    fs::path find_command(const std::vector<std::string>& names) {
        // Preserve the executable name: ld.lld/lld-link may be symlinks to the
        // same multicall binary, whose behavior depends on argv[0].
        for(const auto& name:names) {
            auto candidate=bolt::path_from_utf8(name);
            if(candidate.has_parent_path()) { if(fs::is_regular_file(candidate))return fs::absolute(candidate); continue; }
#ifdef _WIN32
            const auto path=windows_environment(L"PATH");
            const wchar_t separator=L';';
#else
            const char* value=std::getenv("PATH");if(!value)continue;
            const std::string path=value;
            const char separator=':';
#endif
            std::size_t start=0;
            while(start<=path.size()) {
                auto end=path.find(separator,start);if(end==path.npos)end=path.size();
                auto found=fs::path(path.substr(start,end-start))/candidate;
                if(fs::is_regular_file(found))return fs::absolute(found);
                start=end+1;
            }
        }
        return {};
    }
#ifdef _WIN32
    fs::path windows_kernel32_library(const std::string& sysroot, const fs::path& compiler_directory) {
#if defined(_M_ARM64) || defined(__aarch64__)
        const fs::path architecture="arm64";
#else
        const fs::path architecture="x64";
#endif
        auto environment=[](const wchar_t* name) {
            // SDK paths may contain characters outside the active ANSI code page.
            std::wstring value;
            for(;;) {
                const auto required=GetEnvironmentVariableW(name,nullptr,0);
                if(!required)return std::wstring{};
                value.resize(required);
                const auto written=GetEnvironmentVariableW(name,value.data(),required);
                if(written>=required)continue;
                value.resize(written);return value;
            }
        };
        auto existing=[](const fs::path& path) {
            std::error_code error;
            if(!fs::is_regular_file(path,error))return fs::path{};
            auto absolute=fs::absolute(path,error);
            return error?fs::path{}:absolute;
        };
        auto directory=[&](const fs::path& root) {
            for(const auto& candidate:{root/"kernel32.lib",root/architecture/"kernel32.lib",root/"um"/architecture/"kernel32.lib"})
                if(auto found=existing(candidate);!found.empty())return found;
            return fs::path{};
        };
        auto version_parts=[](const fs::path& path) {
            const auto text=bolt::path_to_utf8(path.filename());
            std::vector<unsigned> parts;std::size_t start=0;
            while(start<text.size()) {
                auto end=text.find('.',start);if(end==std::string::npos)end=text.size();
                unsigned number=0;const auto parsed=std::from_chars(text.data()+start,text.data()+end,number);
                if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+end)return std::vector<unsigned>{};
                parts.push_back(number);start=end+1;
            }
            return parts;
        };
        auto versions=[&](const fs::path& root) {
            std::vector<fs::path> candidates;std::error_code error;
            for(fs::directory_iterator it(root,error),end;!error&&it!=end;it.increment(error))
                if(!version_parts(it->path()).empty())candidates.push_back(it->path());
            std::sort(candidates.begin(),candidates.end(),[&](const auto& a,const auto& b){return version_parts(a)>version_parts(b);});
            for(const auto& candidate:candidates)if(auto found=directory(candidate);!found.empty())return found;
            return fs::path{};
        };
        auto sdk=[&](const fs::path& root) {
            if(auto found=directory(root);!found.empty())return found;
            if(auto found=directory(root/"Lib");!found.empty())return found;
            if(auto found=versions(root/"Lib");!found.empty())return found;
            return versions(root);
        };
        if(!sysroot.empty()) {
            const auto root=bolt::path_from_utf8(sysroot);
            if(root.filename()=="kernel32.lib")if(auto found=existing(root);!found.empty())return found;
            if(auto found=sdk(root);!found.empty())return found;
            throw std::runtime_error("Windows console linking requires kernel32.lib for "+bolt::path_to_utf8(architecture)+
                "; none was found under --sysroot '"+sysroot+"'. Pass the Windows SDK root or its Lib/<version>/um/"+
                bolt::path_to_utf8(architecture)+" directory.");
        }
        // A configured toolchain already identifies its preferred SDK. Avoid
        // enumerating installed versions when one of its LIB paths is usable.
        const auto libraries=environment(L"LIB");std::size_t start=0;
        while(start<libraries.size()) {
            auto end=libraries.find(L';',start);if(end==std::wstring::npos)end=libraries.size();
            auto entry=libraries.substr(start,end-start);
            if(entry.size()>=2&&entry.front()==L'"'&&entry.back()==L'"')entry=entry.substr(1,entry.size()-2);
            if(!entry.empty())if(auto found=existing(fs::path(entry)/"kernel32.lib");!found.empty())return found;
            start=end+1;
        }
        const auto configured=environment(L"WindowsSdkDir"),version=environment(L"WindowsSDKVersion");
        if(!configured.empty()) {
            const fs::path root(configured);
            if(!version.empty())if(auto found=directory(root/"Lib"/fs::path(version));!found.empty())return found;
            if(auto found=sdk(root);!found.empty())return found;
        }
        // Portable SDKs ship target-specific import libraries next to the compiler.
        if (auto found = directory(compiler_directory / "windows-libs"); !found.empty()) return found;
        for(const auto* name:{L"ProgramFiles(x86)",L"ProgramFiles"}) {
            const auto installed=environment(name);
            if(!installed.empty())if(auto found=sdk(fs::path(installed)/"Windows Kits"/"10");!found.empty())return found;
        }
        throw std::runtime_error("Windows console linking requires the Windows SDK kernel32.lib for "+
            bolt::path_to_utf8(architecture)+". Install the Windows SDK, set LIB to its library directory, or pass "
            "--sysroot <Windows SDK>/Lib/<version>/um/"+bolt::path_to_utf8(architecture)+".");
    }
#endif
#if defined(_M_X64)
    // Linking a static program only needs import libraries and the CRT archives.
    // Portable compiler installs without these libraries keep using the runtime DLL.
    std::vector<fs::path> windows_aot_libraries(const std::string& sysroot) {
        fs::path vc(windows_environment(L"VCToolsInstallDir"));
        auto crt=vc/"lib"/"x64";
        if(vc.empty() || !fs::is_regular_file(crt/"libcmt.lib")) {
            const auto setup=discover_vcvars();
            if(setup.empty())return {};
            std::ifstream version_file(setup.parent_path()/"Microsoft.VCToolsVersion.default.txt");
            std::string version;
            if(!(version_file>>version))return {};
            crt=setup.parent_path().parent_path().parent_path()/"Tools"/"MSVC"/version/"lib"/"x64";
        }
        for(auto name:{"libcmt.lib","libcpmt.lib","libvcruntime.lib","oldnames.lib","delayimp.lib"})
            if(!fs::is_regular_file(crt/name))return {};
        auto sdk=sysroot;
        if(sdk.empty()) {
            const auto configured=windows_environment(L"WindowsSdkDir");
            const auto installed=windows_environment(L"ProgramFiles(x86)");
            const auto root=configured.empty()?fs::path(installed)/"Windows Kits"/"10":fs::path(configured);
            const auto version=windows_environment(L"WindowsSDKVersion");
            const auto preferred=root/"Lib"/fs::path(version);
            sdk=bolt::path_to_utf8(!version.empty()&&fs::is_regular_file(preferred/"um"/"x64"/"kernel32.lib")?preferred:root);
        }
        fs::path um;
        try {um=windows_kernel32_library(sdk,{}).parent_path();}
        catch(const std::runtime_error&) {return {};}
        const auto ucrt=um.parent_path().parent_path()/"ucrt"/"x64";
        if(!fs::is_regular_file(ucrt/"libucrt.lib"))return {};
        for(auto name:{"shell32.lib","ws2_32.lib","advapi32.lib","bcrypt.lib"})
            if(!fs::is_regular_file(um/name))return {};
        return {crt,ucrt,um};
    }
#endif
    std::string native_backend_name(const std::string& selected,bool release,const std::string& target) {
        if(selected=="auto")return release||!bolt::native_backend::fast_target_supported(target)?"llvm":"fast";
        if(selected!="fast"&&selected!="llvm"&&selected!="cpp")throw std::runtime_error("--backend requires auto, fast, llvm, or cpp");
        if(selected=="fast"&&release)throw std::runtime_error("--backend fast is a development backend; use --release with auto or llvm");
        return selected;
    }
    // Wall-clock build/preparation time only. End before entering user code.
    // Timings/diagnostics go to stderr; legacy Built/Up-to-date status stays on stdout.
    struct BuildTiming {
        std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
        bool enabled=false,reported=false;
        explicit BuildTiming(bool value):enabled(value){}
        void finish(const std::string& phase="Compilation",bool cached=false) {
            if(reported)return;
            reported=true;
            if(!enabled)return;
            const double milliseconds=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            std::ostringstream message;message.imbue(std::locale::classic());
            message<<"[bolt] "<<phase<<": "<<std::fixed<<std::setprecision(2)<<milliseconds<<" ms";
            if(cached)message<<" (cache hit; no code generation/linking)";
            status_message(phase.starts_with("Build/preparation failed")?"error":"info",message.str());
        }
        ~BuildTiming(){try{if(!reported)finish("Build/preparation failed after");}catch(...){}}
    };
    struct NativeBuild {
        std::string backend="auto",llvm,linker,runtime_dir,sysroot,target_triple;
        fs::path work_dir,intermediate_dir;
        std::string project_kind;
        std::vector<std::string> link_libraries;
        bool optimize=false,debug_info=true,require_runtime_free=false;
        bool release=false,rebuild=false,verbose=false,run=false,object_only=false;
        BuildTiming* timing=nullptr;
        std::function<void(const std::string&,const fs::path&,const std::string&,const std::string&,bool)> remember_success;
        std::function<void()> before_run;
        std::vector<std::string> arguments;
    };
#ifdef __APPLE__
    std::string macos_link_sdk(const std::string& selected,const fs::path& compiler_executable) {
        std::string sdk=selected;
        if(sdk.empty())if(const auto* value=std::getenv("SDKROOT"))sdk=value;
        if(sdk.empty()){
            const auto bundled=compiler_executable.parent_path()/"llvm/sysroot";
            if(fs::is_regular_file(bundled/"usr/lib/libSystem.tbd"))sdk=bolt::path_to_utf8(bundled);
        }
        if(sdk.empty()){
            // Fixed command: no user-controlled content is passed to a shell.
            auto* pipe=popen("/usr/bin/xcrun --show-sdk-path","r");if(pipe){char buffer[1024];while(fgets(buffer,sizeof(buffer),pipe))sdk+=buffer;auto status=pclose(pipe);if(status!=0)sdk.clear();while(!sdk.empty()&&std::isspace(static_cast<unsigned char>(sdk.back())))sdk.pop_back();}
        }
        if(sdk.empty()||!fs::is_directory(bolt::path_from_utf8(sdk)))throw std::runtime_error("macOS SDK not found; install Command Line Tools or pass --sysroot SDK_PATH");
        return sdk;
    }
#endif
    bool uses_standalone_library(const bolt::Program& program) {
        return std::any_of(program.classes.begin(),program.classes.end(),[](const bolt::Class& c){
            if(!bolt::emitted_class(c))return false;
            if(c.is_interface && c.name!="System.IStringable")return c.generic_parameters.empty() && c.pack_parameter.empty();
            return (!c.interfaces.empty() && !c.is_native && c.generic_parameters.empty() && c.pack_parameter.empty()) ||
                   c.name=="System.Converter" || c.name=="Bolt.Runtime.ScalarConversion" || c.name=="Bolt.Runtime.Values" ||
                   c.token.at.file=="<bolt-stdlib>/System/Ecs.bolt" ||
                   c.name=="Bolt.Runtime.ThreadKernel" || c.name=="Bolt.Runtime.ValueTree" ||
                   c.token.at.file=="<bolt-stdlib>/System/Random.bolt" ||
                   c.token.at.file=="<bolt-stdlib>/System/Security/Cryptography/RandomNumberGenerator.bolt" ||
                   c.token.at.file=="<bolt-stdlib>/System/Reflection.bolt" ||
                   c.token.at.file=="<bolt-stdlib>/System/Diagnostics.bolt" ||
                   c.token.at.file=="<bolt-stdlib>/System/PlatformProcess.bolt" ||
                   c.token.at.file=="<bolt-stdlib>/System/PlatformSockets.bolt" ||
                   c.token.at.file=="<bolt-stdlib>/System/NetTypes.bolt";
        }) || std::any_of(program.source_types.begin(),program.source_types.end(),[](const bolt::SourceType& t){
            return t.token.text=="IStringable" && !t.token.at.file.starts_with("<bolt-stdlib>");
        });
    }

    int build_direct(const bolt::Program& program,const fs::path& root,const fs::path& target,const fs::path& compiler_executable,const NativeBuild& options,std::optional<bolt::ir::Module> module={}) {
        auto run_application=[&](){application_phase();if(options.before_run)options.before_run();bolt::project::WorkingDirectory cwd(options.work_dir);std::vector<std::string> command{bolt::path_to_utf8(target)};command.insert(command.end(),options.arguments.begin(),options.arguments.end());return process(command);};
        const auto backend=native_backend_name(options.backend,options.optimize,options.target_triple);
        bolt::native_backend::Options codegen;codegen.release=options.optimize;codegen.debug_symbols=options.debug_info;codegen.llvm_library=options.llvm;codegen.target=options.target_triple;
        const auto triple=bolt::native_backend::aot_target(codegen.target);
        const bool cross=triple!=bolt::native_backend::host_target();
        const bool archive=options.project_kind=="static-library";
        const bool shared=options.project_kind=="shared-library";
#if defined(__linux__)
        if(cross&&!archive&&!options.object_only&&(options.runtime_dir.empty()||options.sysroot.empty()))
            throw std::runtime_error("cross-target linking requires --runtime-dir TARGET_SDK/bin and --sysroot TARGET_GLIBC_ROOT; use --object-only to emit an unlinked object");
#endif
        const auto runtime_dir=options.runtime_dir.empty()?compiler_executable.parent_path():fs::absolute(bolt::path_from_utf8(options.runtime_dir));
        bool standalone = false;
#if defined(_M_X64)
        std::vector<fs::path> windows_libs;
        if(options.project_kind.empty() && !cross && options.optimize && !options.require_runtime_free && !options.object_only && backend=="llvm")
            windows_libs=windows_aot_libraries(options.sysroot);
        standalone=options.optimize&&!windows_libs.empty()&&fs::is_regular_file(runtime_dir/"BoltAotRuntime.lib");
#elif defined(__APPLE__)
        const auto sdk=(archive||options.object_only)?std::string{}:macos_link_sdk(options.sysroot,compiler_executable);
        const auto cxx=runtime_dir/"aot";
        const std::vector<std::string> static_cxx{"libc++abi.a","libc++.a","libcompiler_rt.a"};
        const bool bundled_cxx=std::all_of(static_cxx.begin(),static_cxx.end(),
            [&](const auto& file){return fs::is_regular_file(cxx/file);});
        const bool system_cxx=!sdk.empty()&&fs::is_regular_file(bolt::path_from_utf8(sdk)/"usr/lib/libc++.tbd");
        standalone=options.optimize&&!options.object_only&&fs::is_regular_file(runtime_dir/"libBoltCore.a")&&(bundled_cxx||system_cxx);
#elif defined(__FreeBSD__)
        standalone=options.optimize&&!options.object_only&&fs::is_regular_file(runtime_dir/"libBoltCore.a");
#elif defined(__linux__)
        standalone = options.optimize && !options.object_only && fs::is_regular_file(runtime_dir/"BoltAotRuntime.o");
#endif
        if(!options.project_kind.empty())standalone=false;
        bolt::ir::LowerOptions lower_options{.optimize_loops=options.optimize,.standalone_executable=standalone,
            .try_runtime_free_entry=(options.optimize||options.require_runtime_free)&&options.project_kind.empty()&&!options.object_only&&backend=="llvm",
            .source_core=options.require_runtime_free};
        std::string engine=backend=="llvm"?"LLVM "+bolt::native_backend::llvm_version(codegen):"Bolt Fast "+triple;
        if(!module && lower_options.try_runtime_free_entry)module=bolt::ir::lower(program,lower_options);
        const bool runtime_free=module&&module->runtime_free();
        if(options.require_runtime_free&&!runtime_free) {
            const auto reason=module?module->runtime_requirement:"this executable build mode";
            throw std::runtime_error("--no-runtime: Bolt runtime required ("+reason+").");
        }
        if(runtime_free)standalone=true;
#ifdef _WIN32
        if(standalone&&!runtime_free) {
            if(!module)module=bolt::ir::lower(program,lower_options);
            // Runtime.Execute* needs the full compiler and interpreter in the shared runtime.
            if(std::any_of(module->externals.begin(),module->externals.end(),[](const auto& s){return s.name=="br_runtime";})) {
                standalone=false;
                lower_options.standalone_executable=false;
                module.reset();
            }
        }
        const auto runtime=runtime_dir/(standalone?"BoltAotRuntime.lib":"BoltRuntime.dll");
        auto import=runtime_dir/"BoltRuntime.lib";
        if(!fs::is_regular_file(import))import=runtime_dir/"libBoltRuntime.dll.a";
#elif defined(__APPLE__)
        const auto runtime=runtime_dir/(standalone?"libBoltCore.a":"libBoltRuntime.dylib");
#elif defined(__FreeBSD__)
        const auto runtime=runtime_dir/(standalone?"libBoltCore.a":"libBoltRuntime.so");
#else
        // SDKs with a GNU AOT runtime link it into the executable. Other
        // SDKs and development builds keep their original shared deployment.
        const auto runtime=runtime_dir/(standalone ? "BoltAotRuntime.o" : "libBoltRuntime.so");
#endif
        const auto companion=(standalone||archive) ? fs::path{} : target.parent_path()/runtime.filename();
        if(!archive&&!options.object_only&&!runtime_free&&!fs::is_regular_file(runtime))throw std::runtime_error("native runtime not found: "+bolt::path_to_utf8(runtime)+". Build/install BoltRuntime too, or select --runtime-dir PATH.");
        protect_sources(program,target);if(!companion.empty())protect_sources(program,companion);
        auto fingerprint=hash_bytes(14695981039346656037ull,"Bolt-native-cli-v8:"+engine+triple+std::to_string(options.release));
        bool reusable=!options.rebuild;
        if(options.debug_info) {
            fingerprint=debug_source_fingerprint(fingerprint,program);
            // The compiler contains the standard library and the code generators.
            // On Linux /proc/self/exe continues to name this process after an SDK upgrade.
            fs::path image=compiler_executable;
#ifdef __linux__
            std::error_code image_error;
            if(fs::is_regular_file("/proc/self/exe",image_error))image="/proc/self/exe";
#endif
            try { fingerprint=hash_file(fingerprint,image); }
            catch(const std::runtime_error&) { reusable=false; }
        } else {
            if(!module)module=bolt::ir::lower(program,lower_options);
            fingerprint=hash_bytes(fingerprint,bolt::ir::dump(*module));
        }
        fingerprint=hash_bytes(fingerprint,program.project_identity+bolt::preprocessor_identity(program.preprocessor)+std::to_string(options.debug_info)+std::to_string(options.optimize));
        fingerprint=hash_bytes(fingerprint,options.linker+"\n"+options.sysroot+"\n"+options.llvm+std::to_string(options.object_only));
        std::string runtime_digest;
        fingerprint=hash_bytes(fingerprint,options.project_kind);
        if(!options.object_only&&!archive){
            for(const auto& file:options.link_libraries)fingerprint=hash_file(hash_bytes(fingerprint,file),bolt::path_from_utf8(file));
            if(!runtime_free)runtime_digest=bolt::cli_state::file_hash(runtime);
            fingerprint=hash_bytes(fingerprint,runtime_free?"no-bolt-runtime":runtime_digest);
#if defined(__APPLE__)
            fingerprint=hash_bytes(fingerprint,sdk);
            if(standalone&&!runtime_free&&bundled_cxx)for(const auto& file:static_cxx)fingerprint=hash_file(fingerprint,cxx/file);
#elif defined(__linux__)
            if(fs::is_regular_file(runtime_dir/"BoltStart.o"))fingerprint=hash_file(fingerprint,runtime_dir/"BoltStart.o");
#endif
        }
        const auto key=hex_hash(hash_bytes(14695981039346656037ull,bolt::path_to_utf8(target)));
        const auto record=root/".bolt"/"build"/(key+".txt");protect_sources(program,record);
        // Verify identical runtime bytes once. Reusing the verified companion also
        // preserves its file identity on a warm edit (including a loaded DLL).
        const bool has_companion=!companion.empty() && !options.object_only;
        const bool same_runtime=has_companion && !fs::is_symlink(companion) && fs::is_regular_file(companion) &&
            bolt::cli_state::file_hash(companion)==runtime_digest;
        fs::path import_target;
#ifdef _WIN32
        if(shared&&!options.object_only){
            import_target=target;import_target.replace_extension(".lib");
            if(import_target==target)throw std::runtime_error("a shared library and its import library need distinct output filenames");
            protect_sources(program,import_target);
        }
#endif
        if(reusable&&fs::is_regular_file(target)&&fs::is_regular_file(record)) {
            std::ifstream in(record);std::string saved,checksum;in>>saved>>checksum;
            std::string saved_import;in>>saved_import;
            const bool intact_import=import_target.empty() || (fs::is_regular_file(import_target)&&bolt::cli_state::file_hash(import_target)==saved_import);
            if(intact_import&&saved==hex_hash(fingerprint)&&(!has_companion||same_runtime)&&checksum==bolt::cli_state::file_hash(target)){
                std::cout<<"Up to date ("<<backend<<"): "<<bolt::path_to_utf8(target)<<'\n';std::cout.flush();
                if(!options.object_only&&options.remember_success)options.remember_success(backend,companion,checksum,runtime_digest,runtime_free);
                if(options.timing)options.timing->finish(options.release?"Release compilation":"Debug compilation",true);
                return options.run?run_application():0;
            }
        }
        fs::create_directories(record.parent_path());
        const auto before_lower=std::chrono::steady_clock::now();
        if(!module)module=bolt::ir::lower(program,lower_options);
#ifdef _WIN32
        if(shared&&!options.object_only&&std::none_of(module->functions.begin(),module->functions.end(),[](const auto& f){return f.exported;})){
            // link.exe and lld-link create an import library only for a DLL with exports.
            bolt::ir::Function marker;marker.name="bolt_empty_library";marker.exported=true;
            marker.blocks.emplace_back().code.push_back({.op=bolt::ir::Op::Return});
            module->functions.push_back(std::move(marker));
        }
#endif
        const auto begin_codegen=std::chrono::steady_clock::now();
        auto object_bytes=backend=="fast"?bolt::native_backend::fast_object(*module,codegen):bolt::native_backend::llvm_object(*module,codegen);
        const auto generated=std::chrono::steady_clock::now();
        const auto bytes=std::string(reinterpret_cast<const char*>(object_bytes.data()),object_bytes.size());
        if(options.object_only){
            write_file(record,hex_hash(fingerprint)+" "+bolt::cli_state::bytes_digest(bytes)+"\n");
            write_file(target,bytes);auto marker=target;marker+=".bolt.native";protect_sources(program,marker);write_file(marker,"Bolt native artifact 0.4\n");std::cout<<"Emitted native object ("<<engine<<"): "<<bolt::path_to_utf8(target)<<'\n';if(options.timing)options.timing->finish(options.release?"Release object compilation":"Debug object compilation");return 0;}
        auto object=options.intermediate_dir.empty()?target:options.intermediate_dir/target.filename();
#ifdef _WIN32
        object+=".bolt.obj";
#else
        object+=".bolt.o";
#endif
        protect_sources(program,object);write_file(object,bytes);
        auto temporary=target;temporary+=".linking";
#ifdef _WIN32
        temporary+=".exe";
#endif
        if(shared)temporary=options.intermediate_dir/"link"/target.filename();
        for(auto path=temporary;path!=path.root_path();path=path.parent_path())
            if(fs::is_symlink(path))throw std::runtime_error("native link staging path must not traverse a symlink");
        protect_sources(program,temporary);
        fs::create_directories(temporary.parent_path());
        fs::remove(temporary);
        fs::create_directories(target.parent_path());
        fs::path staged_import;
        if(!import_target.empty()){staged_import=temporary;staged_import.replace_extension(".lib");}
        fs::path linker;
#ifndef _WIN32
        if(archive){
#ifdef __linux__
            if(cross)linker=find_command({bolt::linux_target::gnu_prefix(triple)+"ar","llvm-ar"});
            else
#endif
            linker=find_command({"llvm-ar","ar"});
        }else
#endif
        if(!options.linker.empty())linker=find_command({options.linker});
        else if(const auto* configured=std::getenv("BOLT_LINKER"))linker=find_command({configured});
        else {
#ifdef _WIN32
            linker=find_command({bolt::path_to_utf8(compiler_executable.parent_path() / "lld-link.exe"),
                                 "lld-link.exe","link.exe","lld-link"});
#elif defined(__APPLE__)
            linker=find_command({bolt::path_to_utf8(compiler_executable.parent_path() / "llvm/bin/ld64.lld"), "/usr/bin/ld"});
#elif defined(__FreeBSD__)
            linker=find_command({"/usr/bin/ld.lld","ld.lld","ld"});
#else
            linker=cross?find_command({bolt::linux_target::gnu_prefix(triple)+"ld","ld.lld"}):find_command({"ld.lld","ld"});
#endif
        }
        if(linker.empty()&&archive)throw std::runtime_error("native archiver not found for "+triple);
        if(linker.empty())throw std::runtime_error("native linker not found; install LLD/system linker or use --linker PATH. The direct backends do not invoke a C++ compiler.");
        std::vector<std::string> invocation{bolt::path_to_utf8(linker)};
        if(archive){
#ifdef _WIN32
            invocation.insert(invocation.end(),{"/lib","/NOLOGO",coff_machine,"/OUT:"+bolt::path_to_utf8(temporary),bolt::path_to_utf8(object)});
#else
            invocation.insert(invocation.end(),{"rcs",bolt::path_to_utf8(temporary),bolt::path_to_utf8(object)});
#endif
        }else{
#ifdef _WIN32
        invocation.insert(invocation.end(),{"/NOLOGO",coff_machine,"/SUBSYSTEM:CONSOLE","/DYNAMICBASE","/NXCOMPAT","/STACK:8388608",bolt::path_to_utf8(object),"/OUT:"+bolt::path_to_utf8(temporary)});
        if(shared){
            invocation.insert(invocation.end(),{"/DLL","/NOENTRY","/IMPLIB:"+bolt::path_to_utf8(staged_import)});
            auto definition=object;definition+=".def";
            std::string exports="EXPORTS\n";
            for(const auto& fn:module->functions)if(fn.exported)exports+=fn.name+"\n";
            protect_sources(program,definition);write_file(definition,exports);
            invocation.push_back("/DEF:"+bolt::path_to_utf8(definition));
        }
        if(runtime_free) {
            if(!shared)invocation.push_back("/ENTRY:bolt_pure_start");
            invocation.insert(invocation.end(),{"/NODEFAULTLIB","/OPT:REF","/OPT:ICF","/INCREMENTAL:NO"});
#if defined(_M_X64)
            // The compatibility build may use static C helpers. The strict
            // build supplies its own memory initialization and stack probes.
            if(!windows_libs.empty()){
                for(const auto& dir:windows_libs)invocation.push_back("/LIBPATH:"+bolt::path_to_utf8(dir));
                invocation.insert(invocation.end(),{"libcmt.lib","libvcruntime.lib","libucrt.lib"});
            }
#endif
        } else
#if defined(_M_X64)
        if(standalone) {
            for(const auto& dir:windows_libs)invocation.push_back("/LIBPATH:"+bolt::path_to_utf8(dir));
            invocation.insert(invocation.end(),{"/ENTRY:wmainCRTStartup","/OPT:REF","/OPT:ICF","/INCREMENTAL:NO",
                "kernel32.lib","libcmt.lib","libvcruntime.lib","libucrt.lib"});
            invocation.insert(invocation.end(),{"/ALTERNATENAME:wmain=main","/DELAYLOAD:shell32.dll","/DELAYLOAD:ws2_32.dll",bolt::path_to_utf8(runtime),
                "advapi32.lib","shell32.lib","ws2_32.lib","delayimp.lib","libcpmt.lib","oldnames.lib"});
        } else
#endif
        {
            if(!fs::is_regular_file(import))throw std::runtime_error("BoltRuntime import library is missing; build the full solution");
            if(!shared)invocation.push_back("/ENTRY:bolt_start");
            invocation.insert(invocation.end(),{"/NODEFAULTLIB",bolt::path_to_utf8(import)});
        }
        if(std::any_of(module->externals.begin(),module->externals.end(),[](const auto& symbol){return symbol.foreign;})) {
            const auto kernel32=
#if defined(_M_X64)
                standalone&&!windows_libs.empty()?windows_libs.back()/"kernel32.lib":
#endif
                windows_kernel32_library(options.sysroot, compiler_executable.parent_path());
            invocation.push_back(bolt::path_to_utf8(kernel32));
            if(std::any_of(module->externals.begin(),module->externals.end(),[](const auto& symbol){
                return symbol.foreign && symbol.name=="CommandLineToArgvW";
            }))invocation.push_back(bolt::path_to_utf8(kernel32.parent_path()/"shell32.lib"));
            if(std::any_of(module->externals.begin(),module->externals.end(),[](const auto& symbol){
                return symbol.foreign && (symbol.name=="WSAStartup" || symbol.name=="closesocket" ||
                    symbol.name=="inet_pton" || symbol.name=="inet_ntop");
            }))invocation.push_back(bolt::path_to_utf8(kernel32.parent_path()/"ws2_32.lib"));
            // BCryptGenRandom is exported by bcrypt.dll. Keep the RNG a direct
            // OS ABI call while linking the matching Windows SDK import library.
            if(std::any_of(module->externals.begin(),module->externals.end(),[](const auto& symbol){return symbol.foreign&&symbol.name=="BCryptGenRandom";})) {
                const auto bcrypt=kernel32.parent_path()/"bcrypt.lib";
                if(!fs::is_regular_file(bcrypt))throw std::runtime_error("Windows secure random linking requires bcrypt.lib beside kernel32.lib in the Windows SDK");
                invocation.push_back(bolt::path_to_utf8(bcrypt));
            }
        }
        if(options.debug_info&&backend=="llvm"){auto pdb=target;pdb+=".pdb";protect_sources(program,pdb);invocation.push_back("/DEBUG");invocation.push_back("/PDB:"+bolt::path_to_utf8(pdb));}
#elif defined(__APPLE__)
        const std::string arch=triple.starts_with("arm64-")?"arm64":"x86_64";
        const auto minimum=bolt::native_backend::macos_deployment_target();
        if(!standalone)bolt::apple_target::require_compatible(runtime,minimum,arch=="arm64");
        const auto sdk_release=bolt::apple_target::sdk_version(bolt::path_from_utf8(sdk),minimum);
        invocation.insert(invocation.end(),{"-arch",arch,"-platform_version","macos",minimum,sdk_release,"-syslibroot",sdk,
            "-o",bolt::path_to_utf8(temporary),bolt::path_to_utf8(object),"-lSystem"});
        if(shared)invocation.insert(invocation.end(),{"-dylib","-install_name","@rpath/"+bolt::path_to_utf8(target.filename())});
        if(!runtime_free)invocation.push_back(bolt::path_to_utf8(runtime));
        if(standalone&&!runtime_free) {
            invocation.push_back("-dead_strip");
            if(bundled_cxx)for(const auto& file:static_cxx)invocation.push_back(bolt::path_to_utf8(cxx/file));
            else invocation.push_back("-lc++");
        } else if(!standalone || shared || !options.link_libraries.empty())invocation.insert(invocation.end(),{"-rpath",shared?"@loader_path":"@executable_path"});
#elif defined(__FreeBSD__)
        const fs::path sysroot=options.sysroot.empty()?fs::path("/"):fs::absolute(bolt::path_from_utf8(options.sysroot));
        const auto libc=sysroot/"usr/lib";
        if(shared){
            invocation.insert(invocation.end(),{"--sysroot="+bolt::path_to_utf8(sysroot),"-shared","-soname",bolt::path_to_utf8(target.filename()),
                "-o",bolt::path_to_utf8(temporary),bolt::path_to_utf8(object),"-L"+bolt::path_to_utf8(libc),"-lexecinfo","-lc","-rpath","$ORIGIN"});
            if(runtime_free)invocation.insert(invocation.end(),{"-z","defs","--as-needed","-lm","-lpthread"});
            else invocation.push_back(bolt::path_to_utf8(runtime));
        }else{
        for(const auto* name:{"Scrt1.o","crti.o","crtbeginS.o","crtendS.o","crtn.o"}) {
            if(!fs::is_regular_file(libc/name))throw std::runtime_error("FreeBSD startup object missing: "+bolt::path_to_utf8(libc/name));
        }
        invocation.insert(invocation.end(),{"--sysroot="+bolt::path_to_utf8(sysroot),"-pie","--eh-frame-hdr",
            "--dynamic-linker","/libexec/ld-elf.so.1","--hash-style=both","--enable-new-dtags","-z","noexecstack","-z","relro","-z","now",
            "-o",bolt::path_to_utf8(temporary),bolt::path_to_utf8(libc/"Scrt1.o"),bolt::path_to_utf8(libc/"crti.o"),
            bolt::path_to_utf8(libc/"crtbeginS.o"),"-L"+bolt::path_to_utf8(libc),"-L"+bolt::path_to_utf8(sysroot/"lib"),bolt::path_to_utf8(object)});
        if(!runtime_free)invocation.push_back(bolt::path_to_utf8(runtime));
        if(standalone){
            invocation.insert(invocation.end(),{"--gc-sections","--as-needed"});
            if(!runtime_free)invocation.push_back("-lc++");
            if(runtime_free&&!options.link_libraries.empty())invocation.insert(invocation.end(),{"-rpath","$ORIGIN"});
            invocation.insert(invocation.end(),{"-lm","-lexecinfo","-lpthread"});
        } else invocation.insert(invocation.end(),{"-z","origin","-rpath","$ORIGIN","--as-needed","-lexecinfo"});
        invocation.insert(invocation.end(),{"-lgcc","--as-needed","-lgcc_s","--no-as-needed","-lc","-lgcc","--as-needed","-lgcc_s","--no-as-needed",
            bolt::path_to_utf8(libc/"crtendS.o"),bolt::path_to_utf8(libc/"crtn.o")});
        }
#else
        const auto start=runtime_dir/"BoltStart.o";
        if(!shared&&!fs::is_regular_file(start))throw std::runtime_error("BoltStart.o is missing; build/install the full Bolt package");
        const auto arch=triple.substr(0,triple.find('-'));
#ifdef BOLT_LINUX_MUSL
        const auto multiarch=arch+"-linux-musl";
        const auto interpreter="/lib/ld-musl-"+arch+".so.1";
        const auto libc_name="ld-musl-"+arch+".so.1";
        const std::string libc_label="musl";
#else
        const auto multiarch=arch+"-linux-gnu";
        const std::string libc_name="libc.so.6";
        const std::string libc_label="glibc";
        const auto interpreter=arch=="aarch64"?"/lib/ld-linux-aarch64.so.1":arch=="riscv64"?"/lib/ld-linux-riscv64-lp64d.so.1":"/lib64/ld-linux-x86-64.so.2";
#endif
        const auto prefix=options.sysroot.empty()?fs::path("/"):fs::absolute(bolt::path_from_utf8(options.sysroot));
        std::vector<fs::path> directories{prefix/"lib"/multiarch,prefix/"usr/lib"/multiarch,prefix/"lib64",prefix/"usr/lib64",prefix/"lib",prefix/"usr/lib"};
        for(auto& directory:directories)directory=bolt::linux_target::in_sysroot(prefix,directory);
        auto loader=bolt::linux_target::in_sysroot(prefix,prefix/fs::path(interpreter).relative_path());
        // Cross-toolchains may store the interpreter in lib/ instead of its
        // eventual /lib64 installation path. PT_INTERP always uses that path.
        if(!fs::is_regular_file(loader))for(const auto& dir:directories) {
            auto candidate=bolt::linux_target::in_sysroot(prefix,dir/fs::path(interpreter).filename());
            if(fs::is_regular_file(candidate)){loader=candidate;break;}
        }
        if(!fs::is_regular_file(loader))throw std::runtime_error("Linux/"+libc_label+" loader not found at "+bolt::path_to_utf8(loader)+". Install the target "+libc_label+" or select a matching --sysroot.");
        fs::path libc;
        for(const auto& dir:directories) {
            auto candidate=bolt::linux_target::in_sysroot(prefix,dir/libc_name);
            if(fs::is_regular_file(candidate)){libc=candidate;break;}
        }
        if(libc.empty())throw std::runtime_error(libc_name+" was not found in the host/sysroot library paths");
        if(cross){
            if(!shared)bolt::linux_target::require_elf(start,triple);
            for(const auto& binary:{libc,loader})bolt::linux_target::require_elf(binary,triple);
            if(!runtime_free)bolt::linux_target::require_elf(runtime,triple);
        }
        if(!options.sysroot.empty()) {
            // GNU ld resolves the shared runtime's DT_NEEDED dependencies at
            // link time. Search the chosen sysroot without embedding host paths.
            for(const auto& directory:directories)if(fs::is_directory(directory))
                invocation.insert(invocation.end(),{"-rpath-link",bolt::path_to_utf8(directory)});
        }
        if(shared)invocation.insert(invocation.end(),{"-shared","-soname",bolt::path_to_utf8(target.filename()),"-z","defs","-o",bolt::path_to_utf8(temporary),bolt::path_to_utf8(object)});
        else invocation.insert(invocation.end(),{"-pie","-e","_start","--dynamic-linker",interpreter,"-z","noexecstack","-z","relro","-z","now","-o",bolt::path_to_utf8(temporary),bolt::path_to_utf8(start),bolt::path_to_utf8(object)});
#ifdef BOLT_LINUX_MUSL
        // The startup object's separately collectible trace adapter calls the
        // system unwinder. libgcc_s and LLVM libunwind expose the same C ABI.
        invocation.insert(invocation.end(), {"--gc-sections", "--as-needed"});
        bool found_unwinder = false;
        for (const auto* soname : {"libgcc_s.so.1", "libunwind.so.1"}) {
            for (const auto& directory : directories) {
                const auto library = bolt::linux_target::in_sysroot(prefix, directory/soname);
                if (!fs::is_regular_file(library)) continue;
                invocation.push_back(bolt::path_to_utf8(library));
                found_unwinder = true;
                break;
            }
            if (found_unwinder) break;
        }
#endif
        invocation.push_back("--eh-frame-hdr");
        if (standalone) {
            invocation.insert(invocation.end(), {"--gc-sections", "--as-needed"});
            if (!runtime_free) invocation.push_back(bolt::path_to_utf8(runtime));
            for (const auto* name : {"libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1"}) {
                for (const auto& dir : directories) {
                    const auto candidate=bolt::linux_target::in_sysroot(prefix,dir/name);
                    if(fs::is_regular_file(candidate)){invocation.push_back(bolt::path_to_utf8(candidate));break;}
                }
            }
            invocation.push_back(bolt::path_to_utf8(libc));
#ifndef BOLT_LINUX_MUSL
            // glibc supplies ABI entry points such as atexit in this archive.
            // The distribution's libc.so linker script normally adds it.
            if (!runtime_free) for (const auto& dir : directories) {
                auto nonshared = bolt::linux_target::in_sysroot(prefix, dir / "libc_nonshared.a");
                if (fs::is_regular_file(nonshared)) {
                    invocation.push_back(bolt::path_to_utf8(nonshared));
                    break;
                }
            }
#endif
            // TLS references in the prelinked runtime resolve in the loader.
            invocation.push_back(bolt::path_to_utf8(loader));
            invocation.insert(invocation.end(), {"-rpath", "$ORIGIN"});
        } else {
            invocation.insert(invocation.end(), {"-L",bolt::path_to_utf8(runtime_dir),"-lBoltRuntime",bolt::path_to_utf8(libc),"-rpath","$ORIGIN"});
        }
#endif
        invocation.insert(invocation.end(),options.link_libraries.begin(),options.link_libraries.end());
        }
        verbose_command(invocation,options.verbose);
        const int status=process(invocation,"Native linker");std::error_code error;
        if(status!=0){fs::remove(temporary,error);return status;}
        const auto executable_digest=bolt::cli_state::file_hash(temporary);
        // A cache failure must occur before publishing a newly linked executable.
        const auto import_digest=staged_import.empty()?std::string{}:bolt::cli_state::file_hash(staged_import);
        write_file(record,hex_hash(fingerprint)+" "+executable_digest+" "+import_digest+"\n");
        if(!staged_import.empty())bolt::cli_state::replace_file(staged_import,import_target);
        if(has_companion&&!same_runtime&&fs::weakly_canonical(companion)!=fs::weakly_canonical(runtime)){
            auto staged=companion;staged+=".bolt-copy";protect_sources(program,staged);
            fs::copy_file(runtime,staged,fs::copy_options::overwrite_existing);
            bolt::cli_state::replace_file(staged,companion);
        }
        bolt::cli_state::replace_file(temporary,target);
        auto marker=target;marker+=".bolt.native";protect_sources(program,marker);write_file(marker,"Bolt native artifact 0.4\n");
        if(options.verbose){auto seconds=[](auto a,auto b){return std::chrono::duration<double>(b-a).count();};std::ostringstream report;report<<"[bolt] lowering: "<<seconds(before_lower,begin_codegen)<<" s; "<<engine<<" code generation: "<<seconds(begin_codegen,generated)<<" s; link/package: "<<seconds(generated,std::chrono::steady_clock::now())<<" s";status_message("info",report.str());}
        if(options.remember_success)options.remember_success(backend,companion,executable_digest,runtime_digest,runtime_free);
        std::cout<<"Built "<<(options.release?"Release":"Debug")<<" ("<<engine<<"): "<<bolt::path_to_utf8(target)<<'\n';std::cout.flush();
        if(options.timing)options.timing->finish(options.release?"Release compilation":"Debug compilation");
        return options.run?run_application():0;
    }
    int scaffold(const std::string& name) {
        auto root=fs::absolute(bolt::path_from_utf8(name));
        if(fs::exists(root))throw std::runtime_error("refusing to overwrite existing path '"+name+"'");
        fs::create_directories(root);
        write_file(root/"Program.bolt","import Console\n\nstatic class Program\n{\npublic:\n    void Main()\n    {\n        Console.WriteLine(\"Hello from Bolt!\");\n    }\n}\n");
        write_file(root/"bolt.project","{\"schemaVersion\": 1}\n");
        write_file(root/".gitignore","bin/\n.bolt/\n.vs/\n");
        std::cout<<"Created "<<bolt::path_to_utf8(root)<<"\nRun: bolt run "<<std::quoted(name)<<'\n';return 0;
    }
    int clean_project(const std::string& name) {
        if(auto project=bolt::project::resolve(bolt::path_from_utf8(name)))return bolt::project::clean_owned(project->main);
        const auto root=fs::weakly_canonical(fs::absolute(bolt::path_from_utf8(name)));
        if(!fs::is_directory(root))throw std::runtime_error("clean requires a project folder");
        std::vector<fs::path> remove;
        auto inspect=[&](const fs::path& base,bool outputs){
            if(!fs::exists(base))return;
            if(fs::is_symlink(base))throw std::runtime_error("refusing to clean a symlinked output/cache directory");
            for(fs::recursive_directory_iterator it(base),end;it!=end;++it){
                if(it->is_symlink())throw std::runtime_error("refusing to clean through symlinks");
                if(!it->is_regular_file())continue;
                auto ext=bolt::path_to_utf8(it->path().extension());std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                if(ext==".bolt")throw std::runtime_error("refusing to clean a directory containing .bolt source files");
                const auto path=it->path();
                if(outputs){
                    auto filename=bolt::path_to_utf8(path);
                    if(filename.ends_with(".bolt.native")){
                        std::ifstream marker(path);std::string magic;std::getline(marker,magic);
                        if(magic!="Bolt native artifact 0.4")continue;
                        auto executable=bolt::path_from_utf8(filename.substr(0,filename.size()-12));
                        for(const auto& suffix:{"",".bolt.o",".bolt.obj",".pdb",".linking",".linking.exe",".bolt.native"}){
                            auto p=bolt::path_from_utf8(bolt::path_to_utf8(executable)+suffix);
                            if(fs::is_symlink(p))throw std::runtime_error("refusing to clean a symlinked output");
                            if(fs::is_regular_file(p))remove.push_back(p);
                        }
                        continue;
                    }
                    if(!filename.ends_with(".bolt.cpp"))continue;
                    std::ifstream in(path);std::string line;std::getline(in,line);
                    if(!line.starts_with("// Generated by Bolt "))continue;
                    remove.push_back(path);
                    auto executable=bolt::path_from_utf8(filename.substr(0,filename.size()-9));
                    for(const auto& suffix:{"",".obj",".pdb",".compiler.pdb",".linking",".linking.exe"}){
                        auto p=bolt::path_from_utf8(bolt::path_to_utf8(executable)+suffix);
                        if(fs::is_symlink(p))throw std::runtime_error("refusing to clean a symlinked output");
                        if(fs::is_regular_file(p))remove.push_back(p);
                    }
                } else if(path.filename()=="bolt_pch.hpp"||path.filename()=="bolt_pch.pch"||path.filename()=="bolt_pch.hpp.gch"||
                    (path.parent_path().filename()=="build"&&path.extension()==".txt"))remove.push_back(path);
                else if(path.parent_path().filename()=="run"&&path.extension()==".json") {
                    // Only owned successful-build receipts, not arbitrary JSON files.
                    try { (void)bolt::cli_state::read_receipt(path);remove.push_back(path); }
                    catch(const std::exception&) {}
                }
            }
        };
        const auto build_root=bolt::build_paths::root_for(root);
        inspect(build_root/"bin",true);inspect(build_root/".bolt",false);
        for(const auto& p:remove)fs::remove(p);
        std::cout<<"Removed "<<remove.size()<<" recognized Bolt build files. Sources and unrelated files were kept.\n";return 0;
    }
    int cli(std::vector<std::string> args) {
        if(args.size()<2||args[1]=="--help"||args[1]=="-h"||args[1]=="help") {
            if(args.size()>3)throw std::runtime_error("usage: bolt help [command]");
            usage(args.size()==3?args[2]:""); return 0;
        }
        if(args[1]=="--version"||args[1]=="version") {
            if(args.size()==3 && args[2]=="--json") {
                using Json=bolt::tooling::Json;
                const Json info=Json::Object{
                    {"version", BOLT_VERSION_STRING},
                    {"executable", bolt::path_to_utf8(executable_path(args[0]))},
                    {"target", bolt::native_backend::host_target()},
                    {"runtimeAbi", static_cast<int>(bolt::native_abi::Version)},
                    {"compilerHostAbi", static_cast<int>(BOLT_COMPILER_HOST_ABI)}
                };
                std::cout<<info.dump()<<'\n';
            } else if(args.size()==2) {
                std::cout<<"Bolt " BOLT_VERSION_STRING " ("<<bolt::native_backend::host_target()<<")\n";
            } else throw std::runtime_error("usage: bolt version [--json]");
            return 0;
        }
        std::string command=args[1];
        if(command=="backends") {
            if(args.size()!=2)throw std::runtime_error("usage: bolt backends");
            std::cout<<"Host: "<<bolt::native_backend::host_target()<<"\nFast: "<<(bolt::native_backend::fast_supported()?"available (AOT/JIT)":"not available; auto Debug uses LLVM O0")<<'\n';
            try{std::cout<<"LLVM: "<<bolt::native_backend::llvm_version()<<" (AOT/JIT)\n";}catch(const std::exception& e){std::cout<<"not available: "<<e.what()<<'\n';}
            std::cout<<"Shared runtime ABI: core/collections, IO, memory/arenas, legacy system adapters\nProcesses, pipes, DNS and sockets: Bolt with direct platform calls\nNative threads: Bolt completion and synchronization, OS threads and atomic instructions\nC++: compatibility backend for unsupported features/FFI; requires an external C++20 compiler\n";return 0;
        }
        if(command=="lsp") { if(args.size()!=2)throw std::runtime_error("usage: bolt lsp"); return bolt::run_language_server(std::cin,std::cout,std::cerr); }
        if(args.size()>=3&&(args[2]=="--help"||args[2]=="-h")) { usage(command); return 0; }
        if(command=="init"||command=="create") {
            if(args.size()!=3)throw std::runtime_error("usage: bolt init <new-folder>");
            bolt::project::init(bolt::path_from_utf8(args[2]));
            std::cout<<"Created project: "<<args[2]<<"\n";return 0;
        }
        if(command=="project") {
            if(args.size()<4||args[2]!="info")throw std::runtime_error("usage: bolt project info <path> --format json");
            std::string selected;
            bolt::PreprocessorOptions inspect_options;
            for(std::size_t i=4;i<args.size();++i){if(args[i]=="--format"&&i+1<args.size()&&args[i+1]=="json"){++i;continue;}if(args[i]=="--profile"&&i+1<args.size()){selected=args[++i];continue;}if(args[i]=="--define"&&i+1<args.size()){inspect_options.defines[args[++i]]=true;continue;}throw std::runtime_error("unsupported project info option: "+args[i]);}
            auto model=bolt::project::resolve(bolt::path_from_utf8(args[3]));if(!model)throw std::runtime_error("no bolt.proj found");
            auto info=bolt::tooling::Json::parse(bolt::project::describe(*model,bolt::native_backend::host_target(),selected));
            info["artifact"]=bolt::path_to_utf8(bolt::project::artifact(model->main,bolt::native_backend::host_target(),selected.empty()?model->main.default_profile:selected));
            bolt::tooling::Json::Array entries;
            inspect_options.configuration=selected;
            bolt::tooling::Json::Object symbols;
            for(const auto& [name,value]:bolt::preprocessor_symbols(bolt::project_preprocessor_options(model->main.file,{},inspect_options)))symbols[name]=value;
            info["conditional_symbols"]=std::move(symbols);
            auto inspected=bolt::inspect_project(model->main.file,{},model->main.kind=="executable",inspect_options);
            if(inspected.program&&!inspected.program->entry_class.empty())if(const auto* c=inspected.program->find_class(inspected.program->entry_class))for(const auto& m:c->methods)if(m.name=="Main"&&m.is_public&&m.is_static&&m.generic_parameters.empty()&&m.pack_parameter.empty()&&(m.parameters.empty()||m.parameters.size()==1&&m.parameters[0].type=="string[]")){
                entries.emplace_back(bolt::tooling::Json::Object{{"class",c->name},{"method",m.name},{"file",m.token.at.file},{"line",m.token.at.line},{"column",m.token.at.column},{"offset",static_cast<std::int64_t>(m.token.at.offset)},{"source_hash",std::to_string(hash_bytes(14695981039346656037ull,inspected.program->sources.at(m.token.at.file)))}});
            }
            info["entrypoints"]=std::move(entries);
            info["source_diagnostics"]=bolt::tooling::Json::parse(bolt::format_diagnostics_json(inspected.diagnostics));
            std::cout<<info.dump()<<'\n';return 0;
        }
        if(command=="new"||command=="clean") {
            if(args.size()!=3)throw std::runtime_error("usage: bolt "+command+" <folder>");
            return command=="new"?scaffold(args[2]):clean_project(args[2]);
        }
        if(command!="run"&&command!="check"&&command!="emit"&&command!="emit-ir"&&command!="exec"&&command!="build"&&command!="compile")throw std::runtime_error("unknown command '"+command+"'; use bolt help");
        if(command!="exec"&&(args.size()<3||args[2].starts_with("-")))args.insert(args.begin()+2,".");
        if(args.size()<3)throw std::runtime_error("missing source argument; use --help");
        bool release=false,debug_set=false,release_set=false,lto=false,rebuild=false,pch=true,verbose=false,native_run=false,include_set=false,no_entry=false,jit_run=false,backend_set=false,object_only=false;
        bool runtime_run=false,no_compile=false,steps_set=false,native_set=false;
        bool args_set=false,debug_info=false,debug_info_set=false,optimize=false,deps_ready=false;
        std::string selected_profile,work_dir_option;
        std::string output,compiler,library,include=BOLT_INCLUDE_DIR,backend="auto",llvm,linker,runtime_dir,sysroot,requested_target;
        bool mobile_entry=false;
        bool show_timing=command=="run"||command=="compile"||command=="build",require_native=false,require_runtime_free=false;
        bolt::ExecutionOptions options;
        bolt::PreprocessorOptions preprocessing;
        std::vector<std::string> header_dirs,link_libraries,application_arguments;
        std::vector<fs::path> native_runtime_dirs;
        for(std::size_t i=3;i<args.size();++i) {
            const auto& option=args[i];
            if(option=="--"){args_set=true;application_arguments.assign(args.begin()+static_cast<std::ptrdiff_t>(i+1),args.end());break;}
            if(option=="--help"||option=="-h") { usage(command); return 0; }
            if(option=="--project-deps-ready"){deps_ready=true;continue;}
            if(option=="--debug-info"){debug_info=true;debug_info_set=true;continue;}
            if(option=="--no-debug-info"){debug_info=false;debug_info_set=true;continue;}
            if(option=="--runtime"){runtime_run=true;continue;}
            if(option=="--no-compile"){no_compile=true;continue;}
            if(option=="--require-native"){require_native=true;continue;}
            if(option=="--no-runtime"){require_runtime_free=true;require_native=true;continue;}
            if(option=="--timings"){show_timing=true;continue;}
            if(option=="--no-timings"){show_timing=false;continue;}
            if(option=="--no-entry") { no_entry=true; continue; }
            if(option=="--diagnostics") {
                if(i+1>=args.size())throw std::runtime_error("--diagnostics requires text, msbuild, or json");
                const auto& format=args[++i];
                if(format=="text")diagnostic_format=bolt::DiagnosticFormat::Text;
                else if(format=="msbuild")diagnostic_format=bolt::DiagnosticFormat::MSBuild;
                else if(format=="json")diagnostic_format=bolt::DiagnosticFormat::Json;
                else throw std::runtime_error("unknown diagnostic format '"+format+"'");
                continue;
            }
            if(option=="--debug") {
                debug_set=true;
                release=false;
                continue;
            }
            if(option=="--release") {
                release_set=true;
                release=true;
                continue;
            }
            if(option=="--lto") {
                lto=true;
                continue;
            }
            if(option=="--rebuild") {
                rebuild=true;
                continue;
            }
            if(option=="--no-pch") {
                pch=false;
                continue;
            }
            if(option=="--verbose") {
                verbose=true;
                continue;
            }
            if(option=="--native") {
                native_set=true;
                continue;
            }
            if(option=="--jit"){jit_run=true;continue;}
            if(option=="--object-only"){object_only=true;continue;}
            if(i+1>=args.size())throw std::runtime_error("missing value for '"+option+"'");
            const auto& v=args[++i];
            if(option=="--target") {
                struct HostedTarget {const char* platform;const char* arch;};
                static const std::map<std::string,HostedTarget> hosted={
                    {"web",{"Web","WASM64"}},
                    {"android-arm64",{"Android","ARM64"}},{"android-x64",{"Android","X64"}},
                    {"android-armv7",{"Android","ARM32"}},{"android-x86",{"Android","X86"}},
                    {"ios-arm64",{"IOS","ARM64"}},{"ios-sim-arm64",{"IOS","ARM64"}},{"ios-sim-x64",{"IOS","X64"}}
                };
                mobile_entry=false;
                if(auto target=hosted.find(v);target!=hosted.end()) {
                    if(command!="emit" && command!="check")throw std::runtime_error("--target "+v+" is available for emit and check");
                    requested_target.clear();
                    preprocessing.platform=target->second.platform;preprocessing.architecture=target->second.arch;
                    mobile_entry=v!="web";
                } else {
                    requested_target=bolt::native_backend::aot_target(v);
                    preprocessing.platform.clear();preprocessing.architecture.clear();
                    if(requested_target.ends_with("-linux-gnu")) {
                        preprocessing.platform="Linux";
                        preprocessing.architecture=requested_target.starts_with("aarch64-")?"ARM64":requested_target.starts_with("riscv64-")?"RISCV64":"X64";
                    } else if(requested_target.find("-apple-macosx")!=std::string::npos) {
                        preprocessing.platform="MacOS";
                        preprocessing.architecture=requested_target.starts_with("arm64-")?"ARM64":"X64";
                    }
                }
            }
            else if(option=="--define"){preprocessing.defines[v]=true;}
            else if(option=="--profile")selected_profile=v;
            else if(option=="--working-directory")work_dir_option=v;
            else if(option=="-o")output=v;
            else if(option=="--cxx")compiler=v;
            else if(option=="--backend"){backend=v;backend_set=true;}
            else if(option=="--llvm")llvm=v;
            else if(option=="--linker")linker=v;
            else if(option=="--runtime-dir")runtime_dir=v;
            else if(option=="--sysroot")sysroot=v;
            else if(option=="--include"){include=v;include_set=true;}
            else if(option=="--library")library=v;
            else if(option=="--header-dir")header_dirs.push_back(v);
            else if(option=="--link-lib")link_libraries.push_back(v);
            else if(option=="--steps") {
                steps_set=true;
                auto p=std::from_chars(v.data(),v.data()+v.size(),options.max_steps);
                if(p.ec!=std::errc {
                }
                ||p.ptr!=v.data()+v.size())throw std::runtime_error("--steps requires a nonnegative integer");
            } else throw std::runtime_error("unknown option '"+option+"'");
        }
        const auto target_triple=bolt::native_backend::aot_target(requested_target);
        const bool cross=target_triple!=bolt::native_backend::host_target();
#if defined(__APPLE__)
        if(cross&&runtime_dir.empty()) {
            const auto bundled=executable_path(args[0]).parent_path()/"targets"/(target_triple.starts_with("arm64-")?"arm64":"x86_64");
            if(fs::is_regular_file(bundled/"libBoltRuntime.dylib"))runtime_dir=bolt::path_to_utf8(bundled);
        }
#endif
        if(cross&&(command=="run"||command=="exec"||jit_run||runtime_run||no_compile))
            throw std::runtime_error(std::string(target_triple.find("-apple-macosx")!=std::string::npos?"foreign macOS target ":"foreign Linux target ")+target_triple+" supports build, compile, emit, emit-ir and check; run the executable on its target system");
        auto project_model=command=="exec"?std::optional<bolt::project::Graph>{}:bolt::project::resolve(bolt::path_from_utf8(args[2]));
        if(!selected_profile.empty()&&selected_profile!="Debug"&&selected_profile!="Release")throw std::runtime_error("--profile requires Debug or Release");
        if(!selected_profile.empty()&&((debug_set&&selected_profile!="Debug")||(release_set&&selected_profile!="Release")))throw std::runtime_error("conflicting profile flags");
        if(selected_profile.empty())selected_profile=debug_set?"Debug":release_set?"Release":project_model?project_model->main.default_profile:"Debug";
        release=selected_profile=="Release";
        preprocessing.configuration=selected_profile;
        optimize=project_model?project_model->main.profiles.at(selected_profile).optimization=="aggressive":release;
        if(!debug_info_set)debug_info=project_model?project_model->main.profiles.at(selected_profile).debug_info:!release;
        fs::path work_dir;
        if(project_model){
            args[2]=bolt::path_to_utf8(project_model->main.file);
            for(const auto& generated:{"bin","obj",".bolt"})if(fs::is_symlink(bolt::build_paths::root_for(project_model->main.root)/generated))throw std::runtime_error("project generated directories must not be symlinks");
            if(!project_model->main.incremental&&!no_compile)rebuild=true;
            if(command=="run"){
                if(project_model->main.kind!="executable")throw std::runtime_error("a library project cannot be run; use bolt compile");
                if(!args_set)application_arguments=project_model->main.arguments;
                work_dir=project_model->main.working_directory;
            }
        }
        if(!work_dir_option.empty()){
            if(command!="run")throw std::runtime_error("--working-directory only applies to run");
            work_dir=bolt::path_from_utf8(work_dir_option);
            if(work_dir.is_relative())work_dir=(project_model?project_model->main.root:fs::current_path())/work_dir;
            work_dir=fs::weakly_canonical(work_dir);if(!fs::is_directory(work_dir))throw std::runtime_error("working directory does not exist");
        }
        if(debug_set&&release_set)throw std::runtime_error("choose either --debug or --release");
        if(lto&&!release)throw std::runtime_error("--lto requires --release");
        if(no_entry&&command!="check")throw std::runtime_error("--no-entry is only supported by check");
        if(diagnostic_format==bolt::DiagnosticFormat::Json&&command!="check")throw std::runtime_error("JSON diagnostics are supported by bolt check; use text or msbuild for other commands");
        if(!application_arguments.empty()&&command!="run")throw std::runtime_error("Arguments after -- are supported by bolt run only.");
        if(native_set&&command!="run")throw std::runtime_error("--native only applies to run (it is already the default)");
        if(runtime_run&&command!="run")throw std::runtime_error("--runtime only applies to run; use bolt exec for interpreted statements");
        if(jit_run&&command!="run"&&command!="exec")throw std::runtime_error("--jit only applies to run or exec");
        if((native_set&&jit_run)||(runtime_run&&(native_set||jit_run)))
            throw std::runtime_error("choose one execution mode: native AOT (default), --jit, or --runtime");
        if(no_compile&&command!="run")throw std::runtime_error("--no-compile only applies to run");
        if(require_runtime_free&&(jit_run||runtime_run||object_only||(command!="compile"&&command!="build"&&command!="run")))
            throw std::runtime_error("--no-runtime applies to native executable and shared-library builds and to native runs.");
        if(object_only&&command!="compile"&&command!="build")throw std::runtime_error("--object-only only applies to compile");
        if(command=="exec"&&!jit_run&&(debug_set||release_set))
            throw std::runtime_error("interpreted exec has no Debug/Release configuration; use --jit for native code generation");
        if(runtime_run) {
            if(debug_set||release_set||!selected_profile.empty()&&std::find(args.begin(),args.end(),"--profile")!=args.end()||debug_info_set)throw std::runtime_error("--runtime interprets source and has no Debug/Release build configuration; remove --debug/--release");
            if(no_compile||object_only||backend_set||require_native||rebuild||lto||!pch||include_set||!output.empty()||
               !compiler.empty()||!library.empty()||!llvm.empty()||!linker.empty()||!runtime_dir.empty()||!sysroot.empty()||!header_dirs.empty()||!link_libraries.empty())
                throw std::runtime_error("--runtime cannot be combined with native compilation options or --no-compile");
        }
        if(no_compile && (!preprocessing.defines.empty()||jit_run||runtime_run||object_only||rebuild||steps_set||debug_info_set||lto||!pch||include_set||
           !compiler.empty()||!library.empty()||!llvm.empty()||!linker.empty()||!runtime_dir.empty()||!sysroot.empty()||!header_dirs.empty()||!link_libraries.empty()))
            throw std::runtime_error("--no-compile runs a previously built executable; it cannot use JIT/runtime, --rebuild, --steps, or code-generation/linker options");
        native_run=command=="run"&&!runtime_run&&!jit_run;
        if(steps_set&&!(runtime_run||jit_run||command=="exec"))
            throw std::runtime_error("--steps is an interpreter/JIT instruction budget; use --runtime or --jit, not an AOT build");
        if(jit_run&&(!output.empty()||object_only||rebuild||lto||!pch||include_set||!compiler.empty()||!library.empty()||!linker.empty()||!runtime_dir.empty()||!sysroot.empty()||!header_dirs.empty()||!link_libraries.empty()))
            throw std::runtime_error("--jit executes in memory and does not accept executable-output, linker, C++ or build-cache options");
        const auto cli_link_libraries=link_libraries;
        const auto cli_header_dirs=header_dirs;
        if(project_model&&!runtime_run&&!jit_run&&(command=="compile"||command=="build"||command=="run")){
            auto append_unique_string=[](std::vector<std::string>& values,const std::string& value){if(std::find(values.begin(),values.end(),value)==values.end())values.push_back(value);};
            auto append_unique_path=[](std::vector<fs::path>& values,const fs::path& value){if(std::find(values.begin(),values.end(),value)==values.end())values.push_back(value);};
            auto append_native=[&](const bolt::project::Model& model,bool link){
                for(const auto& directory:model.native_include_dirs)append_unique_string(header_dirs,bolt::path_to_utf8(directory));
                for(const auto& directory:model.native_runtime_dirs)append_unique_path(native_runtime_dirs,directory);
                if(link)for(const auto& native_library:model.native_libraries)append_unique_string(link_libraries,bolt::path_to_utf8(resolve_project_native_library(model,native_library)));
            };
            append_native(project_model->main,!no_compile);
            for(const auto& dependency:project_model->dependencies)append_native(dependency,!no_compile);
        }
        if(require_runtime_free && project_model && project_model->main.kind=="static-library")
            throw std::runtime_error("Build the consuming executable or shared library with --no-runtime to verify this static library");
        const bool runtime_free_shared=require_runtime_free && project_model &&
            (project_model->main.kind=="shared-library" || std::any_of(project_model->dependencies.begin(),project_model->dependencies.end(),
                [](const auto& dep){return dep.kind=="shared-library";}));
        const bool project_link=project_model && !require_runtime_free &&
            (project_model->main.kind!="executable" || !project_model->dependencies.empty());
        if((!compiler.empty()||lto||(project_link&&(!pch||include_set||!library.empty()))||(!require_native&&(!header_dirs.empty()||!link_libraries.empty())))&&!backend_set)backend="cpp"; // Preserve existing --cxx scripts and project-native dependencies.
        const std::string requested_backend=backend;
#if defined(__linux__) && defined(__GLIBC_PREREQ) && !defined(BOLT_LINUX_MUSL)
#if __GLIBC_PREREQ(2,34)
        preprocessing.defines["BoltSpawnCloseFrom"]=true;
#endif
#endif
        if(require_runtime_free&&!no_compile){
            preprocessing.defines["BoltStandaloneCore"]=true;
            if(backend!="auto"&&backend!="llvm")throw std::runtime_error("--no-runtime requires the LLVM backend.");
            backend="llvm";
        }
        const bool auto_backend=backend=="auto";
        backend=native_backend_name(backend,optimize,requested_target);
        if((project_model||debug_info_set)&&debug_info&&backend=="fast"&&!no_compile&&!runtime_run){
            // Linux Fast AOT writes DWARF source locations. JIT debugger
            // registration and other platform formats still use LLVM.
#if defined(__linux__)
            const bool needs_llvm=jit_run;
#else
            const bool needs_llvm=true;
#endif
            if(needs_llvm){
                if(backend_set&&requested_backend=="fast")throw std::runtime_error("Fast code generation has no native debug symbols for this route; select --backend llvm or --no-debug-info");
                backend="llvm";
            }
        }
        if(!compiler.empty()&&backend!="cpp")throw std::runtime_error("--cxx only applies to --backend cpp");
        if(jit_run&&backend=="cpp")throw std::runtime_error("the C++ compatibility backend does not provide JIT compilation");
        if(object_only&&backend=="cpp")throw std::runtime_error("--object-only requires a direct native backend");
        if(lto&&backend!="cpp")throw std::runtime_error("--lto is a C++-backend option; direct LLVM release already optimizes the combined Bolt module at O3");
        if(require_native&&backend=="cpp")throw std::runtime_error("--require-native forbids the C++ compatibility backend");
        if(backend=="cpp" || command=="emit")
            preprocessing.defines["BoltCppCompatibility"]=true;
        bolt::JitOptions jit;jit.backend=backend;jit.release=optimize;jit.llvm_library=llvm;jit.execution=options;jit.preprocessing=preprocessing;jit.arguments=application_arguments;
        if(command=="exec")return jit_run?bolt::jit_statements(args[2],jit):bolt::execute_statements(args[2],options,preprocessing);
        const auto input_path=fs::absolute(bolt::path_from_utf8(args[2]));
        const auto root=project_model?project_model->main.root:fs::is_directory(input_path)?fs::weakly_canonical(input_path):input_path.parent_path();
        const auto scratch=project_model?bolt::project::intermediate_directory(project_model->main,target_triple,selected_profile):fs::path{};
        const auto build_root=bolt::build_paths::root_for(root);
        bolt::cli_state::BuildIdentity identity(input_path,build_root,release);
        if(no_compile) {
            auto chosen=output.empty()?fs::path{}:fs::absolute(bolt::path_from_utf8(output));
#ifdef _WIN32
            if(!chosen.empty()&&chosen.extension().empty())chosen+=".exe";
#endif
            const auto target=identity.previous(chosen,requested_backend,require_native,target_triple,require_runtime_free);
            status_message("info",std::string("[bolt] Compilation skipped (--no-compile); running last ")+(release?"Release":"Debug")+" build: "+bolt::path_to_utf8(target)+" (source changes are ignored)");
            std::vector<std::string> invocation{bolt::path_to_utf8(target)};
            invocation.insert(invocation.end(),application_arguments.begin(),application_arguments.end());
            application_phase();bolt::project::WorkingDirectory cwd(work_dir);ScopedLibrarySearchPath native_paths(native_runtime_dirs);return process(invocation);
        }
        if(command=="check") {
            auto inspection=bolt::inspect_project(input_path,{},!no_entry,preprocessing);
            if(diagnostic_format==bolt::DiagnosticFormat::Json)std::cout<<bolt::format_diagnostics_json(inspection.diagnostics);
            else {
                for(const auto& d:inspection.diagnostics)std::cerr<<bolt::format_diagnostic(d,diagnostic_format);
                if(inspection.diagnostics.empty())std::cout<<"OK: "<<inspection.sources.size()<<" source file(s) checked\n";
            }
            return inspection.diagnostics.empty()?0:1;
        }
        std::unique_ptr<bolt::project::BuildLock> project_lock,output_lock;
        if(project_model&&!runtime_run&&!jit_run)project_lock=std::make_unique<bolt::project::BuildLock>(project_model->main,selected_profile);
        BuildTiming timing(show_timing);
        auto program=bolt::load_project(input_path,{},true,false,preprocessing);
        bolt::analyze_for_execution(program,!program.library_mode);


        if(runtime_run) {
            const auto requirement=bolt::interpreter_requirement(program);
            if(!requirement.empty())throw std::runtime_error("--runtime does not support "+requirement+"; use native 'bolt run .' instead. No native compilation was attempted.");
            bolt::Runtime runtime(std::move(program),options);
            timing.finish("Analysis/interpreter preparation (no native compilation)");
            application_phase();bolt::project::WorkingDirectory cwd(work_dir);return runtime.run(application_arguments);
        }
        // Select a viable source implementation before a hosted ABI requirement
        // can reject it or send the executable through the C++ backend.
        std::optional<bolt::ir::Module> standalone;
        if(!project_link && !require_runtime_free && !object_only && !jit_run && backend!="cpp" &&
           (auto_backend || backend=="llvm") && (command=="compile" || command=="build" || native_run) &&
           uses_standalone_library(program)){
            try{
                bolt::native_backend::Options probe;probe.llvm_library=llvm;
                (void)bolt::native_backend::llvm_version(probe);
                auto pp=preprocessing;pp.defines["BoltStandaloneCore"]=true;
                auto candidate=bolt::load_project(input_path,{},true,false,pp);
                bolt::analyze_for_execution(candidate,!candidate.library_mode);
                auto module=bolt::ir::lower(candidate,{.optimize_loops=optimize,.try_runtime_free_entry=true,.source_core=true});
                if(module.runtime_free_entry){
                    standalone=std::move(module);
                    program=std::move(candidate);
                    backend="llvm";
                }
            }catch(const std::exception& error){
                if(verbose)status_message("info",std::string("[bolt] Standalone probe: ")+error.what());
            }
        }
        const auto native_requirement=bolt::native_abi::direct_requirement(program);
        if(!native_requirement.empty()) {
            const auto& features=native_requirement;
            if(require_native||jit_run||object_only||command=="emit-ir"||(backend_set&&!auto_backend&&backend!="cpp"))
                throw std::runtime_error("This program uses "+features+". These features currently require the C++ native backend; use bolt compile/run --backend cpp, without --jit or --object-only.");
            if(backend!="cpp")status_message("warning","[bolt] C++ native backend selected for: "+features+".");
            backend="cpp";if(command=="run")native_run=true;
        }
        if(require_native&&backend=="cpp")throw std::runtime_error("--require-native forbids the C++ compatibility backend");
        const char* configured_llvm=std::getenv("BOLT_LLVM_LIBRARY");
        const bool explicit_llvm=!llvm.empty()||(configured_llvm&&*configured_llvm);
        if(auto_backend&&!explicit_llvm&&backend=="llvm"&&!jit_run&&!object_only&&command!="emit-ir"&&
           (command=="compile"||command=="build"||(command=="run"&&native_run))) {
            bolt::native_backend::Options probe;probe.llvm_library=llvm;
            try{(void)bolt::native_backend::llvm_version(probe);}
            catch(const std::exception& error){
                if(require_native)throw std::runtime_error(std::string("--require-native: ")+error.what());
                status_message("warning",std::string("[bolt] LLVM is unavailable (")+error.what()+"). Falling back to the installed C++20 toolchain.");
                backend="cpp";
            }
        }
        if(command=="run"&&jit_run){bolt::NativeProgram compiled(program,jit);timing.finish("JIT compilation");application_phase();bolt::project::WorkingDirectory cwd(work_dir);return compiled.run();}
        if(project_link&&!deps_ready&&!object_only&&(command=="compile"||command=="build"||native_run)){
            for(const auto& dep:project_model->dependencies){
                auto compiler_image=executable_path(args[0]);
#if defined(__linux__)
                // An atomic SDK upgrade can unlink this image while descendants
                // are being built. Re-execute the running compiler's inode.
                if(compiler_image.native().ends_with(" (deleted)") && !fs::exists(compiler_image))
                    compiler_image="/proc/self/exe";
#endif
                std::vector<std::string> call{bolt::path_to_utf8(compiler_image),"compile",bolt::path_to_utf8(dep.file),"--profile",selected_profile,"--project-deps-ready"};
                call.insert(call.end(),{"--backend",backend});
                if(require_native)call.push_back("--require-native");
                if(debug_info_set||backend=="fast")call.push_back(debug_info?"--debug-info":"--no-debug-info");
                if(!llvm.empty())call.insert(call.end(),{"--llvm",llvm});
                if(!linker.empty())call.insert(call.end(),{"--linker",linker});
                for(const auto& file:cli_link_libraries)call.insert(call.end(),{"--link-lib",file});
                for(const auto& dir:cli_header_dirs)call.insert(call.end(),{"--header-dir",dir});
                if(cross)call.insert(call.end(),{"--target",target_triple});
                for(const auto& [flag,value]:std::vector<std::pair<std::string,std::string>>{{"--runtime-dir",runtime_dir},{"--sysroot",sysroot},{"--library",library},{"--include",include_set?include:""}})
                    if(!value.empty())call.insert(call.end(),{flag,value});
                if(!compiler.empty()){call.push_back("--cxx");call.push_back(compiler);}if(rebuild)call.push_back("--rebuild");if(!pch)call.push_back("--no-pch");if(verbose)call.push_back("--verbose");
                for(const auto& [name,value]:preprocessing.defines){(void)value;call.push_back("--define");call.push_back(name);}
                const auto status=process(call,"Dependency build");if(status!=0)return status;
            }
        }
        if(project_link)for(auto it=project_model->dependencies.rbegin();it!=project_model->dependencies.rend();++it){
            auto file=bolt::project::artifact(*it,target_triple,selected_profile);
#ifdef _WIN32
            if(it->kind=="shared-library")file.replace_extension(".lib");
#endif
            link_libraries.push_back(bolt::path_to_utf8(file));
        }
        const bool default_output=output.empty();
        if(default_output) {
            if(command=="emit"||command=="emit-ir")throw std::runtime_error(command+" requires -o OUTPUT");
            auto name=fs::is_directory(input_path)?root.filename():input_path.stem();
            if(name.empty())name="Program";
            if(object_only) {
#ifdef _WIN32
                name+=".obj";
#else
                name+=".o";
#endif
            }
            auto bin=build_root/"bin";
            if(cross)bin/=target_triple;
            auto artifact=project_model?bolt::project::artifact(project_model->main,target_triple,selected_profile):bin/(release?"Release":"Debug")/name;
            if(project_model&&object_only)artifact.replace_extension(name.extension());
            output=bolt::path_to_utf8(artifact);
        }
        auto target=fs::absolute(bolt::path_from_utf8(output));
#ifdef _WIN32
        if(command!="emit"&&command!="emit-ir"&&!object_only&&!program.library_mode&&!project_model&&(default_output||target.extension().empty()))target+=".exe";
#endif
        protect_sources(program,target);
        if(project_model){
            for(auto ancestor=target;ancestor!=ancestor.root_path();ancestor=ancestor.parent_path())
                if(fs::is_symlink(ancestor))throw std::runtime_error("generated output path must not traverse a symlink");
        }
        if(project_model||((command=="compile"||command=="run")&&backend=="cpp")) {
            bolt::project::Model lock_model;
            lock_model.root=root;lock_model.parallel=true;
            output_lock=std::make_unique<bolt::project::BuildLock>(lock_model,"output-"+hex_hash(hash_bytes(14695981039346656037ull,bolt::path_to_utf8(target))));
        }
        if(command=="emit-ir"){write_file(target,bolt::ir::llvm_text(bolt::ir::lower(program, {.optimize_loops = release}),target_triple,!release,release));std::cout<<"Generated native IR: "<<bolt::path_to_utf8(target)<<'\n';timing.finish("IR generation");return 0;}
        if(command!="emit"&&backend!="cpp"){
            if(!library.empty()||include_set||!pch)throw std::runtime_error("--library, --include and --no-pch are C++-backend options; use --backend cpp");
            NativeBuild direct;direct.backend=backend;direct.llvm=llvm;direct.linker=linker;direct.runtime_dir=runtime_dir;direct.sysroot=sysroot;direct.target_triple=target_triple;direct.release=release;direct.rebuild=rebuild;direct.verbose=verbose;direct.run=native_run;direct.object_only=object_only;direct.timing=&timing;direct.arguments=application_arguments;
            direct.work_dir=work_dir;direct.intermediate_dir=scratch;direct.optimize=optimize;direct.debug_info=debug_info;
            if(project_link)direct.project_kind=project_model->main.kind;
            direct.link_libraries=link_libraries;
            direct.require_runtime_free=require_runtime_free || standalone.has_value();
            std::optional<bolt::project::LibraryDeployments> deployment;
            direct.remember_success=[&](const std::string& actual,const fs::path& runtime,const std::string& target_digest,const std::string& runtime_digest,bool runtime_free){
                std::vector<fs::path> deployed;
                if(project_model)deployed=bolt::project::publish_project_artifacts(*project_model,selected_profile,target_triple,target,scratch,true,deployment?&*deployment:nullptr);
                if(!cross&&!program.library_mode)identity.remember(target,actual,target_triple,runtime,target_digest,runtime_digest,runtime_free,runtime_free?deployed:std::vector<fs::path>{});
            };
            direct.before_run=[&](){output_lock.reset();project_lock.reset();};
            if(runtime_free_shared){
                // Prove the entire graph before publishing any image. Stage specialized
                // libraries under the consumer so two applications can build independently.
                standalone=bolt::ir::lower(program,{.optimize_loops=optimize,.try_runtime_free_entry=true,.source_core=true});
                if(!standalone->runtime_free())throw std::runtime_error("--no-runtime: Bolt runtime required ("+standalone->runtime_requirement+").");
                auto images=bolt::project::freestanding::split(*standalone,*project_model);
                const auto image_dir=scratch/"runtime-free"/bolt::cli_state::hash_text(bolt::path_to_utf8(target));
                std::vector<std::string> imports;
                for(auto& image:images){
                    const bool main=image.project==&project_model->main;
                    const auto name=bolt::project::artifact(*image.project,target_triple,selected_profile).filename();
                    const auto file=main?target:image_dir/name;
                    if(!main && (target.parent_path()/name==target || file==target))
                        throw std::runtime_error("executable output collides with shared dependency "+bolt::path_to_utf8(name));
                    auto lib=direct;lib.project_kind="shared-library";lib.run=false;
                    for(auto it=image.dependencies.rbegin();it!=image.dependencies.rend();++it)lib.link_libraries.push_back(imports.at(*it));
                    if(main){
                        deployment=bolt::project::plan_libraries(*project_model,selected_profile,target_triple,target,true,image_dir);
                    }else{
                        lib.intermediate_dir=image_dir;
                        lib.timing=nullptr;
                        lib.remember_success=[&](const auto&,const auto&,const auto&,const auto&,bool){
                            bolt::project::record_artifacts(project_model->main,selected_profile,file,image_dir);
                        };
                    }
                    const int status=build_direct(program,build_root,file,executable_path(args[0]),lib,std::move(image.module));
                    if(status || main)return status;
                    auto link_file=file;
#ifdef _WIN32
                    link_file.replace_extension(".lib");
#endif
                    imports.push_back(bolt::path_to_utf8(link_file));
                }
                deployment=bolt::project::plan_libraries(*project_model,selected_profile,target_triple,target,true,image_dir);
                direct.link_libraries.insert(direct.link_libraries.end(),imports.rbegin(),imports.rend());
            }
            return build_direct(program,build_root,target,executable_path(args[0]),direct,std::move(standalone));
        }
        // Automatic fallback may select the compatibility emitter after the
        // direct program was analyzed. Its standard-library ABI is different.
        if(!preprocessing.defines.contains("BoltCppCompatibility") && !mobile_entry){
            preprocessing.defines["BoltCppCompatibility"]=true;
            program=bolt::load_project(input_path,{},true,false,preprocessing);
            bolt::analyze_for_execution(program,!program.library_mode);
        }
        auto generated_code=mobile_entry?bolt::emit_mobile_cpp(program):bolt::emit_cpp(program);
        if(cross) {
            std::string architecture="defined(__x86_64__)";
            if(target_triple.starts_with("aarch64-")||target_triple.starts_with("arm64-"))architecture="defined(__aarch64__)";
            else if(target_triple.starts_with("riscv64-"))architecture="(defined(__riscv) && __riscv_xlen == 64)";
            const std::string os=target_triple.find("-apple-macosx")!=std::string::npos?"__APPLE__":"__linux__";
            generated_code="#if !defined("+os+") || !"+architecture+"\n#error This Bolt translation unit requires "+target_triple+"\n#endif\n"+generated_code;
        }
        if(command=="emit") {
            write_file(target,generated_code);
            std::cout<<"Generated "<<bolt::path_to_utf8(target)<<'\n';
            timing.finish("C++ source generation");return 0;
        }
#if defined(_WIN32) && defined(__MINGW32__)
        // The runtime archive and generated C++ share a C++ standard-library ABI.
        // LLVM-MinGW uses libc++; a stock MSVC compiler cannot link this archive.
        if (compiler.empty()) {
#if defined(__aarch64__)
            const char* driver = "aarch64-w64-mingw32-clang++.exe";
#elif defined(__x86_64__)
            const char* driver = "x86_64-w64-mingw32-clang++.exe";
#else
            const char* driver = "clang++.exe";
#endif
            const auto found = find_command({driver});
            if (found.empty()) throw std::runtime_error(std::string("C++ compatibility builds with this LLVM-MinGW SDK need ") +
                driver + " on PATH or --compiler PATH to a compatible LLVM-MinGW clang++.");
            compiler = bolt::path_to_utf8(found);
        }
#elif defined(_WIN32)
        if(compiler.empty())compiler=bolt::path_to_utf8(ensure_msvc_toolchain());
        else {
            auto requested=bolt::path_from_utf8(compiler).filename().string();
            std::transform(requested.begin(),requested.end(),requested.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            if((requested=="cl"||requested=="cl.exe")&&!fs::is_regular_file(bolt::path_from_utf8(compiler))&&find_command({compiler}).empty())
                compiler=bolt::path_to_utf8(ensure_msvc_toolchain());
        }
#elif defined(BOLT_LINUX_MUSL) && defined(_LIBCPP_VERSION)
        if(compiler.empty()){
            const auto found=find_command({"clang++","clang++-19","clang++-20","clang++-21","clang++-22"});
            if(found.empty())throw std::runtime_error("The musl SDK uses libc++. Install clang and libc++ development headers for the C++ backend, or select --cxx.");
            compiler=bolt::path_to_utf8(found);
        }
#elif defined(__APPLE__)
        if(compiler.empty())compiler=cross?"clang++":"c++";
#else
        if(compiler.empty())compiler=cross?bolt::linux_target::gnu_prefix(target_triple)+"g++":"c++";
#endif
        auto driver=bolt::path_to_utf8(executable_path(compiler, false).filename());
        std::transform(driver.begin(),driver.end(),driver.begin(),[](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        auto invoked_driver=bolt::path_to_utf8(bolt::path_from_utf8(compiler).filename());
        std::transform(invoked_driver.begin(),invoked_driver.end(),invoked_driver.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        // clang-cl can be a symlink to clang; argv[0] still selects CL semantics.
        if(invoked_driver=="cl"||invoked_driver=="cl.exe"||invoked_driver=="clang-cl"||invoked_driver=="clang-cl.exe")driver=invoked_driver;
        const bool msvc=driver=="cl"||driver=="cl.exe"||driver=="clang-cl"||driver=="clang-cl.exe";
        const bool clang=driver.find("clang")!=std::string::npos;
        if(cross) {
            if(library.empty()&&runtime_dir.empty())throw std::runtime_error("cross-target C++ builds require --runtime-dir TARGET_SDK/bin or --library PATH_TO_TARGET_LIBBOLTCORE");
            if(library.empty())library=bolt::path_to_utf8(bolt::path_from_utf8(runtime_dir)/"libBoltCore.a");
        }
        if(library.empty()) {
#ifdef _MSC_VER
            library=bolt::path_to_utf8(executable_path(args[0]).parent_path()/"BoltCore.lib");
#else
            library=bolt::path_to_utf8(executable_path(args[0]).parent_path()/"libBoltCore.a");
#endif
        }
        auto archive=fs::absolute(bolt::path_from_utf8(library));
        if(!fs::is_regular_file(archive))throw std::runtime_error("BoltCore archive not found: "+library+"; build the whole solution or use --library");
        auto include_path=fs::absolute(bolt::path_from_utf8(include));
        auto adjacent=executable_path(args[0]).parent_path()/"include";
        if(cross&&!runtime_dir.empty()) {
            const auto target_headers=bolt::path_from_utf8(runtime_dir)/"include";
            if(fs::is_directory(target_headers))adjacent=target_headers;
        }
        if(!include_set&&fs::is_regular_file(adjacent/"bolt"/"Std.hpp"))include_path=adjacent;
        if(!fs::is_regular_file(include_path/"bolt"/"Std.hpp"))throw std::runtime_error("Bolt headers not found; use --include PATH");
        std::vector<std::string> flags;
        if(msvc) {
            flags= {
                "/nologo","/std:c++20","/EHsc","/utf-8","/permissive-","/bigobj"
            };
#if defined(_MSC_VER) && defined(_DEBUG)
            flags.push_back("/MDd");
#else
            flags.push_back("/MD");
#endif
            flags.push_back(optimize?"/O2":"/Od");
            if(debug_info)flags.push_back("/Zi");
            if(release)flags.push_back("/DNDEBUG");
            if(lto)flags.push_back("/GL");
            flags.push_back("/I"+bolt::path_to_utf8(include_path));
        }else {
            flags= {
                "-std=c++20",optimize?"-O3":"-O0"
            };
#if defined(__APPLE__)
            if(cross)flags.insert(flags.end(),{"-arch",target_triple.starts_with("arm64-")?"arm64":"x86_64"});
#else
            if(cross&&clang)flags.push_back("--target="+target_triple);
#endif
#if defined(BOLT_LINUX_MUSL) && defined(_LIBCPP_VERSION)
            if(clang)flags.push_back("-stdlib=libc++");
#endif
            if(cross&&!sysroot.empty())flags.push_back("--sysroot="+bolt::path_to_utf8(fs::absolute(bolt::path_from_utf8(sysroot))));
            if(debug_info)flags.push_back("-g");
            if(release)flags.push_back("-DNDEBUG");
            if(lto)flags.push_back("-flto");
            flags.insert(flags.end(), {
                "-I",bolt::path_to_utf8(include_path)
            });
        }
        // Native bound headers are resolved relative to the project or a
        // repeatable --header-dir. Link arguments are file paths, never shell text.
        if(!program.cpp_headers.empty())header_dirs.insert(header_dirs.begin(),bolt::path_to_utf8(root));
        for(const auto& directory:header_dirs){auto path=fs::absolute(bolt::path_from_utf8(directory));if(!fs::is_directory(path))throw std::runtime_error("Native include directory not found: "+directory);if(msvc)flags.push_back("/I"+bolt::path_to_utf8(path));else{flags.push_back("-I");flags.push_back(bolt::path_to_utf8(path));}}
        if(!msvc){flags.push_back("-pthread");if(program.library_mode)flags.push_back("-fPIC");}
        if(project_model&&project_model->main.kind=="shared-library")flags.push_back(msvc?"/DBOLT_SHARED_LIBRARY":"-DBOLT_SHARED_LIBRARY");
#if defined(__APPLE__)
        if(!msvc)flags.push_back("-mmacosx-version-min="+bolt::native_backend::macos_deployment_target());
#endif
#if defined(_WIN32)
        if(!msvc&&program.entry_accepts_arguments)flags.push_back("-municode");
#endif
        // The header cache avoids reparsing the standard library on repeated native builds.
        // This legacy cache is used only by --backend cpp, never the direct backends.
        auto tool_hash=hash_bytes(14695981039346656037ull,"Bolt-" BOLT_VERSION_STRING "-cache-cpp-preprocessed-v4"+compiler+bolt::path_to_utf8(include_path));
        for(const auto& f:flags)tool_hash=hash_bytes(tool_hash,f+"\n");
        std::error_code ec;
        const auto compiler_path=executable_path(compiler, false);
        if(fs::exists(compiler_path,ec)) {
            tool_hash=hash_bytes(tool_hash,bolt::path_to_utf8(compiler_path));
            tool_hash=hash_file(tool_hash,compiler_path);
            // libc++ may use a wider clock representation; select the portable
            // long-long overload explicitly (AppleClang cannot infer one here).
            tool_hash=hash_bytes(tool_hash,std::to_string(static_cast<long long>(
                fs::last_write_time(compiler_path).time_since_epoch().count())));
        }
        tool_hash=hash_bytes(tool_hash,program.cpp_features.empty()?"core":"extended-library-v1");
        const auto program_digest=bolt::cli_state::bytes_digest(generated_code);
        auto base_fingerprint=hash_bytes(tool_hash,program_digest);
        base_fingerprint=hash_bytes(base_fingerprint,program.project_identity+bolt::preprocessor_identity(program.preprocessor));
        if(debug_info)base_fingerprint=debug_source_fingerprint(base_fingerprint,program);
        for(const auto& dep:link_libraries)base_fingerprint=hash_file(base_fingerprint,bolt::path_from_utf8(dep));
        base_fingerprint=hash_file(base_fingerprint,archive);
        auto generated=scratch.empty()?target:scratch/target.filename();
        generated+=".bolt.cpp";
        protect_sources(program,generated);
        write_if_changed(generated,generated_code);
        const auto cache=build_root/".bolt";
        const auto key=hex_hash(hash_bytes(14695981039346656037ull,bolt::path_to_utf8(target)));
        const auto cache_record=cache/"build"/(key+".txt");
        protect_sources(program,cache_record);
        std::string saved,checksum,saved_mode,saved_program;
        if(fs::is_regular_file(cache_record)) {
            std::ifstream in(cache_record);
            in>>saved>>checksum>>saved_mode>>saved_program;
        }
        bolt::cpp_inputs::Workspace probe_files(cache/"build");
        auto run_application=[&](){probe_files.clear();application_phase();output_lock.reset();project_lock.reset();bolt::project::WorkingDirectory cwd(work_dir);ScopedLibrarySearchPath native_paths(native_runtime_dirs);std::vector<std::string> invocation{bolt::path_to_utf8(target)};invocation.insert(invocation.end(),application_arguments.begin(),application_arguments.end());return process(invocation);};
        const auto expanded=probe_files.file("program.ii");
        using CppDriver=bolt::cpp_inputs::Driver;
        using CppMode=bolt::cpp_inputs::Mode;
        const auto input_driver=!msvc?CppDriver::gnu:clang?CppDriver::clang_cl:CppDriver::msvc;
        const bool gnu_name=!msvc&&!clang&&(driver.find("g++")!=std::string::npos||driver.find("gcc")!=std::string::npos);
        const bool previous_full=saved_mode=="full"&&saved_program==program_digest;
        auto input_mode=gnu_name&&!previous_full?CppMode::directives:CppMode::expanded;
        const auto scan=[&]{return bolt::cpp_inputs::signature(compiler,flags,generated,expanded,input_driver,debug_info,input_mode);};
        auto cpp_inputs=scan();
        if(input_mode==CppMode::directives&&!cpp_inputs) {
            input_mode=CppMode::expanded;
            cpp_inputs=scan();
        }
        const auto cache_fingerprint=[&] {
            const auto mode=input_mode==CppMode::directives?"directives":"full";
            return hex_hash(hash_bytes(hash_bytes(base_fingerprint,mode),*cpp_inputs));
        };
        if(cpp_inputs&&!rebuild&&fs::is_regular_file(target)) {
            if(saved==cache_fingerprint()&&checksum==bolt::cli_state::file_hash(target)) {
                std::cout<<"Up to date: "<<bolt::path_to_utf8(target)<<'\n';
                std::cout.flush();
                if(!cross&&!program.library_mode)identity.remember(target,"cpp",target_triple,{},checksum);
                if(project_model){bolt::project::publish_project_artifacts(*project_model,selected_profile,target_triple,target,scratch);}
                timing.finish(release?"Release compilation":"Debug compilation",true);
                return native_run?run_application():0;
            }
        }
        if(cpp_inputs&&input_mode==CppMode::directives&&!bolt::cpp_inputs::deterministic_directives(compiler,flags,expanded,probe_files)) {
            // Context-sensitive macros (including __TIMESTAMP__) are expanded by
            // the real compiler. Reproducible values can still produce cache hits.
            input_mode=CppMode::expanded;
            cpp_inputs=scan();
        }
        if(!cpp_inputs&&verbose)status_message("info","[bolt] C++ input scan unavailable; rebuilding without a cache receipt.");
        std::vector<std::string> use_pch;
        const std::string preamble=std::string("#include <bolt/Std.hpp>\n#include <bolt/API.hpp>\n#include <bolt/native/Jit.hpp>\n#include <bolt/Path.hpp>\n")+(program.cpp_features.empty()?"":"#include <bolt/library/Library.hpp>\n");
        if(pch&&!msvc&&cpp_inputs) {
            bolt::project::Model lock_model;lock_model.root=root;lock_model.parallel=true;
            bolt::project::BuildLock pch_lock(lock_model,"pch-"+hex_hash(tool_hash));
            const auto probe=cache/"pch"/hex_hash(tool_hash)/"preamble.hpp";
            protect_sources(program,probe);
            write_if_changed(probe,preamble);
            const auto pch_inputs=bolt::cpp_inputs::signature(compiler,flags,probe,probe_files.file("preamble.ii"),CppDriver::gnu,true,CppMode::preamble);
            if(pch_inputs) {
                const auto folder=cache/"pch"/hex_hash(hash_bytes(tool_hash,*pch_inputs));
                const auto header=folder/"bolt_pch.hpp";
                const auto compiled=folder/(clang?"bolt_pch.pch":"bolt_pch.hpp.gch");
                const auto receipt=folder/"pch.digest";
                protect_sources(program,header);
                protect_sources(program,compiled);
                protect_sources(program,receipt);
                const bool header_changed=write_if_changed(header,preamble);
                std::string stored;
                if(std::ifstream previous(receipt);previous)previous>>stored;
                bool ready=!rebuild&&!header_changed&&!stored.empty()&&fs::is_regular_file(compiled)&&stored==bolt::cli_state::file_hash(compiled);
                if(!ready) {
                    const auto staging=probe_files.file("building.pch");
                    std::vector<std::string> invocation{compiler};
                    invocation.insert(invocation.end(),flags.begin(),flags.end());
                    invocation.insert(invocation.end(),{"-x","c++-header",bolt::path_to_utf8(header),"-o",bolt::path_to_utf8(staging)});
                    verbose_command(invocation,verbose);
                    ready=process(invocation,"C++ precompiled header compiler")==0;
                    if(ready) {
                        // A header generator may finish while the PCH compiler is running.
                        // Only publish this result under the original preamble's key if
                        // all of its actual inputs and their exported macros still match.
                        ready=pch_inputs==bolt::cpp_inputs::signature(compiler,flags,probe,probe_files.file("verified-preamble.ii"),CppDriver::gnu,true,CppMode::preamble);
                        if(ready) {
                            const auto digest=bolt::cli_state::file_hash(staging);
                            bolt::cli_state::replace_file(staging,compiled);
                            write_file(receipt,digest+"\n");
                        } else if(verbose)status_message("info","[bolt] Precompiled header inputs changed during compilation; continuing without it.");
                    } else status_message("warning","Bolt: precompiled header failed; retrying without it.");
                }
                if(ready)use_pch=clang?std::vector<std::string>{"-include-pch",bolt::path_to_utf8(compiled)}
                                      :std::vector<std::string>{"-include",bolt::path_to_utf8(header)};
            }
        }
        // For the non-PCH GNU-style build, compile the exact preprocessed input
        // whose digest was validated. Edits made during codegen thus cannot be
        // associated with a receipt for a different header snapshot.
        const bool compile_snapshot=!msvc&&(gnu_name||clang)&&use_pch.empty()&&cpp_inputs.has_value();
        const auto& compile_input=compile_snapshot?expanded:generated;
        const auto remember_cpp=[&](const std::string& checksum){
            // PCH/MSVC builds consume the original source. Do not publish a
            // cache entry if inputs changed while that compiler was running.
            const bool unchanged=cpp_inputs&&(compile_snapshot||cpp_inputs==bolt::cpp_inputs::signature(
                compiler,flags,generated,probe_files.file("verified.ii"),input_driver,debug_info,input_mode));
            if(unchanged) {
                const auto mode=input_mode==CppMode::directives?"directives":"full";
                write_file(cache_record,cache_fingerprint()+" "+checksum+" "+mode+" "+program_digest+"\n");
            }
            else fs::remove(cache_record);
        };
        if(compile_snapshot&&input_mode==CppMode::directives)flags.push_back("-fdirectives-only");
        if(program.library_mode) {
            const bool shared=project_model->main.kind=="shared-library";
            auto object=scratch/target.filename();object+=".obj";
            auto temporary=target;temporary+=".building";
            // The linker embeds a DLL's output basename into its import library.
            // Keep that basename while staging, then publish the completed image.
            if(shared)temporary=scratch/"link"/target.filename();
            protect_sources(program,temporary);
            for(auto ancestor=temporary;ancestor!=ancestor.root_path();ancestor=ancestor.parent_path())
                if(fs::is_symlink(ancestor))throw std::runtime_error("library staging path must not traverse a symlink");
            fs::create_directories(target.parent_path());fs::create_directories(scratch);fs::create_directories(temporary.parent_path());
            fs::remove(temporary);
            std::vector<std::string> build{compiler};build.insert(build.end(),flags.begin(),flags.end());
            build.insert(build.end(),use_pch.begin(),use_pch.end());
            if(shared) {
                if(msvc)build.insert(build.end(),{"/LD",bolt::path_to_utf8(compile_input),bolt::path_to_utf8(archive),"/Fe"+bolt::path_to_utf8(temporary),"/Fo"+bolt::path_to_utf8(object)});
                else build.insert(build.end(),{
#ifdef __APPLE__
                    "-dynamiclib",
#else
                    "-shared",
#endif
                    bolt::path_to_utf8(compile_input),bolt::path_to_utf8(archive),"-o",bolt::path_to_utf8(temporary)});
                for(const auto& d:link_libraries){build.push_back(d);auto path=bolt::path_from_utf8(d);if(!msvc&&(path.extension()==".so"||path.extension()==".dylib"))build.push_back(cross?(target_triple.find("-apple-macosx")!=std::string::npos?"-Wl,-rpath,@loader_path":"-Wl,-rpath,$ORIGIN"):"-Wl,-rpath,"+bolt::path_to_utf8(path.parent_path()));}
#if defined(__linux__) || defined(__FreeBSD__)
#ifdef __linux__
                build.push_back("-ldl");
#endif
                // Link consumers by the library's installed basename, including
                // when a cross build passes its absolute staging path to ld.
                build.push_back("-Xlinker");
                build.push_back("-soname");
                build.push_back("-Xlinker");
                build.push_back(bolt::path_to_utf8(target.filename()));
#endif
#ifdef __APPLE__
                build.push_back("-Wl,-install_name,@rpath/"+bolt::path_to_utf8(target.filename()));
#endif
                if(!msvc&&!link_libraries.empty())build.push_back(bolt::path_to_utf8(archive));
#if defined(__FreeBSD__)
                build.push_back("-lexecinfo");
#endif
#if defined(_WIN32)
                build.push_back(msvc?"ws2_32.lib":"-lws2_32");
                build.push_back(msvc?"shell32.lib":"-lshell32");
                build.push_back(msvc?"bcrypt.lib":"-lbcrypt");
#endif
                if(msvc){build.push_back("/link");auto import=target;import.replace_extension(".lib");build.push_back("/IMPLIB:"+bolt::path_to_utf8(import));}
                verbose_command(build,verbose);int status=process(build,"C++ compiler/linker");if(status)return status;
            } else {
                if(msvc)build.insert(build.end(),{"/c",bolt::path_to_utf8(compile_input),"/Fo"+bolt::path_to_utf8(object)});
                else build.insert(build.end(),{"-c",bolt::path_to_utf8(compile_input),"-o",bolt::path_to_utf8(object)});
                verbose_command(build,verbose);int status=process(build,"C++ compiler");if(status)return status;
                std::vector<std::string> names=msvc?std::vector<std::string>{"lib.exe"}:std::vector<std::string>{"ar","llvm-ar"};
#if defined(__linux__)
                if(cross)names={bolt::linux_target::gnu_prefix(target_triple)+"ar","llvm-ar"};
#endif
                auto archiver=find_command(names);
                if(archiver.empty())throw std::runtime_error("native library archiver not found");
                fs::remove(temporary,ec);
                std::vector<std::string> pack{bolt::path_to_utf8(archiver)};
                if(msvc)pack.insert(pack.end(),{"/NOLOGO","/OUT:"+bolt::path_to_utf8(temporary),bolt::path_to_utf8(object)});
                else pack.insert(pack.end(),{"rcs",bolt::path_to_utf8(temporary),bolt::path_to_utf8(object)});
                verbose_command(pack,verbose);status=process(pack,"Native archiver");if(status)return status;
            }
            remember_cpp(bolt::cli_state::file_hash(temporary));
            bolt::cli_state::replace_file(temporary,target);
            bolt::project::publish_project_artifacts(*project_model,selected_profile,target_triple,target,scratch);
            std::cout<<"Built "<<selected_profile<<" "<<project_model->main.kind<<": "<<bolt::path_to_utf8(target)<<'\n';
            timing.finish(release?"Release library compilation":"Debug library compilation");return 0;
        }
        auto temporary=target;
        temporary+=".linking";
#ifdef _WIN32
        temporary+=".exe";
#endif
        protect_sources(program,temporary);
        fs::remove(temporary);
        fs::create_directories(target.parent_path());
        std::vector<std::string> invocation {
            compiler
        };
        invocation.insert(invocation.end(),flags.begin(),flags.end());
        invocation.insert(invocation.end(),use_pch.begin(),use_pch.end());
        if(msvc) {
            auto object=target;
            object+=".obj";
            auto pdb=target;
            pdb+=".compiler.pdb";
            protect_sources(program,object);
            protect_sources(program,pdb);
            invocation.insert(invocation.end(), {
                bolt::path_to_utf8(compile_input),bolt::path_to_utf8(archive),"/Fe"+bolt::path_to_utf8(temporary),"/Fo"+bolt::path_to_utf8(object),"/Fd"+bolt::path_to_utf8(pdb)
            });
            {
                invocation.push_back("/link");
                invocation.push_back("/STACK:8388608");
                if(lto)invocation.push_back("/LTCG");
                if(debug_info){
                    auto debug_symbols=target;
                    debug_symbols+=".pdb";
                    protect_sources(program,debug_symbols);
                    invocation.push_back("/DEBUG");
                    invocation.push_back("/PDB:"+bolt::path_to_utf8(debug_symbols));
                }
            }
        }else invocation.insert(invocation.end(), {
            bolt::path_to_utf8(compile_input),bolt::path_to_utf8(archive),"-o",bolt::path_to_utf8(temporary)
        });
#if defined(__linux__)
        if(!msvc)invocation.push_back("-ldl");
#endif
        for(const auto& lib:link_libraries){auto path=fs::absolute(bolt::path_from_utf8(lib));if(!fs::is_regular_file(path))throw std::runtime_error("Native library not found: "+lib);invocation.push_back(bolt::path_to_utf8(path));if(!msvc&&(path.extension()==".so"||path.extension()==".dylib"))invocation.push_back(cross?(target_triple.find("-apple-macosx")!=std::string::npos?"-Wl,-rpath,@loader_path":"-Wl,-rpath,$ORIGIN"):"-Wl,-rpath,"+bolt::path_to_utf8(path.parent_path()));}
        if(!msvc&&!link_libraries.empty())invocation.push_back(bolt::path_to_utf8(archive));
#if defined(__FreeBSD__)
                invocation.push_back("-lexecinfo");
#endif
#if defined(_WIN32)
        invocation.push_back(msvc?"ws2_32.lib":"-lws2_32");
        invocation.push_back(msvc?"shell32.lib":"-lshell32");
        invocation.push_back(msvc?"bcrypt.lib":"-lbcrypt");
#endif
        verbose_command(invocation,verbose);
        const auto begin=std::chrono::steady_clock::now();
        const int result=process(invocation,"C++ compiler/linker");
        if(result!=0) {
            fs::remove(temporary,ec);
            return result;
        }
#if defined(__APPLE__)
        if(cross)bolt::apple_target::require_compatible(temporary,bolt::native_backend::macos_deployment_target(),target_triple.starts_with("arm64-"));
#else
        if(cross)bolt::linux_target::require_elf(temporary,target_triple);
#endif
        // Preserve an existing executable if compilation/linking fails.
        const auto executable_digest=bolt::cli_state::file_hash(temporary);
        remember_cpp(executable_digest);
        bolt::cli_state::replace_file(temporary,target);
        if(verbose){std::ostringstream report;report<<"[bolt] native compiler/linker: "<<std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count()<<" s";status_message("info",report.str());}
        if(!cross&&!program.library_mode)identity.remember(target,"cpp",target_triple,{},executable_digest);
                if(project_model){bolt::project::publish_project_artifacts(*project_model,selected_profile,target_triple,target,scratch);}
        std::cout<<"Built "<<(release?"Release":"Debug")<<": "<<bolt::path_to_utf8(target)<<'\n';
        std::cout.flush();
        timing.finish(release?"Release compilation":"Debug compilation");
        return native_run?run_application():0;
    }
}
// namespace
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(utf8(argv[i]));
        return cli(args);
    } catch (const bolt::Error& error) {
        if(diagnostic_format==bolt::DiagnosticFormat::Json)std::cout<<bolt::format_diagnostics_json({error.diagnostic});
        else std::cerr<<bolt::format_diagnostic(error.diagnostic,diagnostic_format);
        return 1;
    } catch (const std::exception& error) {
        const auto d=bolt::tooling_diagnostic(error.what());
        if(diagnostic_format==bolt::DiagnosticFormat::Json)std::cout<<bolt::format_diagnostics_json({d});
        else std::cerr<<bolt::format_diagnostic(d,diagnostic_format);
        return 1;
    }
}
#else
int main(int argc, char** argv) {
    try {
        return cli(std::vector<std::string>(argv, argv + argc));
    } catch (const bolt::Error& error) {
        if(diagnostic_format==bolt::DiagnosticFormat::Json)std::cout<<bolt::format_diagnostics_json({error.diagnostic});
        else std::cerr<<bolt::format_diagnostic(error.diagnostic,diagnostic_format);
        return 1;
    } catch (const std::exception& error) {
        const auto d=bolt::tooling_diagnostic(error.what());
        if(diagnostic_format==bolt::DiagnosticFormat::Json)std::cout<<bolt::format_diagnostics_json({d});
        else std::cerr<<bolt::format_diagnostic(d,diagnostic_format);
        return 1;
    }
}
#endif
