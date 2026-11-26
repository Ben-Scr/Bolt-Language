// Headless editor loop. Send "reload", "frame" or "quit" on stdin.
#include <bolt/CompilerHost.h>
#include "Funke/NativeEngineAPI.hpp"
#ifndef BOLT_EDITOR_BOLT_ONLY
#include <coreclr_delegates.h>
#include <hostfxr.h>
#endif
#include <array>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Callback = std::uint64_t (*)(std::uint64_t, std::uint64_t);
using ErrorBuffer = std::array<char, 4096>;

class Library {
public:
    explicit Library(const fs::path& file) {
#ifdef _WIN32
        handle = LoadLibraryW(file.c_str());
        if (!handle) throw std::runtime_error("LoadLibrary failed: " + std::to_string(GetLastError()));
#else
        handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) throw std::runtime_error(dlerror());
#endif
    }
    Library(const Library&) = delete;
    template<class T> T symbol(const char* name) const {
#ifdef _WIN32
        auto result = GetProcAddress(handle, name);
#else
        auto result = dlsym(handle, name);
#endif
        if (!result) throw std::runtime_error(std::string("Missing native export: ") + name);
        return reinterpret_cast<T>(result);
    }
    // Each runtime is loaded for the process lifetime.
private:
#ifdef _WIN32
    HMODULE handle;
#else
    void* handle;
#endif
};

struct Transform { float x = 0.f; float y = 0.f; };
std::array<Transform,128> positions{};
unsigned writes;
std::array<bool,128> touched{};
bool invalid;

void install_scene() {
    static Funke::NativeEngineAPI api{};
    api.GetDeltaTime = [] { return 0.016667f; };
    api.GetPosition = [](std::uint32_t entity, float* x, float* y) {
        *x = positions[entity].x;
        *y = positions[entity].y;
    };
    api.SetPosition = [](std::uint32_t entity, float x, float y) { positions[entity] = {x, y}; };
    Funke::g_EngineAPI = &api;
}
std::uint64_t count(std::uint64_t, std::uint64_t) noexcept { return positions.size(); }
std::uint64_t elapsed(std::uint64_t, std::uint64_t) noexcept {
    return static_cast<std::uint64_t>(std::lround(double(Funke::g_EngineAPI->GetDeltaTime()) * 1'000'000.0));
}
std::uint64_t move_right(std::uint64_t id, std::uint64_t distance) noexcept {
    if (id >= positions.size() || touched[id]) { invalid = true; return 0; }
    touched[id] = true;
    auto entity = static_cast<std::uint32_t>(id);
    float x = 0.f, y = 0.f;
    Funke::g_EngineAPI->GetPosition(entity,&x,&y);
    Funke::g_EngineAPI->SetPosition(entity,x+static_cast<float>(distance),y);
    ++writes;
    return 1;
}
std::uint64_t checksum() {
    std::uint64_t result = 0;
    for (size_t i=0; i<positions.size(); ++i)
        result += (i+1)*static_cast<std::uint64_t>(positions[i].x);
    return result;
}

class Backend {
public:
    ErrorBuffer error{};
    virtual ~Backend() = default;
    virtual int compile() = 0;
    virtual void publish() = 0;
    virtual void frame() = 0;
    void require(int status) const {
        if (status != 0) throw std::runtime_error("Compiler/host error " + std::to_string(status) + ": " + error.data());
    }
};

class BoltBackend final : public Backend {
    Library library;
    std::string project;
    BoltCompiler* compiler = nullptr;
    BoltCompiledModule* next = nullptr;
    decltype(&bolt_compiler_destroy) destroy = library.symbol<decltype(destroy)>("bolt_compiler_destroy");
    decltype(&bolt_compiler_compile_project) build = library.symbol<decltype(build)>("bolt_compiler_compile_project");
    decltype(&bolt_compiler_load) load = library.symbol<decltype(load)>("bolt_compiler_load");
    decltype(&bolt_module_release) release = library.symbol<decltype(release)>("bolt_module_release");
    decltype(&bolt_compiler_run) run = library.symbol<decltype(run)>("bolt_compiler_run");
public:
    BoltBackend(const fs::path& lib, std::string path) : library(lib), project(std::move(path)) {
        const BoltHostBinding bindings[] = {{"EntityCount",count},{"DeltaMicros",elapsed},{"MoveRight",move_right}};
        auto create = library.symbol<decltype(&bolt_compiler_create)>("bolt_compiler_create");
        require(create(BOLT_COMPILER_HOST_ABI,BOLT_HOST_DEBUG,bindings,3,&compiler,error.data(),error.size()));
    }
    ~BoltBackend() override {
        if (next) release(next);
        if (compiler) destroy(compiler);
    }
    int compile() override { return build(compiler,project.c_str(),&next,error.data(),error.size()); }
    void publish() override {
        const int status = load(compiler,next,error.data(),error.size());
        release(next); next = nullptr; require(status);
    }
    void frame() override {
        int32_t code = -1;
        require(run(compiler,&code,error.data(),error.size()));
        if (code != 0) throw std::runtime_error("Bolt Main returned " + std::to_string(code));
    }
};

#ifndef BOLT_EDITOR_BOLT_ONLY
#ifdef _WIN32
#define FX_LITERAL(x) L##x
#else
#define FX_LITERAL(x) x
#endif
class ManagedBackend final : public Backend {
    Library library;
    using Stage = int (CORECLR_DELEGATE_CALLTYPE *)(char*, int);
    using Frame = int (CORECLR_DELEGATE_CALLTYPE *)(Callback, Callback, Callback, char*, int);
    using Configure = int (CORECLR_DELEGATE_CALLTYPE *)(const char*, const char*, const char*, const char*, char*, int);
    Stage compile_fn = nullptr, publish_fn = nullptr;
    Frame frame_fn = nullptr;
public:
    ManagedBackend(const fs::path& fxr, const fs::path& bridge, const char* project,
                   const char* source, const char* output, const char* dotnet) : library(fxr) {
        auto init = library.symbol<hostfxr_initialize_for_runtime_config_fn>("hostfxr_initialize_for_runtime_config");
        auto get = library.symbol<hostfxr_get_runtime_delegate_fn>("hostfxr_get_runtime_delegate");
        auto close = library.symbol<hostfxr_close_fn>("hostfxr_close");
        hostfxr_handle context = nullptr;
        auto config = bridge.parent_path() / "FunkeEditorBridge.runtimeconfig.json";
        int result = init(config.c_str(),nullptr,&context);
        if (result < 0 || !context) throw std::runtime_error("hostfxr initialization failed: "+std::to_string(result));
        void* raw = nullptr;
        result = get(context,hdt_load_assembly_and_get_function_pointer,&raw);
        close(context);
        if (result < 0 || !raw) throw std::runtime_error("Cannot obtain managed assembly loader: "+std::to_string(result));
        auto resolve = reinterpret_cast<load_assembly_and_get_function_pointer_fn>(raw);
        auto entry = [&](const char_t* name) {
            void* address = nullptr;
            int rc = resolve(bridge.c_str(),FX_LITERAL("EditorBench.Bridge, FunkeEditorBridge"),name,
                             UNMANAGEDCALLERSONLY_METHOD,nullptr,&address);
            if (rc < 0 || !address) throw std::runtime_error("Cannot resolve managed entry point: "+std::to_string(rc));
            return address;
        };
        auto configure = reinterpret_cast<Configure>(entry(FX_LITERAL("Configure")));
        compile_fn = reinterpret_cast<Stage>(entry(FX_LITERAL("Compile")));
        publish_fn = reinterpret_cast<Stage>(entry(FX_LITERAL("Publish")));
        frame_fn = reinterpret_cast<Frame>(entry(FX_LITERAL("Frame")));
        require(configure(project,source,output,dotnet,error.data(),error.size()));
    }
    int compile() override { return compile_fn(error.data(),error.size()); }
    void publish() override { require(publish_fn(error.data(),error.size())); }
    void frame() override { require(frame_fn(count,elapsed,move_right,error.data(),error.size())); }
};

#endif

long long nanoseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end-start).count();
}
fs::path utf8_path(const char* text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
}

int run_editor(int argc, char** argv) {
    try {
        const auto boot = Clock::now();
        install_scene();
        std::unique_ptr<Backend> backend;
        if (argc==4 && std::string(argv[1])=="bolt")
            backend=std::make_unique<BoltBackend>(utf8_path(argv[2]),argv[3]);
#ifndef BOLT_EDITOR_BOLT_ONLY
        else if (argc==8 && std::string(argv[1])=="csharp")
            backend=std::make_unique<ManagedBackend>(utf8_path(argv[2]),utf8_path(argv[3]),
                                                     argv[4],argv[5],argv[6],argv[7]);
#else
        else if (argc>1 && std::string(argv[1])=="csharp")
            throw std::runtime_error("Build the editor without --bolt-only to use the C# host.");
#endif
        else throw std::runtime_error("Usage: BoltEditorSim bolt LIB PROJECT | csharp HOSTFXR BRIDGE PROJECT SOURCE OUTPUT DOTNET");
        std::cout << "READY " << nanoseconds(boot,Clock::now()) << std::endl;
        std::string command;
        while (std::getline(std::cin,command)) {
            if (command=="quit") break;
            if (command!="reload" && command!="frame") throw std::runtime_error("Expected reload, frame or quit");
            writes=0; invalid=false; touched.fill(false);
            const auto start=Clock::now();
            auto compiled=start, published=start;
            if (command=="reload") {
                backend->error[0]=0;
                int rc=backend->compile();
                compiled=Clock::now();
                if (rc==1) {
                    if (!backend->error[0]) throw std::runtime_error("Rejected edit without diagnostic");
                    std::cerr << backend->error.data() << '\n';
                    std::cout << "REJECT " << nanoseconds(start,compiled) << ' ' << checksum() << std::endl;
                    continue;
                }
                backend->require(rc);
                backend->publish(); published=Clock::now();
            }
            backend->frame();
            auto returned=Clock::now();
            if (invalid || writes!=positions.size()) throw std::runtime_error("Expected one valid transform update per entity");
            std::cout << "FRAME " << nanoseconds(start,compiled) << ' ' << nanoseconds(compiled,published) << ' '
                      << nanoseconds(published,returned) << ' ' << nanoseconds(start,returned) << ' ' << checksum() << std::endl;
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> strings;
    for (int i=0; i<argc; ++i) {
        auto value = fs::path(argv[i]).u8string();
        strings.emplace_back(reinterpret_cast<const char*>(value.c_str()));
    }
    std::vector<char*> args;
    for (auto& value : strings) args.push_back(value.data());
    return run_editor(argc,args.data());
}
#else
int main(int argc, char** argv) { return run_editor(argc,argv); }
#endif
