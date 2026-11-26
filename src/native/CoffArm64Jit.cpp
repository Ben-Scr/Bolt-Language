#include <bolt/native/IR.hpp>
#if defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CoffArm64Link.hpp"

namespace bolt::native_backend {
namespace {
class WindowsArm64Image final : public JitImage {
    std::uint8_t* memory=nullptr;
    std::vector<RUNTIME_FUNCTION> unwind;
    bool registered=false;
    std::unordered_map<std::string,void*> exports;

    void clear() noexcept {
        if(registered)RtlDeleteFunctionTable(unwind.data());
        if(memory)VirtualFree(memory,0,MEM_RELEASE);
    }
    static void wincheck(bool ok,const char* operation){
        if(!ok)throw std::runtime_error(std::string("Windows ARM64 LLVM JIT: ")+operation+
            " failed (Win32 "+std::to_string(GetLastError())+")");
    }
    void protect(const coff_arm64::Region& region){
        if(!region.size)return;
        DWORD old=0,access=region.executable?PAGE_EXECUTE_READ:region.writable?PAGE_READWRITE:PAGE_READONLY;
        wincheck(VirtualProtect(memory+region.offset,region.size,access,&old)!=0,"VirtualProtect");
    }
public:
    WindowsArm64Image(const ir::Module& module,const std::vector<Symbol>& symbols,const std::vector<std::uint8_t>& object) {
        SYSTEM_INFO info{};GetSystemInfo(&info);
        coff_arm64::Layout layout(object,info.dwPageSize);
        memory=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,layout.bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        wincheck(memory!=nullptr,"VirtualAlloc");
        try {
            std::unordered_map<std::string,std::uintptr_t> addresses;
            for(const auto& symbol:symbols)addresses.emplace(symbol.name,reinterpret_cast<std::uintptr_t>(symbol.address));
            // LLVM can synthesize CRT calls (memcpy, memset, __chkstk) after IR lowering.
            // Declared Bolt/FFI callbacks are resolved per image before process-builtins.
            const HMODULE system[]={GetModuleHandleW(L"ntdll.dll"),GetModuleHandleW(L"kernel32.dll"),
                GetModuleHandleW(L"ucrtbase.dll"),GetModuleHandleW(L"msvcrt.dll")};
            coff_arm64::link(layout,memory,[&](const std::string& name)->std::uintptr_t{
                if(auto known=addresses.find(name);known!=addresses.end())return known->second;
                for(auto handle:system)if(handle)if(auto ptr=GetProcAddress(handle,name.c_str()))
                    return reinterpret_cast<std::uintptr_t>(ptr);
                return 0;
            });
            for(const auto& fn:module.functions)if(fn.exported){
                auto index=layout.definitions.find(fn.name);
                if(index==layout.definitions.end())throw std::runtime_error("LLVM JIT export is missing: "+fn.name);
                const auto& symbol=layout.symbols[index->second];const auto& section=layout.sections[symbol.section-1];
                coff_arm64::require(section.region.executable && symbol.offset<section.region.size,"invalid executable export");
                exports.emplace(fn.name,memory+section.region.offset+symbol.offset);
            }
            for(const auto& section:layout.sections)if(section.mapped){
                if(section.name==".pdata" || section.name.starts_with(".pdata$")){
                    static_assert(sizeof(RUNTIME_FUNCTION)==8);
                    coff_arm64::require(section.region.size%sizeof(RUNTIME_FUNCTION)==0,"truncated Windows unwind table");
                    auto n=section.region.size/sizeof(RUNTIME_FUNCTION),offset=unwind.size();
                    if(n){unwind.resize(offset+n);std::memcpy(unwind.data()+offset,memory+section.region.offset,section.region.size);}
                }
                protect(section.region);
            }
            protect(layout.thunks);protect(layout.imports);
            wincheck(FlushInstructionCache(GetCurrentProcess(),memory,layout.bytes)!=0,"FlushInstructionCache");
            if(!unwind.empty()){
                std::sort(unwind.begin(),unwind.end(),[](const auto& a,const auto& b){return a.BeginAddress<b.BeginAddress;});
                for(const auto& function:unwind)coff_arm64::require(function.BeginAddress<layout.bytes,"invalid unwind address");
                registered=RtlAddFunctionTable(unwind.data(),static_cast<DWORD>(unwind.size()),reinterpret_cast<DWORD64>(memory))!=0;
                wincheck(registered,"RtlAddFunctionTable");
            }
        }catch(...){clear();throw;}
    }
    ~WindowsArm64Image()override{clear();}
    void* symbol(const std::string& name)const override {
        auto it=exports.find(name);
        if(it==exports.end())throw std::runtime_error("JIT export not found: "+name);
        return it->second;
    }
};
}
std::unique_ptr<JitImage> windows_arm64_llvm_image(const ir::Module& module,const std::vector<Symbol>& symbols,const Options& options) {
    // LLVM 19 MCJIT/RuntimeDyld loses ARM64 COFF data-address addends on complete Bolt
    // modules. Reuse the normal Windows target machine, then relocate its AOT bytes
    // against one mapping with Windows image-relative unwind data and callback thunks.
    return std::make_unique<WindowsArm64Image>(module,symbols,llvm_object(module,options));
}
} // namespace bolt::native_backend
#endif
