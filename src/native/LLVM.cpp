#include <bolt/native/IR.hpp>
#include <cstdlib>
#include <cstring>
#ifdef __FreeBSD__
#include <filesystem>
#endif
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bolt/Path.hpp>
#if defined(__x86_64__) || defined(_M_X64)
#define BOLT_WINDOWS_X64_MCJIT 1
#include "WindowsX64JitMemory.hpp"
#endif
#else
#include <dlfcn.h>
#endif

// This bridge deliberately uses only LLVM's stable C ABI, not C++ headers or a
// clang subprocess. The declarations below are the small ABI subset we consume.
// Reference: LLVM 19 llvm-c/{Core,IRReader,TargetMachine,ExecutionEngine}.h and
// llvm-c/Transforms/PassBuilder.h. No LLVM implementation is vendored here.
namespace bolt::native_backend {
std::string macos_deployment_target() {
#if defined(__APPLE__) && defined(BOLT_MACOS_DEPLOYMENT_TARGET)
    return BOLT_MACOS_DEPLOYMENT_TARGET;
#elif defined(__APPLE__) && defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
    constexpr auto value = __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__;
    return std::to_string(value / 10000) + "." + std::to_string((value / 100) % 100) + "." + std::to_string(value % 100);
#else
    return "";
#endif
}
std::string host_target() {
#if defined(_M_X64) || defined(__x86_64__)
    const std::string cpu="x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    const std::string cpu="aarch64";
#elif defined(__linux__) && defined(__riscv) && __riscv_xlen==64
    const std::string cpu="riscv64";
#else
    throw std::runtime_error("the direct Bolt backend currently requires an x86-64, ARM64 or Linux RV64 host");
#endif
#ifdef _WIN32
    return cpu+"-pc-windows-msvc";
#elif defined(__APPLE__)
    return (cpu=="aarch64"?"arm64":"x86_64")+std::string("-apple-macosx")+macos_deployment_target();
#elif defined(__FreeBSD__)
    return cpu+"-unknown-freebsd";
#elif defined(BOLT_LINUX_MUSL)
    return cpu+"-unknown-linux-musl";
#else
    return cpu+"-unknown-linux-gnu";
#endif
}
std::string aot_target(const std::string& requested) {
    const auto host=host_target();
    if(requested.empty() || requested=="host" || requested==host)return host;
#if defined(__APPLE__)
    for(const std::string arch:{"x86_64","arm64"}) {
        const auto triple=arch+"-apple-macosx"+macos_deployment_target();
        if(requested==arch || requested==triple || requested==arch+"-apple-darwin" ||
           (arch=="arm64" && requested=="aarch64-apple-darwin"))return triple;
    }
#elif defined(BOLT_LINUX_MUSL)
    if(requested==host.substr(0,host.find('-'))+"-linux-musl")return host;
#elif defined(__linux__) && defined(__GLIBC__)
    for(const auto* arch:{"x86_64","aarch64","riscv64"}) {
        const auto triple=std::string(arch)+"-unknown-linux-gnu";
        if(requested==triple || requested==std::string(arch)+"-linux-gnu")return triple;
    }
#endif
    throw std::runtime_error("unsupported native target '"+requested+"' for this compiler (host "+host+")");
}
namespace {
using P=void*;
#if defined(__linux__) && defined(__riscv) && __riscv_xlen == 64
#define BOLT_RISCV_ORC 1
// The LLVM 19 C ABI types LLVMJITEvaluatedSymbol and LLVMOrcCSymbolMapPair.
struct OrcSymbol { std::uint64_t address; std::uint8_t generic_flags, target_flags; };
struct OrcDefinition { P name; OrcSymbol value; };
static_assert(sizeof(OrcSymbol)==16 && sizeof(OrcDefinition)==24);
#endif
struct MCOptions { unsigned OptLevel; int CodeModel; int NoFramePointerElim; int EnableFastISel; P MCJMM; };
class LLVM {
    P library=nullptr;
    template<class F> F load(const char* name) {
#ifdef _WIN32
        auto value=GetProcAddress(static_cast<HMODULE>(library),name);
#else
        auto value=dlsym(library,name);
#endif
        if(!value)throw std::runtime_error(std::string("LLVM C API symbol is unavailable: ")+name+". Use a shared LLVM 19+ build with native code generation and MCJIT enabled.");
        return reinterpret_cast<F>(value);
    }
public:
#define API(ret,n,args) ret (*n) args = nullptr
    API(void,LLVMGetVersion,(unsigned*,unsigned*,unsigned*));
    API(P,LLVMContextCreate,()); API(void,LLVMContextDispose,(P));
    API(P,LLVMCreateMemoryBufferWithMemoryRangeCopy,(const char*,std::size_t,const char*));
    API(int,LLVMParseIRInContext,(P,P,P*,char**));
    API(int,LLVMVerifyModule,(P,int,char**));
    API(void,LLVMDisposeMessage,(char*)); API(void,LLVMDisposeModule,(P));
    API(int,LLVMGetTargetFromTriple,(const char*,P*,char**));
    API(P,LLVMCreateTargetMachineOptions,());
    API(void,LLVMDisposeTargetMachineOptions,(P));
    API(void,LLVMTargetMachineOptionsSetCPU,(P,const char*));
    API(void,LLVMTargetMachineOptionsSetFeatures,(P,const char*));
    API(void,LLVMTargetMachineOptionsSetABI,(P,const char*));
    API(void,LLVMTargetMachineOptionsSetCodeGenOptLevel,(P,int));
    API(void,LLVMTargetMachineOptionsSetRelocMode,(P,int));
    API(P,LLVMCreateTargetMachineWithOptions,(P,const char*,P));
    API(P,LLVMCreateTargetMachine,(P,const char*,const char*,const char*,int,int,int));
    API(void,LLVMDisposeTargetMachine,(P)); API(void,LLVMSetTarget,(P,const char*));
    API(P,LLVMCreateTargetDataLayout,(P)); API(char*,LLVMCopyStringRepOfTargetData,(P));
    API(void,LLVMSetDataLayout,(P,const char*)); API(void,LLVMDisposeTargetData,(P));
    API(P,LLVMCreatePassBuilderOptions,()); API(void,LLVMDisposePassBuilderOptions,(P));
    API(P,LLVMRunPasses,(P,const char*,P,P));
    API(char*,LLVMGetErrorMessage,(P)); API(void,LLVMDisposeErrorMessage,(char*));
    API(int,LLVMTargetMachineEmitToMemoryBuffer,(P,P,int,char**,P*));
    API(const char*,LLVMGetBufferStart,(P)); API(std::size_t,LLVMGetBufferSize,(P)); API(void,LLVMDisposeMemoryBuffer,(P));
    API(void,LLVMLinkInMCJIT,()); API(void,LLVMInitializeMCJITCompilerOptions,(MCOptions*,std::size_t));
#ifdef BOLT_WINDOWS_X64_MCJIT
    API(char*,LLVMCreateMessage,(const char*));
    API(P,LLVMCreateSimpleMCJITMemoryManager,(P,decltype(&WindowsX64JitMemory::code),decltype(&WindowsX64JitMemory::data),decltype(&WindowsX64JitMemory::finish),decltype(&WindowsX64JitMemory::destroy)));
#endif
    API(int,LLVMCreateMCJITCompilerForModule,(P*,P,MCOptions*,std::size_t,char**));
    API(void,LLVMDisposeExecutionEngine,(P)); API(std::uint64_t,LLVMGetFunctionAddress,(P,const char*));
    API(P,LLVMGetNamedFunction,(P,const char*));
    API(void,LLVMAddGlobalMapping,(P,P,P));
#ifdef BOLT_RISCV_ORC
    API(P,LLVMOrcCreateLLJIT,(P*,P));
    API(P,LLVMOrcDisposeLLJIT,(P));
    API(void,LLVMConsumeError,(P));
    API(P,LLVMOrcLLJITGetMainJITDylib,(P));
    API(P,LLVMOrcLLJITMangleAndIntern,(P,const char*));
    API(P,LLVMOrcAbsoluteSymbols,(OrcDefinition*,std::size_t));
    API(P,LLVMOrcJITDylibDefine,(P,P));
    API(void,LLVMOrcDisposeMaterializationUnit,(P));
    API(P,LLVMOrcLLJITAddObjectFile,(P,P,P));
    API(P,LLVMOrcLLJITLookup,(P,std::uint64_t*,const char*));
#endif
#undef API
    std::string version;
    explicit LLVM(const std::string& supplied) {
        std::vector<std::string> paths;
        if(!supplied.empty())paths.push_back(supplied);
        else {
            if(const auto* e=std::getenv("BOLT_LLVM_LIBRARY"))paths.emplace_back(e);
#ifdef _WIN32
            // Embedders often load BoltCompiler.dll by its full path while their executable
            // and current directory are elsewhere. Resolve the bundled LLVM next to this
            // module without altering the host's process-wide DLL search directories.
            static wchar_t module_anchor=0;
            HMODULE owner=nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  &module_anchor,&owner)) {
                std::wstring name(32768,L'\0');
                auto length=GetModuleFileNameW(owner,name.data(),static_cast<DWORD>(name.size()));
                if(length && length<name.size()){
                    name.resize(length);
                    paths.push_back(bolt::path_to_utf8(std::filesystem::path(name).parent_path()/L"LLVM-C.dll"));
                }
            }
            paths.insert(paths.end(),{"LLVM-C.dll","libLLVM.dll","LLVM.dll"});
#elif defined(__APPLE__)
            paths.insert(paths.end(),{"@loader_path/llvm/lib/libLLVM.19.1.dylib","libLLVM.dylib","/opt/homebrew/opt/llvm/lib/libLLVM.dylib","/usr/local/opt/llvm/lib/libLLVM.dylib"});
#elif defined(__FreeBSD__)
            Dl_info owner{};
            if(dladdr(reinterpret_cast<const void*>(&host_target),&owner)&&owner.dli_fname) {
                const auto bundled=std::filesystem::absolute(owner.dli_fname).parent_path()/"llvm/lib/libLLVM.so.19.1";
                paths.push_back(bundled.string());
            }
            paths.insert(paths.end(),{"libLLVM.so.19.1","libLLVM.so","/usr/local/llvm19/lib/libLLVM.so",
                                      "/usr/local/llvm20/lib/libLLVM.so","/usr/local/llvm21/lib/libLLVM.so"});
#else
            paths.insert(paths.end(),{"libLLVM.so.19.1","libLLVM-19.so","libLLVM.so.20.1","libLLVM-20.so","libLLVM.so.21.1","libLLVM-21.so","libLLVM.so.22.1","libLLVM-22.so","libLLVM.so"});
#endif
        }
        for(const auto& p:paths){
#ifdef _WIN32
            auto wide=bolt::path_from_utf8(p).wstring();library=LoadLibraryW(wide.c_str());
#else
            library=dlopen(p.c_str(),RTLD_NOW|RTLD_LOCAL);
#endif
            if(library)break;
        }
        if(!library)throw std::runtime_error("LLVM shared library not found. Install/bundle LLVM 19+ or set BOLT_LLVM_LIBRARY to its shared library. No C++ compiler is invoked by the LLVM backend.");
#define GET(n) n=load<decltype(n)>(#n)
        GET(LLVMGetVersion); GET(LLVMContextCreate);GET(LLVMContextDispose);GET(LLVMCreateMemoryBufferWithMemoryRangeCopy);
        GET(LLVMParseIRInContext);GET(LLVMVerifyModule);GET(LLVMDisposeMessage);GET(LLVMDisposeModule);GET(LLVMGetTargetFromTriple);
        GET(LLVMCreateTargetMachineOptions);GET(LLVMDisposeTargetMachineOptions);GET(LLVMTargetMachineOptionsSetCPU);
        GET(LLVMTargetMachineOptionsSetFeatures);GET(LLVMTargetMachineOptionsSetABI);GET(LLVMTargetMachineOptionsSetCodeGenOptLevel);
        GET(LLVMTargetMachineOptionsSetRelocMode);GET(LLVMCreateTargetMachineWithOptions);
        GET(LLVMCreateTargetMachine);GET(LLVMDisposeTargetMachine);GET(LLVMSetTarget);GET(LLVMCreateTargetDataLayout);GET(LLVMCopyStringRepOfTargetData);
        GET(LLVMSetDataLayout);GET(LLVMDisposeTargetData);GET(LLVMCreatePassBuilderOptions);GET(LLVMDisposePassBuilderOptions);GET(LLVMRunPasses);
        GET(LLVMGetErrorMessage);GET(LLVMDisposeErrorMessage);GET(LLVMTargetMachineEmitToMemoryBuffer);GET(LLVMGetBufferStart);GET(LLVMGetBufferSize);GET(LLVMDisposeMemoryBuffer);
#ifdef BOLT_WINDOWS_X64_MCJIT
        GET(LLVMCreateMessage);GET(LLVMCreateSimpleMCJITMemoryManager);
#endif
        GET(LLVMLinkInMCJIT);GET(LLVMInitializeMCJITCompilerOptions);GET(LLVMCreateMCJITCompilerForModule);GET(LLVMDisposeExecutionEngine);GET(LLVMGetFunctionAddress);GET(LLVMGetNamedFunction);GET(LLVMAddGlobalMapping);
#ifdef BOLT_RISCV_ORC
        GET(LLVMOrcCreateLLJIT);GET(LLVMOrcDisposeLLJIT);GET(LLVMConsumeError);
        GET(LLVMOrcLLJITGetMainJITDylib);GET(LLVMOrcLLJITMangleAndIntern);
        GET(LLVMOrcAbsoluteSymbols);GET(LLVMOrcJITDylibDefine);GET(LLVMOrcDisposeMaterializationUnit);
        GET(LLVMOrcLLJITAddObjectFile);GET(LLVMOrcLLJITLookup);
#endif
#undef GET
        unsigned major=0,minor=0,patch=0;LLVMGetVersion(&major,&minor,&patch);
        if(major<19)throw std::runtime_error("Bolt direct code generation requires LLVM 19 or newer");
        version=std::to_string(major)+"."+std::to_string(minor)+"."+std::to_string(patch);
        // Initialize the supported targets when present. The selected target must
        // be supported by the actual LLVM library; no unsupported host fallback.
        for(const char* arch:{"X86","AArch64","RISCV"})for(const char* part:{"TargetInfo","Target","TargetMC","AsmPrinter","AsmParser"}) {
            auto n="LLVMInitialize"+std::string(arch)+part;
#ifdef _WIN32
            auto p=GetProcAddress(static_cast<HMODULE>(library),n.c_str());
#else
            auto p=dlsym(library,n.c_str());
#endif
            if(p)reinterpret_cast<void(*)()>(p)();
        }
        LLVMLinkInMCJIT();
    }
    std::string message(char* p) {std::string result=p?p:"unspecified LLVM error";if(p)LLVMDisposeMessage(p);return result;}
};
std::mutex llvm_mutex;
LLVM& library(const Options& opt) {
    // Keep the shared library loaded for the process (LLVM owns global target
    // registries), but own and release each C-API bridge at normal shutdown.
    static std::unordered_map<std::string,std::unique_ptr<LLVM>> instances;
    std::string key=opt.llvm_library;
    if(key.empty())if(const auto* env=std::getenv("BOLT_LLVM_LIBRARY"))key=env;
    auto it=instances.find(key);if(it!=instances.end())return *it->second;
    auto value=std::make_unique<LLVM>(key);auto* ptr=value.get();instances.emplace(key,std::move(value));return *ptr;
}
struct Build {
    LLVM& api;P context=nullptr,module=nullptr,machine=nullptr;
    explicit Build(LLVM& a,const ir::Module& input,const Options& opt):api(a){
        const auto triple=aot_target(opt.target);
        context=api.LLVMContextCreate();
        try {
            const auto text=ir::llvm_text(input,triple,opt.debug_symbols,opt.release);
            auto buf=api.LLVMCreateMemoryBufferWithMemoryRangeCopy(text.data(),text.size(),"bolt-native-ir");
            char* error=nullptr;
            if(api.LLVMParseIRInContext(context,buf,&module,&error))throw std::runtime_error("invalid generated LLVM IR: "+api.message(error));
            if(api.LLVMVerifyModule(module,2,&error))throw std::runtime_error("LLVM verification failed: "+api.message(error));
            if(error)api.LLVMDisposeMessage(error);
            P target=nullptr;error=nullptr;
            if(api.LLVMGetTargetFromTriple(triple.c_str(),&target,&error))throw std::runtime_error(api.message(error));
            if(triple.starts_with("riscv64-")) {
                auto settings=api.LLVMCreateTargetMachineOptions();
                api.LLVMTargetMachineOptionsSetCPU(settings,"generic-rv64");
                api.LLVMTargetMachineOptionsSetFeatures(settings,"+m,+a,+f,+d,+c,+zicsr,+zifencei");
                api.LLVMTargetMachineOptionsSetABI(settings,"lp64d");
                api.LLVMTargetMachineOptionsSetCodeGenOptLevel(settings,opt.release?3:0);
                api.LLVMTargetMachineOptionsSetRelocMode(settings,2);
                machine=api.LLVMCreateTargetMachineWithOptions(target,triple.c_str(),settings);
                api.LLVMDisposeTargetMachineOptions(settings);
            } else machine=api.LLVMCreateTargetMachine(target,triple.c_str(),"generic","",opt.release?3:0,2,0);
            if(!machine)throw std::runtime_error("LLVM could not create a target machine for "+triple);
            api.LLVMSetTarget(module,triple.c_str());auto layout=api.LLVMCreateTargetDataLayout(machine);auto dl=api.LLVMCopyStringRepOfTargetData(layout);
            api.LLVMSetDataLayout(module,dl);api.LLVMDisposeMessage(dl);api.LLVMDisposeTargetData(layout);
            if(opt.release){auto passes=api.LLVMCreatePassBuilderOptions();auto e=api.LLVMRunPasses(module,"default<O3>",machine,passes);api.LLVMDisposePassBuilderOptions(passes);if(e){auto msg=api.LLVMGetErrorMessage(e);std::string s=msg;api.LLVMDisposeErrorMessage(msg);throw std::runtime_error("LLVM optimization failed: "+s);}}
        }catch(...){clear();throw;}
    }
    void clear(){if(module)api.LLVMDisposeModule(module);if(machine)api.LLVMDisposeTargetMachine(machine);if(context)api.LLVMContextDispose(context);module=machine=context=nullptr;}
    ~Build(){clear();}
};
#ifdef BOLT_RISCV_ORC
// RV64 ELF needs JITLink relocations. Feed ORC the same LP64D object used
// by the AOT path so calls into C or C++ use the hard-float ABI in both modes.
class OrcImage final : public JitImage {
    LLVM& api;
    P engine;
    std::unordered_map<std::string,P> exports;
public:
    OrcImage(LLVM& api,P engine,std::unordered_map<std::string,P> exports)
        : api(api),engine(engine),exports(std::move(exports)) {}
    ~OrcImage() override {
        std::lock_guard lock(llvm_mutex);
        if(auto error=api.LLVMOrcDisposeLLJIT(engine))api.LLVMConsumeError(error);
    }
    P symbol(const std::string& name)const override {
        auto found=exports.find(name);
        if(found==exports.end())throw std::runtime_error("JIT export not found: "+name);
        return found->second;
    }
};
void check_orc(LLVM& api,P error){
    if(!error)return;
    auto message=api.LLVMGetErrorMessage(error);
    std::string text=message;
    api.LLVMDisposeErrorMessage(message);
    throw std::runtime_error("LLVM ORC JIT: "+text);
}
std::unique_ptr<JitImage> riscv_jit(Build& build,const ir::Module& module,const std::vector<Symbol>& symbols){
    auto& api=build.api;
    P engine=nullptr;
    check_orc(api,api.LLVMOrcCreateLLJIT(&engine,nullptr));
    try{
        auto dylib=api.LLVMOrcLLJITGetMainJITDylib(engine);
        std::vector<OrcDefinition> definitions;
        definitions.reserve(symbols.size());
        for(const auto& symbol:symbols)if(api.LLVMGetNamedFunction(build.module,symbol.name)){
            constexpr std::uint8_t exported_callable=1u|4u;
            auto name=api.LLVMOrcLLJITMangleAndIntern(engine,symbol.name);
            definitions.push_back({name,{reinterpret_cast<std::uintptr_t>(symbol.address),exported_callable,0}});
        }
        if(!definitions.empty()){
            auto unit=api.LLVMOrcAbsoluteSymbols(definitions.data(),definitions.size());
            if(auto error=api.LLVMOrcJITDylibDefine(dylib,unit)){
                api.LLVMOrcDisposeMaterializationUnit(unit);
                check_orc(api,error);
            }
        }
        P object=nullptr;
        char* diagnostic=nullptr;
        if(api.LLVMTargetMachineEmitToMemoryBuffer(build.machine,build.module,1,&diagnostic,&object))
            throw std::runtime_error("LLVM object generation failed: "+api.message(diagnostic));
        check_orc(api,api.LLVMOrcLLJITAddObjectFile(engine,dylib,object));
        std::unordered_map<std::string,P> exports;
        for(const auto& function:module.functions)if(function.exported){
            std::uint64_t address=0;
            check_orc(api,api.LLVMOrcLLJITLookup(engine,&address,function.name.c_str()));
            exports.emplace(function.name,reinterpret_cast<P>(static_cast<std::uintptr_t>(address)));
        }
        return std::make_unique<OrcImage>(api,engine,std::move(exports));
    }catch(...){
        if(auto error=api.LLVMOrcDisposeLLJIT(engine))api.LLVMConsumeError(error);
        throw;
    }
}
#endif
class LLVMImage final:public JitImage {
    LLVM& api;P engine,context;std::unordered_map<std::string,P> exports;
public:
    LLVMImage(LLVM& a,P e,P c,std::unordered_map<std::string,P> names):api(a),engine(e),context(c),exports(std::move(names)){}
    ~LLVMImage() override {std::lock_guard lock(llvm_mutex);api.LLVMDisposeExecutionEngine(engine);api.LLVMContextDispose(context);}
    P symbol(const std::string& s)const override {auto i=exports.find(s);if(i==exports.end())throw std::runtime_error("JIT export not found: "+s);return i->second;}
};
}
std::string llvm_version(const Options& opt){std::lock_guard lock(llvm_mutex);return library(opt).version;}
std::vector<std::uint8_t> llvm_object(const ir::Module& m,const Options& opt){
    std::lock_guard lock(llvm_mutex);auto& api=library(opt);Build b(api,m,opt);char* error=nullptr;P buffer=nullptr;
    if(api.LLVMTargetMachineEmitToMemoryBuffer(b.machine,b.module,1,&error,&buffer))throw std::runtime_error("LLVM object generation failed: "+api.message(error));
    const auto* start=reinterpret_cast<const std::uint8_t*>(api.LLVMGetBufferStart(buffer));auto size=api.LLVMGetBufferSize(buffer);std::vector<std::uint8_t> bytes(start,start+size);api.LLVMDisposeMemoryBuffer(buffer);return bytes;
}
#if defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
std::unique_ptr<JitImage> windows_arm64_llvm_image(const ir::Module&,const std::vector<Symbol>&,const Options&);
#endif
std::unique_ptr<JitImage> llvm_jit(const ir::Module& m,const std::vector<Symbol>& symbols,const Options& opt){
    if(aot_target(opt.target)!=host_target())throw std::runtime_error("JIT compilation requires the host target");
#if defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
    return windows_arm64_llvm_image(m,symbols,opt);
#else
    std::lock_guard lock(llvm_mutex);auto& api=library(opt);Build b(api,m,opt);
#ifdef BOLT_RISCV_ORC
    return riscv_jit(b,m,symbols);
#else
    const auto module=b.module;
    MCOptions options{};api.LLVMInitializeMCJITCompilerOptions(&options,sizeof(options));options.OptLevel=opt.release?3u:0u;options.NoFramePointerElim=opt.debug_symbols?1:0;options.EnableFastISel=opt.release?0:1;
#ifdef BOLT_WINDOWS_X64_MCJIT
    auto memory = std::make_unique<WindowsX64JitMemory>(api.LLVMCreateMessage);
    options.MCJMM = api.LLVMCreateSimpleMCJITMemoryManager(memory.get(), WindowsX64JitMemory::code,
        WindowsX64JitMemory::data, WindowsX64JitMemory::finish, WindowsX64JitMemory::destroy);
    if (!options.MCJMM) throw std::runtime_error("Could not create the Windows x64 JIT memory manager");
    memory.release(); // MCJIT owns the manager and its callbacks from this point.
#endif
    P engine=nullptr;char* error=nullptr;
    if(api.LLVMCreateMCJITCompilerForModule(&engine,b.module,&options,sizeof(options),&error))throw std::runtime_error("LLVM JIT initialization failed: "+api.message(error));
    b.module=nullptr;
    try{
        // Resolve host callbacks per image; two engines can register the same
        // external name with different targets without sharing LLVM global state.
        for(const auto& s:symbols)
            if(auto declaration=api.LLVMGetNamedFunction(module,s.name))
                api.LLVMAddGlobalMapping(engine,declaration,s.address);
        std::unordered_map<std::string,P> names;for(const auto& f:m.functions)if(f.exported){auto address=api.LLVMGetFunctionAddress(engine,f.name.c_str());if(!address)throw std::runtime_error("LLVM JIT export is missing: "+f.name);names.emplace(f.name,reinterpret_cast<P>(static_cast<std::uintptr_t>(address)));}
        auto result=std::make_unique<LLVMImage>(api,engine,b.context,std::move(names));b.context=nullptr;return result;
    }catch(...){api.LLVMDisposeExecutionEngine(engine);throw;}
#endif
#endif
}
} // namespace bolt::native_backend
