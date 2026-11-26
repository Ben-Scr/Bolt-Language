#pragma once
// Bolt's backend-neutral, typed native IR. Language semantics are resolved before
// this layer: the code generators never parse .bolt or dispatch AST instructions.
#include <bolt/Ast.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bolt::ir {
enum class Type { Void, I8, I16, I32, I64, F32, F64, Ptr };
unsigned width(Type type);
bool floating(Type type);
const char* name(Type type);
using Reg = std::uint32_t;
using Label = std::uint32_t;
enum class Op {
    Constant, Address, Alloca, Offset, Load, Store, Zero,
    Add, Sub, Mul, SDiv, UDiv, SRem, URem, And, Or, Xor, Shl, LShr, AShr,
    FAdd, FSub, FMul, FDiv, FNeg,
    CheckedAdd, CheckedSub, CheckedMul,
    Compare, Cast, Call, CallIndirect, Jump, Branch, Return, Select,
    // A fixed-size copy between whole array elements, identical or disjoint.
    CopyElement, AtomicCompareExchange, AtomicAdd, FSqrt
};
struct Instruction {
    Op op{};
    Reg result = 0, overflow = 0;
    std::vector<Reg> args;
    std::uint64_t immediate = 0; // raw constant bits, byte offset, or stack size
    std::string text;           // symbol, comparison predicate, or cast operation
    Label yes = 0, no = 0;
    Location at;
    // Nonzero for a move of whole scalar elements within one live allocation.
    std::uint8_t storage_move_stride = 0;
};
struct Block {
    std::vector<Instruction> code;
    // Rare checked fallback. Keep calls out of line and favour its sibling edge.
    bool cold = false;
};
struct Function {
    std::string name, display_name;
    Type result = Type::Void;
    std::vector<Type> parameters;
    // Register zero is invalid. Parameters occupy registers 1..parameters.size().
    std::vector<Type> registers{Type::Void};
    std::vector<Block> blocks;
    Location at;
    bool exported = false;
    bool cold = false;
};
struct External {
    std::string name;
    Type result;
    std::vector<Type> parameters;
    bool foreign = false;
    // C callers extend narrow signed integers according to the target ABI.
    bool signed_result = false;
    std::vector<bool> signed_arguments;
    bool signed_argument(std::size_t index) const {
        return index<signed_arguments.size() && signed_arguments[index];
    }
};
struct Relocation { std::size_t offset = 0; std::string symbol; std::int64_t addend = 0; };
struct Global {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool writable = false;
    unsigned alignment = 8;
    std::vector<Relocation> relocations;
};
struct Module {
    std::vector<Function> functions;
    std::vector<External> externals;
    std::vector<Global> globals;
    bool instrument_budget = false;
    bool runtime_free_entry = false;
    bool runtime_free_library = false;
    bool runtime_free() const { return runtime_free_entry || runtime_free_library; }
    std::string runtime_requirement;
};
struct MethodEnvironment;
struct EditData {
    std::shared_ptr<const MethodEnvironment> methods;
    struct Slot {std::string symbol; std::uint64_t bits; bool emitted=false;};
    std::unordered_map<const Expr*,Slot> slots;
    struct Site {std::string symbol; int column,length; std::size_t capacity;};
    std::unordered_map<std::string,std::unordered_map<int,std::vector<Site>>> sites;
};
struct LowerOptions {
    bool instrument_budget = false;
    // Embedding may explicitly replace standard I/O with C++ streams. Ordinary
    // Windows AOT/JIT console calls go directly to the operating system.
    bool custom_input = false;
    bool custom_output = false;
    bool optimize_loops = false;
    bool standalone_executable = false;
    // A closed executable may use the OS ABI even when no Bolt runtime is installed.
    bool try_runtime_free_entry = false;
    bool source_core = false;
    EditData* edit_data = nullptr;
};
Module lower(const Program&, LowerOptions = {});
Module lower_methods(const Program&,const Class&,const std::vector<Method*>&,const MethodEnvironment&);
void verify(const Module&);
std::string dump(const Module&);
std::string llvm_text(const Module&, const std::string& triple, bool debug, bool release = false);
} // namespace bolt::ir

namespace bolt::native_backend {
struct Options {
    bool release = false;
    bool debug_symbols = true;
    std::string llvm_library;
    std::string target; // Empty means host. Linux AOT also supports the other 64-bit GNU/Linux target.
};
struct Symbol { const char* name; void* address; };
struct JitImage {
    virtual ~JitImage() = default;
    virtual void* symbol(const std::string&) const = 0;
};
// The deployment target compiled into the toolchain; used by LLVM and both link paths.
std::string macos_deployment_target();
std::string host_target();
std::string aot_target(const std::string& requested);
std::string llvm_version(const Options& = {});
std::vector<std::uint8_t> llvm_object(const ir::Module&, const Options& = {});
std::unique_ptr<JitImage> llvm_jit(const ir::Module&, const std::vector<Symbol>&, const Options& = {});
std::vector<std::uint8_t> fast_object(const ir::Module&, const Options& = {});
std::unique_ptr<JitImage> fast_jit(const ir::Module&, const std::vector<Symbol>&, const Options& = {});
struct FastRecipe {
    virtual ~FastRecipe() = default;
    virtual std::unique_ptr<JitImage> link() const = 0;
    virtual std::unique_ptr<JitImage> link(const ir::Module& methods) const = 0;
};
std::shared_ptr<const FastRecipe> fast_recipe(const ir::Module&,const std::vector<Symbol>&,const Options&);
bool fast_supported();
bool fast_target_supported(const std::string& target);
} // namespace bolt::native_backend
