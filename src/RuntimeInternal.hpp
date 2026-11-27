#pragma once

#include "ExecutionBudget.hpp"
#include <bolt/API.hpp>
#include <bolt/Frontend.hpp>
#include <bolt/native/Jit.hpp>
#include <filesystem>
#include <iosfwd>
#include <string_view>

struct BrContext;

namespace bolt::detail {
int execute_program_nested(std::string_view,ExecutionOptions,const PreprocessorOptions&,
                           std::istream&,std::ostream&,SharedExecutionBudget);
int execute_statements_nested(std::string_view,ExecutionOptions,const PreprocessorOptions&,
                              std::istream&,std::ostream&,SharedExecutionBudget);
int execute_file_nested(const std::filesystem::path&,ExecutionOptions,const PreprocessorOptions&,
                        std::istream&,std::ostream&,SharedExecutionBudget);
int jit_program_nested(std::string_view,JitOptions,SharedExecutionBudget);
int jit_statements_nested(std::string_view,JitOptions,SharedExecutionBudget);
int jit_file_nested(const std::filesystem::path&,JitOptions,SharedExecutionBudget);
}

namespace bolt::native_abi {
void configure_execution(BrContext*,const ExecutionOptions&,detail::SharedExecutionBudget={});
void reset_execution(BrContext*) noexcept;
}
