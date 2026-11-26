#include <bolt/CompilerHost.h>
#include <bolt/Frontend.hpp>
#include <bolt/Version.hpp>
#include <bolt/Path.hpp>
#include <bolt/native/Jit.hpp>

#include "IncrementalFrontend.hpp"
#include "HostLayouts.hpp"
#include "FastEditTemplate.hpp"
#include <bolt/Tooling.hpp>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
// An identity token outlives a destroyed compiler while a module still exists.
struct Owner {};
using Image = std::shared_ptr<bolt::NativeProgram>;

void diagnostic(char *out, size_t capacity, std::string_view message) noexcept {
    if (!out || !capacity) return;
    auto length = std::min(capacity - 1, message.size());
    while (length && length < message.size() &&
           (static_cast<unsigned char>(message[length]) & 0xC0) == 0x80) --length;
    if (length) std::memcpy(out, message.data(), length);
    out[length] = '\0';
}

template<class Operation>
int invoke(char *error, size_t capacity, Operation operation) noexcept {
    diagnostic(error, capacity, {});
    try {
        return operation();
    } catch (const bolt::Error& e) {
        try {
            diagnostic(error, capacity, bolt::format_diagnostic(e.diagnostic));
        } catch (...) {
            diagnostic(error, capacity, e.what());
        }
    } catch (const std::exception& e) {
        diagnostic(error, capacity, e.what());
    } catch (...) {
        diagnostic(error, capacity, "Unknown Bolt compiler error");
    }
    return BOLT_HOST_ERROR;
}
}

using LayoutTable = std::shared_ptr<const std::unordered_map<std::string,bolt::host::Layout>>;

struct BoltCompiler {
    std::unique_ptr<bolt::host::FastEditTemplate> edit_template;
    LayoutTable edit_layouts;
    std::shared_ptr<Owner> owner = std::make_shared<Owner>();
    bolt::JitOptions options;
    bolt::HostFunctions bindings;
    std::mutex mutex;
    Image active;
    std::mutex compile_mutex;
    bolt::ParseCache cache;
    bolt::IncrementalFrontend frontend;
};

struct BoltCompiledModule {
    std::shared_ptr<Owner> owner;
    Image image;
    BoltCompileStats stats;
    LayoutTable layouts;
    std::size_t incremental_methods=0;
};

using Clock = std::chrono::steady_clock;
static uint64_t nanoseconds(Clock::duration duration) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

static void validate_host_signatures(const bolt::Program& program, const BoltCompiler& compiler) {
    for (const auto& owner : program.classes) {
        for (const auto& method : owner.methods) {
            if (method.native_abi.empty()) continue;
            const auto& name = method.native_name.empty() ? method.name : method.native_name;
            const bool pointer = compiler.options.pointer_bindings.contains(name);
            const bool submit = compiler.options.submit_bindings.contains(name);
            if (!pointer && !submit && !compiler.bindings.contains(name)) continue;
            const auto& args = method.parameters;
            if (method.native_abi != "C" || method.type != (pointer ? "void*" : "ulong")
                || args.size() != (submit ? 3u : 2u) || args[0].type != "ulong"
                || args[1].type != (submit ? "void*" : "ulong") || (submit && args[2].type != "void*"))
                throw bolt::Error(method.token.at, "Declaration does not match registered host function: " + name, "BLT4001");
        }
    }
}

template<class Frontend, class Consumer>
static void with_program(BoltCompiler& compiler, Frontend frontend, Consumer consume) {
    bolt::ParseCacheScope use_cache(compiler.cache);
    const auto start = Clock::now();
    auto& program = compiler.frontend.check(frontend(), compiler.cache);
    validate_host_signatures(program, compiler);
    const auto analyzed = Clock::now();
    BoltCompileStats stats{compiler.cache.parsed, compiler.cache.reused, nanoseconds(analyzed - start), 0};
    consume(program, stats);
}

template<class Frontend>
static void compile_program(BoltCompiler& compiler, Frontend frontend, BoltCompiledModule **out) {
    std::lock_guard lock(compiler.compile_mutex);
    with_program(compiler, frontend, [&](auto& program, BoltCompileStats stats) {
        const auto start = Clock::now();
        auto image = std::make_shared<bolt::NativeProgram>(program, compiler.options, compiler.bindings);
        stats.native_codegen_ns = nanoseconds(Clock::now() - start);
        *out = new BoltCompiledModule{compiler.owner, std::move(image), stats, std::make_shared<const std::unordered_map<std::string,bolt::host::Layout>>(bolt::host::layouts(program))};
    });
}

extern "C" int bolt_compiler_check_project(BoltCompiler *compiler, const char *path,
                                           BoltCompileStats *stats, char *error, size_t capacity) {
    if (stats) *stats = {};
    return invoke(error, capacity, [&] {
        if (!compiler || !path || !*path) throw std::invalid_argument("Invalid project path or compiler handle");
        std::lock_guard lock(compiler->compile_mutex);
        with_program(*compiler, [&] {return bolt::load_project(bolt::path_from_utf8(path), {}, true, false,
                     compiler->options.preprocessing);}, [&](auto&, const BoltCompileStats& info) {
            if (stats) *stats = info;
        });
        return BOLT_HOST_OK;
    });
}

extern "C" size_t bolt_module_incremental_methods(const BoltCompiledModule* module) {
    return module?module->incremental_methods:0;
}

extern "C" int bolt_module_stats(const BoltCompiledModule *module, BoltCompileStats *out) {
    if (!module || !out) return BOLT_HOST_ERROR;
    *out = module->stats;
    return BOLT_HOST_OK;
}

extern "C" const char *bolt_compiler_version(void) { return BOLT_VERSION_STRING; }
extern "C" uint32_t bolt_compiler_host_abi(void) { return BOLT_COMPILER_HOST_ABI; }

extern "C" int bolt_compiler_create(uint32_t abi, int profile, const BoltHostBinding *bindings,
                                    size_t count, BoltCompiler **out, char *error, size_t capacity) {
    return bolt_compiler_create_with_llvm(abi, profile, bindings, count, nullptr, out, error, capacity);
}

extern "C" int bolt_compiler_create_with_llvm(uint32_t abi, int profile, const BoltHostBinding *bindings,
    size_t count, const char *llvm_library, BoltCompiler **out, char *error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] {
        if (!out || (count && !bindings)) throw std::invalid_argument("Invalid host binding table or output");
        if (abi != BOLT_COMPILER_HOST_ABI) throw std::invalid_argument("Unsupported Bolt compiler host ABI");
        if (profile != BOLT_HOST_DEBUG && profile != BOLT_HOST_RELEASE) throw std::invalid_argument("Unknown build profile");
        auto compiler = std::make_unique<BoltCompiler>();
        compiler->options.release = profile == BOLT_HOST_RELEASE;
        if (llvm_library) compiler->options.llvm_library = llvm_library;
        compiler->options.preprocessing.configuration = compiler->options.release ? "Release" : "Debug";
        for (size_t i = 0; i < count; ++i) {
            const auto& value = bindings[i];
            if (!value.name || !*value.name || !value.function)
                throw std::invalid_argument("Empty host binding");
            if (!compiler->bindings.emplace(value.name, value.function).second)
                throw std::invalid_argument("Duplicate host binding");
        }
        *out = compiler.release();
        return BOLT_HOST_OK;
    });
}

extern "C" int bolt_compiler_bind_submit(BoltCompiler *compiler, const char *name,
    BoltHostSubmitFunction function, char *error, size_t capacity) {
    return invoke(error, capacity, [&] {
        if (!compiler || !name || !*name || !function) throw std::invalid_argument("Invalid submission binding");
        std::lock_guard lock(compiler->compile_mutex);
        if (compiler->bindings.contains(name) || compiler->options.pointer_bindings.contains(name)) throw std::invalid_argument("Conflicting host binding");
        const auto [it, inserted] = compiler->options.submit_bindings.emplace(name, function);
        if (inserted) compiler->edit_template.reset();
        if (!inserted && it->second != function) throw std::invalid_argument("Conflicting submission binding");
        return BOLT_HOST_OK;
    });
}
extern "C" int bolt_compiler_bind_pointer(BoltCompiler *compiler, const char *name,
    BoltHostPointerFunction function, char *error, size_t capacity) {
    return invoke(error, capacity, [&] {
        if (!compiler || !name || !*name || !function) throw std::invalid_argument("Invalid pointer binding");
        std::lock_guard lock(compiler->compile_mutex);
        if (compiler->bindings.contains(name) || compiler->options.submit_bindings.contains(name)) throw std::invalid_argument("Conflicting host binding");
        const auto [it, inserted] = compiler->options.pointer_bindings.emplace(name, function);
        if (inserted) compiler->edit_template.reset();
        if (!inserted && it->second != function) throw std::invalid_argument("Conflicting pointer binding");
        return BOLT_HOST_OK;
    });
}
extern "C" void bolt_compiler_destroy(BoltCompiler *compiler) { delete compiler; }
extern "C" int bolt_module_visit_layout(const BoltCompiledModule* module,const char* type,
    BoltLayoutVisitor visit,void* user,char* error,size_t capacity) {
    return invoke(error,capacity,[&]{
        if(!module || !type || !visit)throw std::invalid_argument("Missing struct or layout visitor");
        auto it=module->layouts->find(type);
        if(it==module->layouts->end())throw std::invalid_argument("No native ABI layout for "+std::string(type));
        const auto& layout=it->second;
        if(!layout.error.empty())throw std::invalid_argument(layout.error);
        std::vector<BoltLayoutField> fields;
        for(const auto& f:layout.fields)fields.push_back({f.name.c_str(),f.type.c_str(),f.offset,f.size,f.visible});
        visit(user,layout.size,layout.alignment,fields.data(),fields.size());
        return BOLT_HOST_OK;
    });
}
extern "C" void bolt_module_release(BoltCompiledModule *module) { delete module; }

template<class Frontend>
static void compile_memory(BoltCompiler* compiler,const BoltSource* sources,std::size_t count,const char* entry,
                           BoltCompiledModule** out,Frontend parse) {
    if(compiler->options.release || !bolt::native_backend::fast_supported()) {
        compile_program(*compiler,parse,out);
        return;
    }
    std::lock_guard lock(compiler->compile_mutex);
    BoltCompileStats info{};
    if(compiler->edit_template) {
        std::size_t methods=0;
        if(auto image=compiler->edit_template->instantiate(compiler->options,sources,count,entry,info,methods)) {
            *out=new BoltCompiledModule{compiler->owner,std::move(image),info,compiler->edit_layouts,methods};
            return;
        }
    }
    with_program(*compiler,parse,[&](auto& program,BoltCompileStats stats) {
        const auto start=Clock::now();
        auto candidate=std::make_unique<bolt::host::FastEditTemplate>(program,sources,count,stats.parsed_files+stats.reused_files);
        Image image;
        try {
            image=candidate->compile(program,compiler->options,compiler->bindings,compiler->cache);
        } catch(const std::runtime_error&) {
            candidate.reset();
            image=std::make_shared<bolt::NativeProgram>(program,compiler->options,compiler->bindings);
        }
        auto layouts=std::make_shared<const std::unordered_map<std::string,bolt::host::Layout>>(bolt::host::layouts(program));
        stats.native_codegen_ns=nanoseconds(Clock::now()-start);
        *out=new BoltCompiledModule{compiler->owner,std::move(image),stats,layouts};
        if(candidate && candidate->accepts_methods()) candidate->retain(compiler->frontend.take_checked());
        compiler->edit_template=std::move(candidate);
        compiler->edit_layouts=std::move(layouts);
    });
}

extern "C" int bolt_compiler_compile(BoltCompiler *compiler, const char *name, const char *source,
                                     size_t length, BoltCompiledModule **out, char *error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] {
        if (!compiler || !out || !name || !source) throw std::invalid_argument("Invalid source or compiler handle");
        const BoltSource unit{name,source,length};
        compile_memory(compiler,&unit,1,nullptr,out,[&] {
            return bolt::parse_memory_source({source,length},name,compiler->options.preprocessing);
        });
        return BOLT_HOST_OK;
    });
}

extern "C" int bolt_compiler_visit_fields(const BoltSource *source, int profile, BoltFieldVisitor visit,
    void *user, char *error, size_t capacity) {
    return invoke(error, capacity, [&] {
        if (!source || !source->name || !source->source || !visit ||
            (profile != BOLT_HOST_DEBUG && profile != BOLT_HOST_RELEASE))
            throw std::invalid_argument("Invalid field visitor or source");
        bolt::PreprocessorOptions options;
        options.configuration = profile == BOLT_HOST_RELEASE ? "Release" : "Debug";
        auto program = bolt::parse_memory_source({source->source, source->length}, source->name, options);
        for (const auto& owner : program.classes) for (const auto& field : owner.fields) {
            std::vector<std::vector<const char*>> arguments(field.attributes.size());
            std::vector<BoltAttributeInfo> attributes;
            for (size_t i = 0; i < field.attributes.size(); ++i) {
                const auto& attr = field.attributes[i];
                for (const auto& arg : attr.arguments) arguments[i].push_back(arg.c_str());
                attributes.push_back({attr.name.c_str(), arguments[i].data(), arguments[i].size()});
            }
            std::string literal;
            const auto* value = field.initializer.get();
            if (value && value->kind == bolt::Expr::Kind::Literal) literal = value->token.text;
            else if (value && value->kind == bolt::Expr::Kind::Unary && value->text == "-" &&
                     value->args.size() == 1 && value->args[0]->kind == bolt::Expr::Kind::Literal)
                literal = "-" + value->args[0]->token.text;
            BoltFieldInfo info{owner.name.c_str(), field.name.c_str(), field.type.c_str(), literal.c_str(),
                field.is_public, field.is_static, field.constant, attributes.data(), attributes.size()};
            visit(user, &info);
        }
        return BOLT_HOST_OK;
    });
}

extern "C" int bolt_compiler_compile_sources(BoltCompiler *compiler, const BoltSource *sources,
    size_t count, const char *entry, BoltCompiledModule **out, char *error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] {
        if (!compiler || !out || !count || !sources) throw std::invalid_argument("Missing source units");
        for(std::size_t i=0;i<count;++i)
            if(!sources[i].name || !*sources[i].name || !sources[i].source) throw std::invalid_argument("Invalid source unit");
        auto parse=[&] {
            bolt::Program program;
            program.preprocessor=compiler->options.preprocessing;
            if(entry) program.selected_entry=entry;
            for(std::size_t i=0;i<count;++i)
                bolt::append_memory_source(program,{sources[i].source,sources[i].length},sources[i].name);
            return program;
        };
        compile_memory(compiler,sources,count,entry,out,parse);
        return BOLT_HOST_OK;
    });
}

extern "C" int bolt_compiler_compile_project(BoltCompiler *compiler, const char *path,
                                             BoltCompiledModule **out, char *error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] {
        if (!compiler || !out || !path || !*path) throw std::invalid_argument("Invalid project path or compiler handle");
        compile_program(*compiler, [&] {return bolt::load_project(bolt::path_from_utf8(path), {}, true, false,
                        compiler->options.preprocessing);}, out);
        return BOLT_HOST_OK;
    });
}

extern "C" int bolt_compiler_load(BoltCompiler *compiler, const BoltCompiledModule *module,
                                  char *error, size_t capacity) {
    return invoke(error, capacity, [&] {
        if (!compiler) throw std::invalid_argument("Missing compiler handle");
        if (module && module->owner != compiler->owner) throw std::invalid_argument("Module belongs to another compiler");
        Image replacement = module ? module->image : nullptr;
        std::lock_guard lock(compiler->mutex);
        compiler->active.swap(replacement);
        return BOLT_HOST_OK;
    });
}

static int run_module(BoltCompiler *compiler, int32_t *exit_code, char *error, size_t capacity, bool persistent) {
    return invoke(error, capacity, [&] {
        if (!compiler || !exit_code) throw std::invalid_argument("Missing compiler handle or exit code");
        Image current;
        {
            std::lock_guard lock(compiler->mutex);
            current = compiler->active;
        }
        if (!current) {
            diagnostic(error, capacity, "No module loaded");
            return BOLT_HOST_NO_MODULE;
        }
        *exit_code = persistent ? current->run_persistent() : current->run();
        return BOLT_HOST_OK;
    });
}

extern "C" int bolt_compiler_run(BoltCompiler *compiler, int32_t *exit_code, char *error, size_t capacity) {
    return run_module(compiler, exit_code, error, capacity, false);
}
extern "C" int bolt_compiler_run_persistent(BoltCompiler *compiler, int32_t *exit_code, char *error, size_t capacity) {
    return run_module(compiler, exit_code, error, capacity, true);
}
