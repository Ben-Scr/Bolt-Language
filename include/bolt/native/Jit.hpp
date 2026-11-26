#pragma once
#include <bolt/API.hpp>
#include <bolt/native/IR.hpp>
namespace bolt {
namespace host { class FastEditTemplate; }
using HostFunction = std::uint64_t(*)(std::uint64_t,std::uint64_t);
using HostFunctions = std::unordered_map<std::string,HostFunction>;
struct JitOptions {
    std::unordered_map<std::string,void*(*)(std::uint64_t,std::uint64_t)> pointer_bindings;
    std::unordered_map<std::string,std::uint64_t(*)(std::uint64_t,void*,void*)> submit_bindings;
    // "auto" uses the fast native backend when supported; "llvm" selects LLVM.
    std::string backend="auto";
    bool release=false;
    PreprocessorOptions preprocessing;
    std::string llvm_library;
    ExecutionOptions execution{};
    // Application arguments (excluding the executable name), copied per run.
    std::vector<std::string> arguments;
    std::istream* input=nullptr;
    std::ostream* output=nullptr;
};
// Compile once, execute multiple times. Each run starts a fresh language context;
// this API does not capture caller-local variables. Calls on one module serialize.
class NativeProgram {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit NativeProgram(const Program&,JitOptions = {});
    NativeProgram(const Program&,JitOptions,const HostFunctions&);
    // Internal constructors for independent generations of a source-set compilation.
    NativeProgram(const Program&,JitOptions,const HostFunctions&,ir::EditData&,std::shared_ptr<const native_backend::FastRecipe>&);
    NativeProgram(JitOptions,std::unordered_map<std::string,std::string>,const native_backend::FastRecipe&,const ir::Module* methods=nullptr);
private:
    friend class host::FastEditTemplate;
    void* data_symbol(const std::string&) const;
public:
    ~NativeProgram();
    NativeProgram(NativeProgram&&) noexcept;
    NativeProgram& operator=(NativeProgram&&) noexcept;
    NativeProgram(const NativeProgram&)=delete;
    NativeProgram& operator=(const NativeProgram&)=delete;
    int run();
    int run_persistent();
};
int jit_program(std::string_view,JitOptions = {});
int jit_statements(std::string_view,JitOptions = {});
int jit_file(const std::filesystem::path&,JitOptions = {});
}
