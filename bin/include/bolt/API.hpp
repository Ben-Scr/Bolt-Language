#pragma once
#include "Ast.hpp"
#include "Value.hpp"
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string_view>
#include <string>
#include <vector>

namespace bolt {
struct ExecutionOptions {
    // A zero step budget disables that limit. Not an OS security sandbox.
    std::size_t max_steps = 1'000'000;
    std::size_t max_call_depth = 128;
    std::size_t max_runtime_depth = 16;
};
class Runtime {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    // Pass an analyzed Program produced by load() or compile_source().
    explicit Runtime(Program program, ExecutionOptions options = {},
                     std::istream* input = nullptr, std::ostream* output = nullptr);
    ~Runtime();
    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    int run();
    // Command-line arguments exclude argv[0]; only Main(string[] args) receives them.
    int run(const std::vector<std::string>& arguments);
    // Calls public static methods. Static fields persist between calls.
    Value call(const std::string& class_name, const std::string& method_name,
               std::vector<Value> arguments = {});
    Value create(const std::string& class_name);
    // Instance calls mutate the supplied value, not an implicit shared object.
    Value call(Value& instance, const std::string& method_name, std::vector<Value> arguments = {});
};
int execute_program(std::string_view source, ExecutionOptions options = {}, const PreprocessorOptions& preprocessing = {});
int execute_statements(std::string_view source, ExecutionOptions options = {}, const PreprocessorOptions& preprocessing = {});
int execute_file(const std::filesystem::path& path, ExecutionOptions options = {}, const PreprocessorOptions& preprocessing = {});
} // namespace bolt
