#pragma once
#include <bolt/Ast.hpp>
#include <string>
#include <vector>

namespace bolt::native_abi {
// Compile-time capability manifest and matching precompiled runtime operation IDs.
// No user C++ translation unit is needed for these operations.
bool library_pair(const bolt::Class&);
bool library_type_supported(const bolt::Class&);
bool library_primitive_method(const bolt::Class&,const Method&);
std::string library_method_key(const bolt::Class&,const Method&);
std::string library_property_key(const bolt::Class&,const Field&);
std::uint64_t library_schema() noexcept;
int library_operation(const std::string& key);
std::size_t library_storage_size(const std::string& implementation);
std::size_t library_storage_size(const std::string& implementation,const PreprocessorOptions& target);
// Empty when a fixed external C signature can be called by the native ABI.
std::string direct_extern_requirement(const Program&,const Method&);
std::string direct_requirement(const Program&);
}
