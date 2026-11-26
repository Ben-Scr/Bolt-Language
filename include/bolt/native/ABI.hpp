#pragma once
// Stable C ABI between generated Bolt machine code and the prebuilt runtime.
// ABI v2 targets little-endian 64-bit Windows, Linux and macOS. User class values
// are laid out by the Bolt frontend; no std::string/STL object crosses this ABI.
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <bolt/native/IR.hpp>
#include <bolt/API.hpp>
#if defined(BOLT_WINDOWS_AOT_RUNTIME)
#define BOLT_NATIVE_EXPORT
#elif defined(_WIN32)
#define BOLT_NATIVE_EXPORT __declspec(dllexport)
#else
#define BOLT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

// A persistent JIT context starts with an exclusive instruction counter. Clear
// this flag before publishing any worker that may debit its parent's budget.
inline constexpr std::uint32_t BR_PRIVATE_STEPS = 256u;
struct BrContext {
    static constexpr std::uint32_t Avx2 = 1u << 1;
    static constexpr std::uint32_t Avx512F = 1u << 2;
    std::int32_t failed;
    std::uint32_t flags;
    std::uint64_t steps;
    std::uint32_t depth;
    std::uint32_t max_depth;
    void* implementation;
    std::uint64_t active_ranges = 0;
};
// Array and Buffer slots hold either nullptr or a pointer to this prefix.
struct BrSequenceHeader {
    std::byte* data = nullptr;
    std::size_t count = 0;
    std::size_t capacity = 0;
};
static_assert(std::is_standard_layout_v<BrSequenceHeader>);
static_assert(sizeof(void*) == 8 && sizeof(std::size_t) == 8);

// A non-null string slot points to this immutable prefix. The UTF-8 byte
// range belongs to the string. A null slot denotes the empty string.
struct BrTextHeader {
    const char* data;
    std::size_t size;
};
static_assert(std::is_standard_layout_v<BrTextHeader>);
static_assert(sizeof(BrTextHeader) == 16 && offsetof(BrTextHeader, data) == 0 && offsetof(BrTextHeader, size) == 8);

// Borrowed only within compiler-proven read-only regions. The originating
// string owns the immutable scalar array for the entire borrow.
struct BrTextView {
    const char32_t* data = nullptr;
    std::size_t count = 0;
};
static_assert(std::is_standard_layout_v<BrTextView>);

struct BrType;
namespace bolt::native_abi {
inline constexpr std::uint32_t Version=0x00100009;
enum ReflectedFlags : std::uint32_t { ReadOnlyField=1, IgnoreJson=2, PropertyField=4, SnapshotGetter=8 };
enum class SerialMethod { Export, Import, Count, At, Add, Key, Value, NumberOfMethods };
enum Kind : std::int32_t { Signed=1,Unsigned,Float,Bool,String,Pointer,Class,Array,List,HashSet,Dictionary,Queue,Interface,Arena,LibraryValue,Range,Optional,ErrorState,Struct,SharedResource=23 };
constexpr bool scalar_storage_kind(std::int32_t kind) noexcept {
    return kind == Signed || kind == Unsigned || kind == Float || kind == Bool || kind == Pointer;
}
std::vector<native_backend::Symbol> symbols();
void rethrow(BrContext*);
void arguments(BrContext*,const std::vector<std::string>&);
void streams(BrContext*,std::istream*,std::ostream*);
}
extern "C" {
BOLT_NATIVE_EXPORT void br_destructor(BrContext*,BrType*,void*,std::uint64_t) noexcept;
BOLT_NATIVE_EXPORT void br_new_failed(BrContext*,BrType*,void*) noexcept;
BOLT_NATIVE_EXPORT BrContext* br_context_child(BrContext*) noexcept;
BOLT_NATIVE_EXPORT BrContext* br_context_activate(BrContext*) noexcept;
BOLT_NATIVE_EXPORT void* br_context_registry(BrContext*) noexcept;
BOLT_NATIVE_EXPORT std::uint64_t br_type_size(BrType*) noexcept;
BOLT_NATIVE_EXPORT void br_context_cleanup(BrContext*,void*) noexcept;
BOLT_NATIVE_EXPORT void br_shared_hooks(BrContext*,BrType*,void*,void*) noexcept;
BOLT_NATIVE_EXPORT void br_base(BrContext*,BrType*,BrType*) noexcept;
BOLT_NATIVE_EXPORT void br_raise(BrContext*,const char*,BrType*,const void*) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_exception_match(BrContext*,BrType*) noexcept;
BOLT_NATIVE_EXPORT void* br_catch_begin(BrContext*,const char*) noexcept;
BOLT_NATIVE_EXPORT void br_catch_end(BrContext*) noexcept;
BOLT_NATIVE_EXPORT void br_rethrow(BrContext*,const char*) noexcept;
BOLT_NATIVE_EXPORT void br_error_suspend(BrContext*,void*) noexcept;
BOLT_NATIVE_EXPORT void br_error_restore(BrContext*,void*) noexcept;

BOLT_NATIVE_EXPORT void br_reflected_type(BrContext*,BrType*,const char* space,const char* attributes) noexcept;
BOLT_NATIVE_EXPORT void br_reflected_field(BrContext*,BrType*,const char* name,const char* json_name,const char* attributes,
    BrType* value,std::uint64_t offset,std::uint32_t flags,void* getter,void* setter) noexcept;
BOLT_NATIVE_EXPORT void br_reflected_method(BrContext*,BrType*,const char* name,const char* result,const char* parameters,
    const char* signature,const char* attributes,std::uint32_t is_static,void* method) noexcept;
BOLT_NATIVE_EXPORT void br_serialization_method(BrContext*,BrType*,std::uint32_t slot,BrType* value,void* method) noexcept;
BOLT_NATIVE_EXPORT void br_reflection(BrContext*,const char*,const char* operation,BrType* owner,BrType* result,void* output,
    void* object,const void* name,BrType* value,const char* signature,void** args) noexcept;
BOLT_NATIVE_EXPORT void br_tree_object(BrContext*,const char*,BrType* value,void* object,BrType* tree_type,void* tree,std::int32_t encode) noexcept;
BOLT_NATIVE_EXPORT std::uint64_t br_library_schema() noexcept;
BOLT_NATIVE_EXPORT void br_process_arguments(BrContext*,std::int32_t count,const char* const* args) noexcept;
BOLT_NATIVE_EXPORT void br_arguments(BrContext*,const char* site,BrType*,void* output) noexcept;
BOLT_NATIVE_EXPORT void br_range(BrContext*,const char* site,BrType*,void* source,BrType* element,void* output) noexcept;
BOLT_NATIVE_EXPORT void* br_range_next(BrContext*,const char* site,void* range) noexcept;
BOLT_NATIVE_EXPORT std::uint32_t br_abi_version() noexcept;
BOLT_NATIVE_EXPORT void br_library(BrContext*,const char* site,std::int32_t operation,BrType* result,void* output,void* self,BrType* const* types,void* const* args,std::uint64_t count) noexcept;
BOLT_NATIVE_EXPORT BrContext* br_create(std::uint64_t steps,std::uint32_t depth) noexcept;
BOLT_NATIVE_EXPORT void br_destroy(BrContext*) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_finish(BrContext*,std::int32_t status) noexcept;
BOLT_NATIVE_EXPORT void br_process_exit(std::int32_t status) noexcept;
BOLT_NATIVE_EXPORT void br_fail(BrContext*,const char* site,const char* message) noexcept;
BOLT_NATIVE_EXPORT void br_frame(BrContext*,const char* site,const char* method) noexcept;
BOLT_NATIVE_EXPORT void br_tick(BrContext*,const char* site) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_enter(BrContext*,const char* site) noexcept;
BOLT_NATIVE_EXPORT void br_leave(BrContext*,std::int32_t entered) noexcept;
// The executable owns type descriptors and static storage for its project graph.
BOLT_NATIVE_EXPORT void br_project_bind(BrContext*,const char* key,void* address) noexcept;
BOLT_NATIVE_EXPORT void* br_project_lookup(BrContext*,const char* key) noexcept;
BOLT_NATIVE_EXPORT BrType* br_type(BrContext*,const char* name,std::int32_t kind,std::uint64_t size) noexcept;
BOLT_NATIVE_EXPORT void br_field(BrContext*,BrType*,std::uint64_t offset,BrType*) noexcept;
BOLT_NATIVE_EXPORT void br_elements(BrContext*,BrType*,BrType* key,BrType* value) noexcept;
BOLT_NATIVE_EXPORT void br_initializer(BrContext*,BrType*,void* function) noexcept;
BOLT_NATIVE_EXPORT void br_virtual(BrContext*,BrType*,std::int32_t slot,void* function) noexcept;
BOLT_NATIVE_EXPORT void* br_dispatch(BrContext*,const char* site,void* self,std::int32_t slot) noexcept;
BOLT_NATIVE_EXPORT void br_box(BrContext*,const char* site,BrType* target,void* dest,BrType* source,const void* value) noexcept;
BOLT_NATIVE_EXPORT void* br_unbox(BrContext*,const char* site,void* self) noexcept;
BOLT_NATIVE_EXPORT void br_value_contract(BrContext*,BrType*,void* equality,void* hash) noexcept;
BOLT_NATIVE_EXPORT void br_formatter(BrContext*,BrType*,void* function) noexcept;
BOLT_NATIVE_EXPORT void br_memory_copy(BrContext*, const char*, std::uint8_t*, const std::uint8_t*, std::uint64_t) noexcept;
BOLT_NATIVE_EXPORT void br_memory_move(BrContext*, const char*, std::uint8_t*, const std::uint8_t*, std::uint64_t) noexcept;
BOLT_NATIVE_EXPORT void br_memory_set(BrContext*, const char*, std::uint8_t*, std::uint8_t, std::uint64_t) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_memory_compare(BrContext*, const char*, const std::uint8_t*, const std::uint8_t*, std::uint64_t) noexcept;
BOLT_NATIVE_EXPORT void br_zero(void* address,std::uint64_t size) noexcept;
BOLT_NATIVE_EXPORT void br_drop(BrType*,void* address) noexcept;
BOLT_NATIVE_EXPORT void br_reset(BrType*,void* address) noexcept;
BOLT_NATIVE_EXPORT void br_copy(BrContext*,const char* site,BrType*,void* dest,const void* src) noexcept;
BOLT_NATIVE_EXPORT void* br_new(BrContext*,const char* site,BrType*) noexcept;
BOLT_NATIVE_EXPORT void br_arena_create(BrContext*,const char* site,void* slot,std::uint64_t initial,std::uint64_t maximum,std::int32_t grow) noexcept;
BOLT_NATIVE_EXPORT void br_arena_action(BrContext*,const char* site,void* slot,std::int32_t dispose) noexcept;
BOLT_NATIVE_EXPORT std::uint64_t br_arena_stat(BrContext*,const char* site,void* slot,std::int32_t property) noexcept;
BOLT_NATIVE_EXPORT void* br_arena_new(BrContext*,const char* site,void* slot,BrType*) noexcept;
BOLT_NATIVE_EXPORT void br_delete(BrContext*,const char* site,void*) noexcept;
BOLT_NATIVE_EXPORT void br_string(BrContext*,const char* site,void* dest,const char* bytes,std::uint64_t size) noexcept;
// Borrowed UTF-8 view, valid until the string is changed or destroyed. This is
// value storage access only; the generated caller owns all platform I/O.
BOLT_NATIVE_EXPORT std::uint32_t br_string_at(BrContext*,const char* site,const void* slot,std::int32_t index) noexcept;
// Replace one byte in a compiler-proven, non-escaping UTF-8 snapshot.
BOLT_NATIVE_EXPORT void br_edit_utf8_byte(BrContext*,const char* site,void* dest,std::uint64_t offset,std::uint8_t byte) noexcept;
// Compiler-proven ToUtf8 snapshot of dest, with all writes in [first, end).
BOLT_NATIVE_EXPORT void br_replace_utf8_edits(BrContext*,const char* site,void* dest,const void* array,
                                             std::uint64_t first,std::uint64_t end) noexcept;
BOLT_NATIVE_EXPORT const char* br_string_data(const void* slot,std::uint64_t* size) noexcept;
BOLT_NATIVE_EXPORT void br_text(BrContext*,const char* site,void* dest,BrType*,const void* value) noexcept;
BOLT_NATIVE_EXPORT void br_concat(BrContext*,const char* site,void* dest,const void* left,const void* right) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_compare(BrContext*,const char* site,BrType*,const void*,const void*,std::int32_t op) noexcept;
BOLT_NATIVE_EXPORT void br_write(BrContext*,const char* site,const void* text,std::int32_t newline) noexcept;
BOLT_NATIVE_EXPORT void br_read(BrContext*,const char* site,void* dest) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_capacity(BrContext*,const char* site,BrType*,const void*) noexcept;
BOLT_NATIVE_EXPORT std::uint64_t br_value_hash(BrContext*,const char* site,BrType*,const void*) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_count(BrContext*,const char* site,BrType*,const void* value) noexcept;
BOLT_NATIVE_EXPORT void br_cast(BrContext*,const char* site,BrType* target,void* dest,BrType* source,const void* value) noexcept;
BOLT_NATIVE_EXPORT void br_parse(BrContext*,const char* site,BrType* target,void* dest,const void* text) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_try_parse(BrContext*,const char* site,BrType* target,void* dest,const void* text) noexcept;
BOLT_NATIVE_EXPORT void br_array(BrContext*,const char* site,BrType*,void* dest,std::uint64_t size,std::int32_t initialize) noexcept;
BOLT_NATIVE_EXPORT void br_array_shape(BrContext*,const char* site,BrType*,void* dest,const std::uint64_t* dimensions,std::uint64_t rank,std::int32_t initialize) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_array_length(BrContext*,const char* site,BrType*,const void* container,std::int64_t dimension) noexcept;
BOLT_NATIVE_EXPORT void* br_array_index(BrContext*,const char* site,BrType*,void* container,const std::int64_t* indices,std::uint64_t rank) noexcept;
BOLT_NATIVE_EXPORT void* br_index(BrContext*,const char* site,BrType*,void* container,std::int64_t index) noexcept;
BOLT_NATIVE_EXPORT void* br_key(BrContext*,const char* site,BrType*,void* container,const void* key,std::int32_t create) noexcept;
BOLT_NATIVE_EXPORT void br_buffer_append(BrContext*,const char* site,BrType*,void* buffer,const void* array,std::int32_t offset,std::int32_t count) noexcept;
BOLT_NATIVE_EXPORT void br_collection(BrContext*,const char* site,BrType*,void* container,const char* operation,void* result,void* a,void* b) noexcept;
BOLT_NATIVE_EXPORT std::int32_t br_runtime(BrContext*,const char* site,std::int32_t mode,const void* source) noexcept;
}
