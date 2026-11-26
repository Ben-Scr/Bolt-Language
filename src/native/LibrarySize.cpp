#include "LibraryStorage.hpp"
#include <bolt/ValueTree.hpp>
#include <bolt/native/Library.hpp>
#include <bolt/Buffer.hpp>
#include <bolt/Platform.hpp>
#include <bolt/SystemDiagnostics.hpp>
#include <bolt/SystemMemory.hpp>
#include <bolt/SystemNetworking.hpp>
#include <bolt/SystemProcess.hpp>
#include <bolt/library/Converter.hpp>
#include <bolt/library/IO.hpp>
#include <bolt/library/Process.hpp>
#include <bolt/library/Strings.hpp>
#include <bolt/library/Time.hpp>
#include <cstddef>

// PlatformDiagnostics.bolt reads these fields directly through the target C ABI.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
static_assert(sizeof(CONTEXT) <= 2048 && offsetof(CONTEXT, Rip) == 31 * sizeof(std::uint64_t));
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
static_assert(sizeof(CONTEXT) <= 2048 && offsetof(CONTEXT, Pc) == 33 * sizeof(std::uint64_t));
#elif defined(__FreeBSD__) && defined(__LP64__)
static_assert(offsetof(struct kinfo_proc, ki_flag) == 368 && P_TRACED == 0x800);
#elif defined(__APPLE__)
static_assert(offsetof(struct kinfo_proc, kp_proc.p_flag) == (sizeof(void*) == 8 ? 32 : 16) && P_TRACED == 0x800);
#endif

namespace bolt::native_abi {
std::size_t library_storage_size(const std::string& name) {
#define BOLT_LIBRARY_TYPE(Type) if(name==#Type)return library_size<Type>();
#include "LibraryOps.inc"
#undef BOLT_LIBRARY_TYPE
    throw std::runtime_error("unregistered native library type: "+name);
}
std::size_t library_storage_size(const std::string& name,const PreprocessorOptions& target) {
    if(target.platform!="Android" && target.platform!="IOS" && target.platform!="MacOS")return library_storage_size(name);
    const bool small=target.architecture=="ARM32" || target.architecture=="X86";
    // The desktop frontend also answers sizeof for a hosted target before
    // invoking its C++ compiler. Verify those values when building for that ABI.
#if defined(__ANDROID__) || defined(__APPLE__)
#define BOLT_MOBILE_LIBRARY(Type,small_size,large_size) \
    static_assert(library_size<Type>() == (sizeof(void*)==4 ? small_size : large_size)); \
    if(name==#Type)return small ? small_size : large_size;
#else
#define BOLT_MOBILE_LIBRARY(Type,small_size,large_size) if(name==#Type)return small ? small_size : large_size;
#endif
#include "MobileLibrarySizes.inc"
#undef BOLT_MOBILE_LIBRARY
    throw std::runtime_error("unregistered mobile library type: "+name);
}

}
