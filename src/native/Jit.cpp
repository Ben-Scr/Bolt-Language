#include "JitCall.hpp"
#include "../RuntimeInternal.hpp"
#include <bolt/native/Jit.hpp>
#include <bolt/native/ABI.hpp>
#include <bolt/Frontend.hpp>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <limits>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#ifdef BOLT_LINUX_MUSL
#include <bolt/detail/DiagnosticUnwind.h>
#endif
#endif

namespace bolt {
namespace {
thread_local detail::SharedExecutionBudget inherited_execution;

struct InheritedExecutionGuard {
    detail::SharedExecutionBudget previous;
    explicit InheritedExecutionGuard(detail::SharedExecutionBudget budget)
        :previous(std::exchange(inherited_execution,std::move(budget))) {}
    ~InheritedExecutionGuard(){inherited_execution=std::move(previous);}
};

using native_backend::detail::call_generated;
}
struct NativeProgram::Impl {
    JitOptions options;
    // Symbol stores a borrowed name pointer. Keep these strings unchanged and
    // alive until after the compiled image has been destroyed.
    std::vector<std::string> foreign_names;
    std::unique_ptr<native_backend::JitImage> image;
    std::unordered_map<std::string,std::string> sources;
    std::mutex execution;
    using Initialize=void(*)(BrContext*);
    using Entry=std::int32_t(*)(BrContext*);
    Initialize initialize=nullptr,finalize=nullptr;
    Entry entry=nullptr;
    Impl(const Program& p,JitOptions o,const HostFunctions& bindings,ir::EditData* edits=nullptr,std::shared_ptr<const native_backend::FastRecipe>* recipe=nullptr):options(std::move(o)),sources(p.sources){
        if(p.library_mode || !p.external_sources.empty())
            throw std::runtime_error("separately linked project libraries require an AOT build; use bolt compile or bolt run without --jit");
        if(options.execution.max_call_depth>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("native call-depth limit exceeds uint capacity");
        ir::LowerOptions lowering{true,options.input&&options.input!=&std::cin,
                                     options.output&&options.output!=&std::cout,options.release};
        lowering.edit_data=edits;
        auto module=ir::lower(p,lowering);
        native_backend::Options settings;settings.release=options.release;settings.debug_symbols=!options.release;settings.llvm_library=options.llvm_library;
        auto backend=options.backend;
        if(backend=="auto")backend=options.release||!native_backend::fast_supported()?"llvm":"fast";
        auto symbols=native_abi::symbols();
        foreign_names.reserve(module.externals.size());
        for(const auto& external:module.externals)if(external.foreign) {
            if (auto it=options.submit_bindings.find(external.name);it!=options.submit_bindings.end()) {
                using T=ir::Type;
                if (external.result!=T::I64 || external.parameters!=std::vector<T>{T::I64,T::Ptr,T::Ptr})
                    throw std::runtime_error("Callback submission binding requires ulong(ulong, void*, void*): "+external.name);
                foreign_names.push_back(external.name);
                symbols.push_back({foreign_names.back().c_str(),reinterpret_cast<void*>(it->second)});
                continue;
            }
            if (auto it=options.pointer_bindings.find(external.name);it!=options.pointer_bindings.end()) {
                using T=ir::Type;
                if (external.result!=T::Ptr || external.parameters!=std::vector<T>{T::I64,T::I64})
                    throw std::runtime_error("Pointer binding requires void*(ulong, ulong): "+external.name);
                foreign_names.push_back(external.name);
                symbols.push_back({foreign_names.back().c_str(),reinterpret_cast<void*>(it->second)});
                continue;
            }
            if(auto binding=bindings.find(external.name);binding!=bindings.end()) {
                using T=ir::Type;
                if(external.result!=T::I64||external.parameters!=std::vector<T>{T::I64,T::I64})
                    throw std::runtime_error("Host binding requires ulong(ulong, ulong): "+external.name);
                if(!binding->second||std::any_of(symbols.begin(),symbols.end(),[&](const auto& s){return external.name==s.name;}))
                    throw std::runtime_error("Invalid or reserved host binding: "+external.name);
                foreign_names.push_back(external.name);
                symbols.push_back({foreign_names.back().c_str(),reinterpret_cast<void*>(binding->second)});
                continue;
            }
#ifdef _WIN32
            const auto kernel=GetModuleHandleW(L"kernel32.dll");
            if(!kernel){const auto error=GetLastError();throw std::runtime_error("cannot resolve Windows extern C symbols: kernel32.dll is unavailable (Win32 error "+std::to_string(error)+")");}
            auto address=GetProcAddress(kernel,external.name.c_str());
            if(!address) {
                // Secure random uses the Windows CNG API directly. Keep bcrypt.dll
                // loaded for the process lifetime so JIT code can call its symbol.
                static const HMODULE bcrypt=LoadLibraryW(L"bcrypt.dll");
                if(bcrypt)address=GetProcAddress(bcrypt,external.name.c_str());
            }
            if(!address) {
                static const HMODULE winsock=LoadLibraryExW(L"ws2_32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                if(winsock)address=GetProcAddress(winsock,external.name.c_str());
            }
            if(!address){const auto error=GetLastError();throw std::runtime_error("unresolved Windows extern C symbol '"+external.name+"' in kernel32.dll/bcrypt.dll/ws2_32.dll (Win32 error "+std::to_string(error)+")");}
            foreign_names.push_back(external.name);
            symbols.push_back({foreign_names.back().c_str(),reinterpret_cast<void*>(address)});
#else
#ifdef BOLT_LINUX_MUSL
            if (external.name == "bolt_diagnostic_backtrace") {
                foreign_names.push_back(external.name);
                symbols.push_back({foreign_names.back().c_str(), reinterpret_cast<void*>(&bolt_diagnostic_backtrace)});
                continue;
            }
#endif
            dlerror();
            const auto address=dlsym(RTLD_DEFAULT,external.name.c_str());
            const auto* error=dlerror();
            if(error||!address)throw std::runtime_error("unresolved extern C symbol '"+external.name+"' in the host process: "+(error?std::string(error):std::string("symbol address is null")));
            foreign_names.push_back(external.name);
            symbols.push_back({foreign_names.back().c_str(),address});
#endif
        }
        if(recipe) {
            *recipe=native_backend::fast_recipe(module,symbols,settings);
            image=(*recipe)->link();
        } else if(backend=="fast")image=native_backend::fast_jit(module,symbols,settings);
        else if(backend=="llvm")image=native_backend::llvm_jit(module,symbols,settings);
        else throw std::runtime_error("JIT backend must be auto, fast, or llvm");
        resolve_entries();
    }
    Impl(JitOptions o,std::unordered_map<std::string,std::string> text,const native_backend::FastRecipe& recipe,const ir::Module* methods)
        :options(std::move(o)),image(methods?recipe.link(*methods):recipe.link()),sources(std::move(text)){resolve_entries();}
    void resolve_entries() {
        initialize=reinterpret_cast<Initialize>(image->symbol("bolt_module_initialize"));
        finalize=reinterpret_cast<Initialize>(image->symbol("bolt_module_finalize"));
        entry=reinterpret_cast<Entry>(image->symbol("bolt_entry"));
    }
    BrContext* session = nullptr;
    ~Impl() {
        if (session) {
            call_generated(finalize,session);
            br_destroy(session);
        }
    }
    int run(bool persistent){

        std::lock_guard lock(execution);
        if (!persistent && session) throw std::runtime_error("This module already has a persistent session");
        if (session && session->failed) throw std::runtime_error("Persistent session failed; replace the module to restart");
        const bool first = !session;
        auto* context=session ? session : br_create(options.execution.max_steps,static_cast<std::uint32_t>(options.execution.max_call_depth));
        if(!context)throw std::runtime_error("could not allocate a JIT execution context");
        if(first)native_abi::configure_execution(context,options.execution,inherited_execution);
        if (persistent && first) context->flags |= BR_PRIVATE_STEPS;
        if (persistent) session = context;
        native_abi::reset_execution(context);
        try {
            if (first) {
                native_abi::streams(context,options.input,options.output);
                native_abi::arguments(context,options.arguments);
                call_generated(initialize,context);
            }
            const int status=context->failed ? 0 : call_generated(entry,context);
            if (!persistent) call_generated(finalize,context);
            native_abi::rethrow(context);
            if (!persistent) br_destroy(context);
            return status;
        } catch (Error& error) {
            if (!persistent) br_destroy(context);
            auto i=sources.find(error.diagnostic.location.file);
            if(i!=sources.end()) attach_source(error.diagnostic,i->second);
            throw;
        } catch (...) {
            if (!persistent) br_destroy(context);
            throw;
        }
    }
};
NativeProgram::NativeProgram(const Program& p,JitOptions options):NativeProgram(p,std::move(options),{}){}
NativeProgram::NativeProgram(const Program& p,JitOptions options,const HostFunctions& bindings)
    :impl_(std::make_unique<Impl>(p,std::move(options),bindings)){}
NativeProgram::NativeProgram(const Program& p,JitOptions o,const HostFunctions& b,ir::EditData& edits,std::shared_ptr<const native_backend::FastRecipe>& recipe)
    :impl_(std::make_unique<Impl>(p,std::move(o),b,&edits,&recipe)){}
NativeProgram::NativeProgram(JitOptions o,std::unordered_map<std::string,std::string> sources,const native_backend::FastRecipe& recipe,const ir::Module* methods)
    :impl_(std::make_unique<Impl>(std::move(o),std::move(sources),recipe,methods)){}
void* NativeProgram::data_symbol(const std::string& name) const {return impl_->image->symbol(name);}
NativeProgram::~NativeProgram()=default;
NativeProgram::NativeProgram(NativeProgram&&)noexcept=default;
NativeProgram& NativeProgram::operator=(NativeProgram&&)noexcept=default;
int NativeProgram::run(){return impl_->run(false);}
int NativeProgram::run_persistent(){return impl_->run(true);}
int jit_program(std::string_view text,JitOptions o){auto pp=o.preprocessing;if(pp.configuration.empty())pp.configuration=o.release?"Release":"Debug";auto p=compile_source(text,"<runtime>",true,pp);NativeProgram executable(p,std::move(o));return executable.run();}
int jit_statements(std::string_view code,JitOptions o){
    const std::string source="import Math\nimport Random\nimport Type\nimport System.Collections.Generic\nstatic class Program { public: void Main() {\n"+std::string(code)+"\n} }\n";
    return jit_program(source,std::move(o));
}
int jit_file(const std::filesystem::path& path,JitOptions o){auto pp=o.preprocessing;if(pp.configuration.empty())pp.configuration=o.release?"Release":"Debug";auto p=load(path,true,pp);NativeProgram executable(p,std::move(o));return executable.run();}
namespace detail {
int jit_program_nested(std::string_view source,JitOptions options,SharedExecutionBudget budget){
    InheritedExecutionGuard guard(std::move(budget));
    return bolt::jit_program(source,std::move(options));
}
int jit_statements_nested(std::string_view source,JitOptions options,SharedExecutionBudget budget){
    InheritedExecutionGuard guard(std::move(budget));
    return bolt::jit_statements(source,std::move(options));
}
int jit_file_nested(const std::filesystem::path& path,JitOptions options,SharedExecutionBudget budget){
    InheritedExecutionGuard guard(std::move(budget));
    return bolt::jit_file(path,std::move(options));
}
}
} // namespace bolt
