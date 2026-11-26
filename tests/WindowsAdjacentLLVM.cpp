// The test executable runs outside the SDK and imports only Windows APIs.
#define NOMINMAX
#include <windows.h>
#include <bolt/CompilerHost.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

template<class T>
T entry(HMODULE library,const char* name) {
    auto address=GetProcAddress(library,name);
    if(!address)throw std::runtime_error(name);
    return reinterpret_cast<T>(address);
}

std::wstring absolute_path(const wchar_t* argument) {
    std::wstring path(32768,L'\0');
    auto count=GetFullPathNameW(argument,static_cast<DWORD>(path.size()),path.data(),nullptr);
    if(!count||count>=path.size())throw std::runtime_error("cannot resolve SDK path");
    path.resize(count);
    return path;
}

int wmain(int argc,wchar_t** argv) {
    if(argc!=2){std::fputs("Expected the full path to BoltCompiler.dll\n",stderr);return 2;}
    try {
        auto compiler_path=absolute_path(argv[1]);
        auto stop=compiler_path.find_last_of(L"\\/");
        if(stop==std::wstring::npos)throw std::runtime_error("compiler DLL has no parent directory");
        auto llvm=compiler_path.substr(0,stop+1)+L"LLVM-C.dll";
        if(GetFileAttributesW(llvm.c_str())==INVALID_FILE_ATTRIBUTES){
            std::puts("SKIP no LLVM-C.dll next to the compiler DLL");return 77;
        }
        SetEnvironmentVariableW(L"BOLT_LLVM_LIBRARY",nullptr);
        SetEnvironmentVariableW(L"PATH",L"");
        if(!SetDllDirectoryW(L""))throw std::runtime_error("cannot exclude the current DLL directory");
        if(GetModuleHandleW(L"LLVM-C.dll"))throw std::runtime_error("LLVM already loaded before test");
        auto library=LoadLibraryW(compiler_path.c_str());
        if(!library)throw std::runtime_error("cannot load BoltCompiler.dll");
        std::fputs("Compiler DLL loaded in isolated process\n",stderr);
        auto create=entry<decltype(&bolt_compiler_create)>(library,"bolt_compiler_create");
        auto compile=entry<decltype(&bolt_compiler_compile)>(library,"bolt_compiler_compile");
        auto load=entry<decltype(&bolt_compiler_load)>(library,"bolt_compiler_load");
        auto run=entry<decltype(&bolt_compiler_run)>(library,"bolt_compiler_run");
        auto release=entry<decltype(&bolt_module_release)>(library,"bolt_module_release");
        auto destroy=entry<decltype(&bolt_compiler_destroy)>(library,"bolt_compiler_destroy");
        std::array<char,2048> error{};
        BoltCompiler* compiler=nullptr;BoltCompiledModule* module=nullptr;
        std::int32_t status=0;
        auto ok=[&](int code){if(code!=BOLT_HOST_OK)throw std::runtime_error(error.data());};
        ok(create(BOLT_COMPILER_HOST_ABI,BOLT_HOST_RELEASE,nullptr,0,&compiler,error.data(),error.size()));
        constexpr char source[]="public static class Program{public: static int Main(){return 42;}}";
        ok(compile(compiler,"Host.bolt",source,sizeof(source)-1,&module,error.data(),error.size()));
        std::fputs("Release program compiled\n",stderr);
        ok(load(compiler,module,error.data(),error.size()));
        release(module);
        ok(run(compiler,&status,error.data(),error.size()));
        if(status!=42)throw std::runtime_error("release module returned wrong result");
        auto image=GetModuleHandleW(L"LLVM-C.dll");
        std::wstring path(32768,L'\0');
        auto length=image?GetModuleFileNameW(image,path.data(),static_cast<DWORD>(path.size())):0;
        if(!length||length>=path.size())throw std::runtime_error("cannot find loaded LLVM module");
        path.resize(length);
        if(CompareStringOrdinal(path.c_str(),-1,llvm.c_str(),-1,TRUE)!=CSTR_EQUAL)
            throw std::runtime_error("loaded LLVM is outside the SDK directory");
        destroy(compiler);
        std::puts("PASS isolated host compiled and executed through its SDK's adjacent LLVM-C.dll");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"%s\n",e.what());return 1;
    }
}
