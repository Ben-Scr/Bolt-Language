#include <bolt/native/ABI.hpp>
#include "NativeText.hpp"
#include "JitCall.hpp"
#include "../RuntimeInternal.hpp"
#include "ScalarSearch.hpp"
#include <bolt/native/Jit.hpp>
#include <bolt/native/Library.hpp>
#include "LibraryStorage.hpp"
#include <bolt/ValueTree.hpp>
#include <bolt/library/IO.hpp>
#include <bolt/library/Converter.hpp>
#include <bolt/library/Time.hpp>
#include <bolt/library/Strings.hpp>
#include <bolt/library/Process.hpp>
#include <bolt/SystemMemory.hpp>
#include <bolt/Buffer.hpp>
#include <bolt/SystemProcess.hpp>
#include <bolt/SystemNetworking.hpp>
#include <bolt/SystemDiagnostics.hpp>
#include <typeinfo>
#include <concepts>
#include <map>
#ifdef _WIN32
#include <shellapi.h>
#endif
#include <bolt/Frontend.hpp>
#include <bolt/Path.hpp>
#include <bolt/Std.hpp>
#include <bolt/Platform.hpp>
#include <bolt/SecureRandom.hpp>
#include <bolt/Arena.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <optional>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

std::uint64_t bolt::platform::Entropy::Seed() {
    std::uint64_t value;
    secure_random_bytes(reinterpret_cast<std::uint8_t*>(&value),sizeof(value));
    return value;
}

using MemberCall=void(*)(BrContext*,void*,void*,void**);
struct ReflectedField {
    std::string name,json_name;
    BrType* type;
    std::size_t offset;
    std::uint32_t flags;
    MemberCall get,set;
};
struct ReflectedMethod {std::string name,signature;MemberCall call;};
struct ReflectedType {
    bool reflected=false;
    bolt::library::TypeRecord info;
    std::vector<ReflectedField> fields;
    std::vector<ReflectedMethod> methods;
    using Slot=bolt::native_abi::SerialMethod;
    struct Adapter { BrType* type=nullptr;MemberCall call=nullptr; };
    std::array<Adapter,static_cast<unsigned>(Slot::NumberOfMethods)> serial{};
    Adapter method(Slot slot)const{return serial[static_cast<unsigned>(slot)];}
};
struct BrType {
    std::unique_ptr<ReflectedType> metadata;
    ReflectedType& get_metadata(){if(!metadata)metadata=std::make_unique<ReflectedType>();return *metadata;}
    std::string name;
    std::int32_t kind;
    std::size_t size;
    std::size_t rank=0;
    BrType* base=nullptr;
    std::vector<std::pair<std::size_t,BrType*>> fields;
    BrType* key=nullptr;
    BrType* value=nullptr;
    void (*initialize)(BrContext*,void*)=nullptr;
    void (*destructor)(BrContext*,void*)=nullptr;
    std::size_t marker=static_cast<std::size_t>(-1);
    std::vector<void*> methods;
    void (*format)(BrContext*,void*,void*)=nullptr;
    void (*retain_resource)(BrContext*,void*)=nullptr, (*release_resource)(BrContext*,void*)=nullptr;
    BrContext* context=nullptr;
    std::uint8_t (*equal_value)(BrContext*,const void*,const void*)=nullptr;
    std::uint64_t (*hash_value)(BrContext*,const void*)=nullptr;
};
namespace {
using namespace bolt::native_abi;
struct Box;
struct ExceptionRecord {
    std::optional<bolt::Diagnostic> diagnostic;
    std::string type_name="System.Exception";
    std::shared_ptr<Box> payload;
};
// Repeated large ToArray snapshots can otherwise fault the same number of
// pages back in on every iteration. Keep at most one block per execution.
struct SnapshotStorage {
    std::mutex mutex;
    std::byte* data = nullptr;
    std::size_t bytes = 0;

    ~SnapshotStorage() { std::free(data); }
    static bool cacheable(std::size_t size, std::size_t minimum) { return size >= minimum && size <= 1024*1024; }
    std::byte* take(std::size_t size) {
        std::lock_guard lock(mutex);
        auto* block = std::exchange(data,nullptr);
        if (bytes == size) return block;
        std::free(block);
        return nullptr;
    }
    void put(std::byte* block,std::size_t size) {
        std::lock_guard lock(mutex);
        std::free(std::exchange(data,block));
        bytes = size;
    }
};
struct Context {
    void (*drain)(BrContext*)=nullptr;
    alignas(8) std::uint64_t thread_registry[2]{};
    SnapshotStorage snapshot_storage;
    Context* type_owner=nullptr;
    bolt::ExecutionOptions options;
    bolt::detail::SharedExecutionBudget budget;
    bool owns_budget=true;
    std::optional<bolt::Diagnostic> error;
    std::string error_type="System.Exception";
    std::shared_ptr<Box> pending_exception;
    std::vector<ExceptionRecord> caught;

    std::map<std::string,void*,std::less<>> project_symbols;
    std::vector<std::unique_ptr<BrType>> types;
    std::unordered_set<const BrType*> type_addresses;
    // Workers borrow their parent execution's type registry and heap lifetime.
    // Entries are detached under the lock; user cleanup always runs after unlocking.
    std::mutex allocation_mutex;
    std::unordered_map<void*,BrType*> allocations;
    struct Epoch {std::uint64_t value=0;std::size_t readers=0;};
    std::unordered_map<const void*,Epoch> range_epochs;
    std::vector<std::string> arguments;
    std::istream* input=nullptr;
    std::ostream* output=nullptr;
};
thread_local BrContext* active_worker_context=nullptr;
Context& impl(BrContext* c){return *static_cast<Context*>(c->implementation);}
auto& registered_types(Context& c){return c.type_owner?c.type_owner->types:c.types;}
Context& allocation_owner(Context& c){return c.type_owner?*c.type_owner:c;}
BrType* remove_allocation(Context& c,void* pointer){
    auto& owner=allocation_owner(c);
    std::lock_guard lock(owner.allocation_mutex);
    const auto found=owner.allocations.find(pointer);
    if(found==owner.allocations.end())return nullptr;
    auto* type=found->second;owner.allocations.erase(found);return type;
}
bolt::Location location(const char* p,std::string* source_line=nullptr){
    bolt::Location at{"<native>",1,1};if(!p)return at;
    try{
        std::string s(p);std::size_t n=0;
        auto part=[&](){auto end=s.find(':',n);if(end==std::string::npos)throw std::runtime_error("bad source site");auto value=std::stoull(s.substr(n,end-n));n=end+1;if(value>static_cast<std::uint64_t>(std::numeric_limits<int>::max()))throw std::runtime_error("source site is too large");return static_cast<int>(value);};
        at.line=part();at.column=part();at.length=part();const auto file_length=static_cast<std::size_t>(part());
        at.file=s.substr(n,file_length);if(source_line)*source_line=s.substr(n+file_length);
    }catch(...){at.file=p;}
    return at;
}
void failure(BrContext* c,const char* site,const std::string& s)noexcept{
    if(!c||c->failed)return;
    c->failed=1;
    try {auto& state=impl(c);state.pending_exception.reset();state.error_type="System.Exception";
        if(s.find("division by zero")!=s.npos||s.find("remainder by zero")!=s.npos)state.error_type="System.DivideByZeroException";
        else if(s.find("overflow")!=s.npos||s.find("underflow")!=s.npos)state.error_type="System.OverflowException";
        else if(s.find("index")!=s.npos||s.find("bounds")!=s.npos)state.error_type="System.IndexOutOfRangeException";
        else if(s.find("null pointer")!=s.npos)state.error_type="System.NullReferenceException";
        else if(s.find("optional value is empty")!=s.npos)state.error_type="System.InvalidOperationException";
    }catch(...){}
    try{std::string source;auto at=location(site,&source);auto diagnostic=bolt::runtime_diagnostic(std::move(at),s);diagnostic.source_line=std::move(source);impl(c).error=std::move(diagnostic);}catch(...){}
}
void typed_failure(BrContext* c,const char* site,const bolt::library::Exception& e)noexcept {
    failure(c,site,e.what());try {impl(c).error_type="System."+std::string(e.TypeName());}catch(...){}
}
template<class F> void checked(BrContext* c,const char* site,F&& fn)noexcept{
    if(!c||c->failed)return;
    try{fn();}catch(const bolt::Error& e){c->failed=1;try{impl(c).error=e.diagnostic;}catch(...){}}
    catch(const bolt::library::Exception& e){typed_failure(c,site,e);}
    catch(const std::bad_alloc&){failure(c,site,"Out of memory.");try{impl(c).error_type="System.OutOfMemoryException";}catch(...){}}
    catch(const std::exception& e){failure(c,site,e.what());}catch(...){failure(c,site,"unknown native runtime failure");}
}
template<class T> T read(const void* p){T v{};if(p)std::memcpy(&v,p,sizeof(T));return v;}
template<class T> void write(void* p,const T& v){std::memcpy(p,&v,sizeof(T));}
void drop(BrType*,void*)noexcept;
void relocate(BrType*,void*,void*)noexcept;
void clone(BrType*,void*,const void*,bolt::memory::Arena*);
bool equal(BrType*,const void*,const void*);
std::size_t hash(BrType*,const void*);
// A library value is opaque to generated code, but is constructed *in place*.
// The first word is the common Bolt type descriptor, the second identifies
// precompiled operations; following bytes have a
// real C++ lifetime. All storage movement goes through relocate(), never a raw
// memcpy of a live std::shared_ptr or other non-trivial C++ object.
struct LibraryOps {
    std::size_t bytes;
    const std::type_info* identity;
    void (*destroy)(void*) noexcept;
    void (*move)(void*,void*) noexcept;
    void (*copy)(void*,const void*);
    bolt::library::Stream* (*stream)(void*) noexcept;
    std::string (*format)(const void*);
    bool (*equals)(const void*,const void*);
    std::size_t (*hash_value)(const void*);
    bolt::library::Exception* (*exception)(void*) noexcept;
    void (*make_exception)(void*,const std::string&,const bolt::Location&);

};
constexpr std::size_t library_offset=bolt::native_abi::library_value_offset;
const LibraryOps* slot_ops(const void* p){return read<const LibraryOps*>(static_cast<const std::byte*>(p)+8);}
void set_ops(void* p,const LibraryOps* ops){write(static_cast<std::byte*>(p)+8,ops);}
void* library_data(void* p){return static_cast<std::byte*>(p)+library_offset;}
const void* library_data(const void* p){return static_cast<const std::byte*>(p)+library_offset;}
template<class T> const LibraryOps* library_ops(){
    static_assert(alignof(T)<=8,"Update the native ABI alignment before exposing this type");
    static_assert(std::is_abstract_v<T> || std::is_nothrow_move_constructible_v<T>,"Library values need noexcept relocation");
    static const LibraryOps ops{
        bolt::native_abi::library_size<T>(),&typeid(T),
        [](void* p) noexcept {if constexpr(!std::is_abstract_v<T>)std::destroy_at(static_cast<T*>(p));},
        [](void* d,void* p) noexcept {
            if constexpr(!std::is_abstract_v<T>){std::construct_at(static_cast<T*>(d),std::move(*static_cast<T*>(p)));std::destroy_at(static_cast<T*>(p));}
        },
        [](void* d,const void* p){
            if constexpr(!std::is_abstract_v<T>)std::construct_at(static_cast<T*>(d),*static_cast<const T*>(p));
            else throw std::runtime_error("an abstract native library value cannot be copied by value");
        },
        [](void* p) noexcept -> bolt::library::Stream* {
            if constexpr(std::is_base_of_v<bolt::library::Stream,T>)return static_cast<T*>(p);
            else return nullptr;
        },
        [](const void* p)->std::string {
            if constexpr(requires(const T& v){v.ToString();}) {auto value=static_cast<const T*>(p)->ToString();return std::string(value.data(),value.size());}
            else throw std::runtime_error("this native library type has no string representation");
        },
        [](const void* a,const void* b)->bool {
            if constexpr(requires(const T& x,const T& y){{x==y}->std::convertible_to<bool>;})return *static_cast<const T*>(a)==*static_cast<const T*>(b);
            else if constexpr(requires(const T& v){v.Identity();})return static_cast<const T*>(a)->Identity()==static_cast<const T*>(b)->Identity();
            else throw std::runtime_error("this native library type does not support value equality");
        },
        [](const void* p)->std::size_t {
            if constexpr(requires(const T& v){v.BoltHash();})return static_cast<const T*>(p)->BoltHash();
            else if constexpr(requires(const T& v){std::hash<T>{}(v);})return std::hash<T>{}(*static_cast<const T*>(p));
            else if constexpr(requires(const T& v){v.Identity();})return std::hash<const void*>{}(static_cast<const T*>(p)->Identity());
            else return 0; // valid conservative hash for other opaque types
        }
        ,[](void* p) noexcept -> bolt::library::Exception* {if constexpr(std::is_base_of_v<bolt::library::Exception,T>)return static_cast<T*>(p);else return nullptr;}
        ,[](void* p,const std::string& message,const bolt::Location& at){if constexpr(std::is_base_of_v<bolt::library::Exception,T>){std::construct_at(static_cast<T*>(p),message);try{static_cast<T*>(p)->SetSource(at);}catch(...){std::destroy_at(static_cast<T*>(p));throw;}}else throw std::runtime_error("Not an exception payload type.");}
    };return &ops;
}
const LibraryOps* exception_ops(const std::string& name){
#define BOLT_LIBRARY_TYPE(Type) if constexpr(std::is_base_of_v<bolt::library::Exception,Type>) { if(name==#Type)return library_ops<Type>(); }
#include "LibraryOps.inc"
#undef BOLT_LIBRARY_TYPE
    throw std::runtime_error("unknown native library implementation: "+name);
}
template<class T> T& library_object(void* p){
    if(!p)throw std::runtime_error("null native library receiver");
    auto* ops=slot_ops(p);
    if(!ops)throw std::runtime_error("uninitialized native library receiver");
    if(*ops->identity==typeid(T))return *std::launder(static_cast<T*>(library_data(p)));
    if constexpr(std::is_same_v<T,bolt::library::Stream>)if(auto* stream=ops->stream(library_data(p)))return *stream;
    if constexpr(std::is_same_v<T,bolt::library::Exception>)if(auto* exception=ops->exception(library_data(p)))return *exception;
    throw std::runtime_error("native library receiver type mismatch");
}
struct Box {
    BrType* type=nullptr;
    // Avoid the Windows RPC macro `small`, which expands to `char`.
    alignas(8) std::array<std::byte,32> inline_storage{};
    std::byte* large=nullptr;
    bolt::memory::Arena* owner=nullptr;
    Box()=default;
    explicit Box(BrType* t,bolt::memory::Arena* a=nullptr):type(t),owner(a){if(t&&t->size>inline_storage.size()){large=static_cast<std::byte*>(bolt::memory::allocate_bytes(a,t->size));std::memset(large,0,t->size);}}
    Box(BrType* t,const void* p,bolt::memory::Arena* a=nullptr):Box(t,a){try{if(t)clone(t,data(),p,owner);}catch(...){drop(type,data());type=nullptr;bolt::memory::deallocate_bytes(large);large=nullptr;throw;}}
    Box(const Box& v):Box(v.type,v.data(),bolt::memory::owner_of(this)){}
    Box(Box&& v)noexcept:type(v.type),large(std::exchange(v.large,nullptr)),owner(v.owner){if(type&&!large)relocate(type,inline_storage.data(),v.inline_storage.data());v.type=nullptr;}
    Box& operator=(Box v)noexcept{swap(v);return *this;}
    ~Box(){if(type)drop(type,data());bolt::memory::deallocate_bytes(large);}
    void* data(){return large?large:inline_storage.data();}
    const void* data()const{return large?large:inline_storage.data();}
    void swap(Box& o)noexcept{
        if(this==&o)return;
        Box a(std::move(*this)),b(std::move(o));
        auto take=[](Box& d,Box& v){d.type=v.type;d.owner=v.owner;d.large=std::exchange(v.large,nullptr);if(d.type&&!d.large)relocate(d.type,d.inline_storage.data(),v.inline_storage.data());v.type=nullptr;};
        take(*this,b);take(o,a);
    }
};
// Interface storage is [static interface descriptor, owning Box*]. References
// may point directly at a concrete class instead; all interface operations accept both.
Box* interface_box(const void* p){return read<Box*>(static_cast<const std::byte*>(p)+8);}
struct CaptureBox : Box {
    std::atomic<std::size_t> references{1};
    CaptureBox(BrType* t,const void* value):Box(t,value){}
};
std::pair<BrType*,const void*> unwrapped(BrType* t,const void* p){
    if(t->kind==Interface||t->kind==Class||t->kind==LibraryValue){
        if(auto* dynamic=read<BrType*>(p))t=dynamic;
        if(t->kind==Interface){auto* value=interface_box(p);if(!value)throw std::runtime_error("uninitialized interface value");return {value->type,value->data()};}
    }
    return {t,p};
}
struct Hasher{std::size_t operator()(const Box& v)const{return hash(v.type,v.data());}};
struct Equal{bool operator()(const Box& a,const Box& b)const{return a.type==b.type&&equal(a.type,a.data(),b.data());}};
bool scalar_storage(const BrType* type) noexcept {
    return scalar_storage_kind(type->kind);
}
// Mirror clone/drop recursively. Struct padding is deliberately excluded:
// the normal field-wise clone leaves those bytes zero, independent of source padding.
bool bitwise_sequence_element(const BrType* t, unsigned depth = 0) noexcept {
    if (scalar_storage(t)) return true;
    if (t->kind != Struct || t->destructor || depth >= 64) return false;
    std::size_t covered = 0;
    for (const auto& [offset, field] : t->fields) {
        if (offset != covered || !bitwise_sequence_element(field, depth + 1)) return false;
        covered += field->size;
    }
    return covered == t->size;
}
bool needs_element_drop(const BrType* t, unsigned depth = 0) noexcept {
    if (scalar_storage(t)) return false;
    if (depth >= 64 || t->destructor) return true;
    if (t->kind == Optional) return !t->key || needs_element_drop(t->key, depth + 1);
    if (t->kind != Class && t->kind != Struct) return true;
    if (t->base && needs_element_drop(t->base, depth + 1)) return true;
    // Class field tables include the base fields already visited above.
    const auto inherited = t->kind == Class && t->base ? t->base->fields.size() : 0;
    for (auto i = inherited; i < t->fields.size(); ++i)
        if (needs_element_drop(t->fields[i].second, depth + 1)) return true;
    return false;
}
// Sequence elements are contiguous raw native values, not interpreter Values.
struct Sequence : BrSequenceHeader {
    BrType* element;
    const bool bitwise;
    bool recyclable_snapshot = false;
    bolt::memory::Arena* owner=nullptr;
    std::pmr::vector<std::size_t> dimensions;
    explicit Sequence(BrType* t,bolt::memory::Arena* a=nullptr):element(t),bitwise(bitwise_sequence_element(t)),owner(a),dimensions(bolt::memory::resource(a)){}
    ~Sequence() {
        clear(false);
        release_storage();
    }
    Sequence(const Sequence& o,bolt::memory::Arena* a):element(o.element),bitwise(o.bitwise),owner(a),dimensions(o.dimensions,bolt::memory::resource(a)){
        reserve(o.capacity);
        if (copy_trivial_from(o)) return;
        try { for (; count < o.count; ++count) clone(element, at(count), o.at(count), owner); }
        catch (...) {
            if (count < o.count) drop(element, at(count));
            clear();
            release_storage();
            throw;
        }
    }
    // Both callers own separate storage, reserve the full range and have no live elements here.
    bool copy_trivial_from(const Sequence& source) noexcept {
        if (!bitwise) return false;
        if (source.count) std::memcpy(data, source.data, source.count * element->size);
        count = source.count;
        return true;
    }
    void* at(std::size_t i){return data+i*element->size;}
    const void* at(std::size_t i)const{return data+i*element->size;}
    // Scalar heap blocks have ordinary malloc alignment. They never contain a
    // live C++ object and realloc can preserve the old block in place. Arena
    // and non-trivial element storage stay with their original allocator.
    bool uses_heap_realloc() const noexcept { return !owner && bitwise; }
    void release_storage() noexcept {
        if (uses_heap_realloc()) std::free(data);
        else bolt::memory::deallocate_bytes(data);
    }
    enum class Spare { Zero, Overwritten };
    void reserve(std::size_t n, Spare spare = Spare::Zero) {
        if (n <= capacity) return;
        if (element->size && n > std::numeric_limits<std::size_t>::max() / element->size)
            throw std::runtime_error("array allocation size overflow");
        const auto bytes = n * element->size;
        if (uses_heap_realloc()) {
            const bool initially_zero = !data && spare == Spare::Zero;
            auto* grown = static_cast<std::byte*>(initially_zero
                ? std::calloc(n, element->size) : std::realloc(data, bytes));
            if (!grown) throw std::bad_alloc();
            if (!initially_zero && spare == Spare::Zero) {
                const auto live = count * element->size;
                std::memset(grown + live, 0, bytes - live);
            }
            data = grown;
            capacity = n;
            return;
        }
        auto* replacement = static_cast<std::byte*>(bolt::memory::allocate_bytes(owner, bytes));
        if (bitwise) {
            const auto live = count * element->size;
            if (live) std::memcpy(replacement, data, live);
            if (spare == Spare::Zero) std::memset(replacement + live, 0, bytes - live);
        } else {
            std::memset(replacement, 0, bytes);
            for (std::size_t i = 0; i < count; ++i)
                relocate(element, replacement + i * element->size, at(i));
        }
        bolt::memory::deallocate_bytes(data);
        data = replacement;
        capacity = n;
    }
    // Both callers overwrite the entire payload from an existing allocation.
    // The cache owns only dead storage and a hit transfers it to this sequence.
    void reserve_snapshot(Context& context, std::size_t n, std::size_t minimum) {
        if (!data && uses_heap_realloc() && element->size &&
            n <= std::numeric_limits<std::size_t>::max() / element->size) {
            const auto bytes = n * element->size;
            if (SnapshotStorage::cacheable(bytes, minimum)) {
                data = allocation_owner(context).snapshot_storage.take(bytes);
                if (data) capacity = n;
                recyclable_snapshot = true;
            }
        }
        reserve(n, Spare::Overwritten);
    }
    void grow_for(std::size_t required) {
        if (required > capacity)
            reserve(bolt::memory::detail::buffer_growth_capacity(capacity, required), Spare::Overwritten);
    }
    void clear(bool reuse = true) noexcept {
        if (count && !bitwise) {
            if (needs_element_drop(element)) {
                for (std::size_t i = count; i > 0; --i) drop(element, at(i - 1));
            } else if (reuse) {
                std::memset(data, 0, count * element->size);
            }
        }
        count = 0;
    }
    void truncate(std::size_t retained) noexcept {
        // Only [0, count) is live. Add, Insert and AppendRange initialize each
        // new element before exposing it, like the typed C++ Buffer<T>.
        if (count <= retained) return;
        if (!bitwise) {
            if (needs_element_drop(element)) {
                while (count > retained) drop(element, at(--count));
                return;
            }
            // Clear reusable slots, including padding and inactive optional payloads.
            std::memset(at(retained), 0, (count - retained) * element->size);
        }
        count = retained;
    }
    void add(const void* src) {
        if (count < capacity && bitwise) {
            std::memcpy(at(count), src, element->size);
            ++count;
            return;
        }
        Box value(element, src, owner); // Own the source before relocating aliased storage.
        if (count == capacity) grow_for(count + 1);
        relocate(element, at(count++), value.data());
    }
    void erase(std::size_t i) {
        if (bitwise) {
            const auto tail = count - i - 1;
            if (tail) std::memmove(at(i), at(i + 1), tail * element->size);
        } else {
            drop(element, at(i));
            for (auto n = i; n + 1 < count; ++n)
                relocate(element, at(n), at(n + 1));
        }
        --count;
        std::memset(at(count), 0, element->size);
    }
    void insert(std::size_t i, const void* src) {
        // Snapshot the incoming element before growing or shifting aliased storage.
        std::byte local[64];
        if (bitwise && element->size <= sizeof(local)) {
            std::memcpy(local, src, element->size);
            insert_bytes(i, local);
            return;
        }
        Box value(element, src, owner);
        if (bitwise) {
            insert_bytes(i, value.data());
            return;
        }
        if (count == capacity) grow_for(count + 1);
        for (auto n = count; n > i; --n) relocate(element, at(n), at(n - 1));
        relocate(element, at(i), value.data());
        ++count;
    }
    void insert_bytes(std::size_t i, const void* saved) {
        if (count == capacity) grow_for(count + 1);
        const auto tail = count - i;
        if (tail) std::memmove(at(i + 1), at(i), tail * element->size);
        std::memcpy(at(i), saved, element->size);
        ++count;
    }
};
// Explicit base conversions make the slot ABI independent of the C++ layout
// of Sequence, including the implementation of the PMR vector.
Sequence* read_sequence(const void* slot) noexcept {
    return static_cast<Sequence*>(read<BrSequenceHeader*>(slot));
}
void write_sequence(void* slot, Sequence* sequence) noexcept {
    write(slot, static_cast<BrSequenceHeader*>(sequence));
}
using Set=std::pmr::unordered_set<Box,Hasher,Equal>;
using Dict=std::pmr::unordered_map<Box,Box,Hasher,Equal>;
using QueueValues=std::pmr::deque<Box>;
using Text=bolt::native_abi::NativeText;

// A range borrows the container. Its iterator is used only after verifying that
// no structural mutation occurred; a single allocation replaces O(n^2) indexing
// of hash-table iterators. Mutations of existing List element values remain legal.
struct RangeState {
    BrContext* context;BrType* type;void* slot;void* identity;
    std::uint64_t epoch;std::size_t index=0;char32_t character=0;
    std::optional<Set::iterator> set_iterator;
    std::optional<QueueValues::iterator> queue_iterator;
    std::optional<Dict::iterator> dict_iterator;Box pair_view;
    RangeState(BrContext& c,BrType* t,void* s,BrType* element):context(&c),type(t),slot(s),identity(read<void*>(s)),epoch(0),pair_view(t->kind==Dictionary?element:nullptr){
        if(type->kind==HashSet&&identity)set_iterator=static_cast<Set*>(identity)->begin();
        if(type->kind==Queue&&identity)queue_iterator=static_cast<QueueValues*>(identity)->begin();
        if(type->kind==Dictionary&&identity)dict_iterator=static_cast<Dict*>(identity)->begin();
        auto& state=impl(&c).range_epochs[slot];epoch=state.value;++state.readers;
        ++c.active_ranges;
    }
    ~RangeState(){
        auto& epochs = impl(context).range_epochs;
        auto i = epochs.find(slot);
        if(i != epochs.end() && !--i->second.readers) epochs.erase(i);
        --context->active_ranges;
    }
    void* next();
};
void mutated(Context& state, const void* slot) {
    if (state.range_epochs.empty()) return;
    const auto entry = state.range_epochs.find(slot);
    if (entry != state.range_epochs.end()) ++entry->second.value;
}
void mutated(BrContext* c,const void* slot){if(c->active_ranges)mutated(impl(c),slot);}

std::string_view text(const void* slot) {
    // Read-only bridge inputs may use a static literal descriptor. Every
    // owning NativeText begins with the same public header.
    const auto* view = read<const BrTextHeader*>(slot);
    return view ? std::string_view(view->data,view->size) : std::string_view{};
}
char32_t text_at(const void* slot,std::int32_t index) {
    const auto* value = read<Text*>(slot);
    const auto position = static_cast<std::size_t>(index);
    return value ? value->at(position) : bolt::unicode::at({}, position);
}
Text* put_text(void* dest,std::string_view value,std::string_view suffix = {}) {
    auto* next = Text::create(bolt::memory::owner_of(dest), value, suffix);
    bolt::memory::destroy(read<Text*>(dest));
    write(dest, next);
    return next;
}
template<class T> T& container(void* slot){auto* p=read<T*>(slot);if(!p){auto* owner=bolt::memory::owner_of(slot);p=bolt::memory::make<T>(owner,bolt::memory::resource(owner));write(slot,p);}return *p;}
Sequence& sequence(BrType* t,void* slot){auto* p=read_sequence(slot);if(!p){auto* owner=bolt::memory::owner_of(slot);p=bolt::memory::make<Sequence>(owner,t->key,owner);write_sequence(slot,p);}return *p;}
// Cleanup operates even while an exception is in flight. Generated destructor
// code reports through a context flag, never through a C++ unwinder.
void write_diagnostic(std::string_view message) {
#ifdef BOLT_WINDOWS_AOT_RUNTIME
    // A file-only executable need not construct the standard iostreams.
    // As with cerr, flush pending stdout before emitting the diagnostic.
    std::fflush(stdout);
    std::fwrite(message.data(),1,message.size(),stderr);
    std::fflush(stderr);
#else
    std::cerr << message;
#endif
}
void run_destructor(BrType* type,void* p)noexcept {
    if(!type->destructor)return;
    auto* c=active_worker_context?active_worker_context:type->context;
    if(!c)std::terminate();
    auto& state=impl(c);auto old=ExceptionRecord{std::move(state.error),std::move(state.error_type),std::move(state.pending_exception)};
    const auto failed=c->failed;const auto flags=c->flags,limit=c->max_depth;
    c->failed=0;c->flags=flags & BR_PRIVATE_STEPS;c->max_depth=0;
    auto* previous=active_worker_context;active_worker_context=c;
    type->destructor(c,p);
    active_worker_context=previous;
    if(c->failed){try{if(state.error)write_diagnostic(bolt::format_diagnostic(*state.error));write_diagnostic("Bolt: exception escaped a destructor; terminating.\n");}catch(...){}std::terminate();}
    state.error=std::move(old.diagnostic);state.error_type=std::move(old.type_name);state.pending_exception=std::move(old.payload);
    c->failed=failed;c->flags=(flags & ~BR_PRIVATE_STEPS) | (c->flags & BR_PRIVATE_STEPS);c->max_depth=limit;
}
bool completed_as(BrType* ready,BrType* type)noexcept {for(auto* t=ready;t;t=t->base)if(t==type)return true;return false;}
void drop_class(BrType* t,void* p,BrType* ready)noexcept {
    if(t->kind!=Class){drop(t,p);return;}
    write(p,t);
    if(completed_as(ready,t))run_destructor(t,p);
    const auto inherited=t->base?t->base->fields.size():0;
    for(std::size_t i=t->fields.size();i>inherited;--i){auto [off,field]=t->fields[i-1];drop(field,static_cast<std::byte*>(p)+off);}
    if(t->base)drop_class(t->base,p,ready);
}
void drop(BrType* t,void* p)noexcept{
    if(!t||!p)return;
    switch(t->kind){
    case SharedResource:if(auto* value=read<void*>(p))bolt::native_backend::detail::call_generated(t->release_resource,nullptr,value);break;
    case Optional:if(read<std::uint8_t>(p))drop(t->key,static_cast<std::byte*>(p)+8);break;
    case Range:delete read<RangeState*>(p);break;
    case ErrorState:delete read<ExceptionRecord*>(p);break;
    case LibraryValue:if(auto* ops=slot_ops(p))ops->destroy(library_data(p));break;
    case Arena:delete read<bolt::memory::Arena*>(p);break;
    case String:bolt::memory::destroy(read<Text*>(p));break;
    case Interface:
        if(auto* boxed=interface_box(p)) {
            if(bolt::capture_cell_type(t->name)) {
                auto* cell=static_cast<CaptureBox*>(boxed);
                if(cell->references.fetch_sub(1,std::memory_order_acq_rel)==1)delete cell;
            }else bolt::memory::destroy(boxed);
        }
        break;
    case Struct:{
        run_destructor(t,p);
        for(std::size_t i=t->fields.size();i>0;--i){auto [off,field]=t->fields[i-1];drop(field,static_cast<std::byte*>(p)+off);}
        break;
    }
    case Class:{auto* ready=t->marker==static_cast<std::size_t>(-1)?nullptr:read<BrType*>(static_cast<std::byte*>(p)+t->marker);drop_class(t,p,ready);break;}
    case Array:case List:
        if (auto* seq=read_sequence(p)) {
            if (seq->recyclable_snapshot && seq->uses_heap_realloc()) {
                const auto bytes=seq->capacity*seq->element->size;
                if (SnapshotStorage::cacheable(bytes, 4096))
                    allocation_owner(impl(t->context)).snapshot_storage.put(std::exchange(seq->data,nullptr),bytes);
            }
            bolt::memory::destroy(seq);
        }
        break;
    case HashSet:bolt::memory::destroy(read<Set*>(p));break;
    case Dictionary:bolt::memory::destroy(read<Dict*>(p));break;
    case Queue:bolt::memory::destroy(read<QueueValues*>(p));break;
    default:break;
    }
    std::memset(p,0,t->size);
}
void clone(BrType* t,void* dest,const void* src,bolt::memory::Arena* owner){
    switch(t->kind){
    case SharedResource:if(auto* value=read<void*>(src)){bolt::native_backend::detail::call_generated(t->retain_resource,nullptr,value);write(dest,value);}break;
    case Optional:if(read<std::uint8_t>(src)){write(dest,std::uint8_t{1});clone(t->key,static_cast<std::byte*>(dest)+8,static_cast<const std::byte*>(src)+8,owner);}break;
    case Range:case ErrorState:throw std::runtime_error("internal control state cannot be copied");
    case LibraryValue:if(auto* ops=slot_ops(src)){
        if(ops->bytes>t->size)throw std::runtime_error("native library value does not fit destination storage; use a reference to its base type");
        ops->copy(library_data(dest),library_data(src));write(dest,t);set_ops(dest,ops);
    }break;
    case Arena:throw std::runtime_error("arena owners cannot be copied");
    case String:if(auto* p=read<Text*>(src))write(dest,p->clone(owner));break;
    case Interface:{
        Box* boxed;
        if(bolt::capture_cell_type(t->name)&&read<BrType*>(src)==t) {
            auto* cell=static_cast<CaptureBox*>(interface_box(src));
            if(cell)cell->references.fetch_add(1,std::memory_order_relaxed);
            boxed=cell;
        }else {
            auto [dynamic,value]=unwrapped(t,src);
            boxed=bolt::capture_cell_type(t->name)?new CaptureBox(dynamic,value):bolt::memory::make<Box>(owner,dynamic,value,owner);
        }
        write(dest,t);write(static_cast<std::byte*>(dest)+8,boxed);break;
    }
    case Struct:{
        for(auto [off,f]:t->fields)clone(f,static_cast<std::byte*>(dest)+off,static_cast<const std::byte*>(src)+off,owner);
        break;
    }
    case Class:{
        if(t->base)clone(t->base,dest,src,owner);write(dest,t);
        const auto inherited=t->base?t->base->fields.size():0;
        for(std::size_t i=inherited;i<t->fields.size();++i){auto [off,f]=t->fields[i];clone(f,static_cast<std::byte*>(dest)+off,static_cast<const std::byte*>(src)+off,owner);}
        if(t->marker!=static_cast<std::size_t>(-1))write(static_cast<std::byte*>(dest)+t->marker,t);
        break;
    }
    case Array:case List:if(auto* p=read_sequence(src))write_sequence(dest,bolt::memory::make<Sequence>(owner,*p,owner));break;
    case HashSet:if(auto* p=read<Set*>(src))write(dest,bolt::memory::make<Set>(owner,*p,bolt::memory::resource(owner)));break;
    case Dictionary:if(auto* p=read<Dict*>(src))write(dest,bolt::memory::make<Dict>(owner,*p,bolt::memory::resource(owner)));break;
    case Queue:if(auto* p=read<QueueValues*>(src))write(dest,bolt::memory::make<QueueValues>(owner,*p,bolt::memory::resource(owner)));break;
    default:std::memcpy(dest,src,t->size);break;
    }
}
void relocate(BrType* t,void* dest,void* src)noexcept{
    if(dest==src||!t)return;
    if(t->kind==Optional){if(read<std::uint8_t>(src)){relocate(t->key,static_cast<std::byte*>(dest)+8,static_cast<std::byte*>(src)+8);write(dest,std::uint8_t{1});}}
    else if(t->kind==LibraryValue){if(auto* ops=slot_ops(src)){ops->move(library_data(dest),library_data(src));write(dest,read<BrType*>(src));set_ops(dest,ops);}else std::memset(dest,0,t->size);}
    else if(t->kind==Struct){
        for(auto [off,f]:t->fields)relocate(f,static_cast<std::byte*>(dest)+off,static_cast<std::byte*>(src)+off);
    }
    else if(t->kind==Class){
        auto* dynamic=read<BrType*>(src);auto* ready=t->marker==static_cast<std::size_t>(-1)?nullptr:read<BrType*>(static_cast<std::byte*>(src)+t->marker);
        if(t->base)relocate(t->base,dest,src);
        write(dest,dynamic);const auto inherited=t->base?t->base->fields.size():0;
        for(std::size_t i=inherited;i<t->fields.size();++i){auto [off,f]=t->fields[i];relocate(f,static_cast<std::byte*>(dest)+off,static_cast<std::byte*>(src)+off);}
        if(t->marker!=static_cast<std::size_t>(-1))write(static_cast<std::byte*>(dest)+t->marker,ready);
    }
    else std::memcpy(dest,src,t->size);
    std::memset(src,0,t->size);
}
void assign(BrType* t,void* dest,const void* src){
    if(dest==src)return;
    if (t->kind == String) {
        const auto* source = read<Text*>(src);
        auto* next = source ? source->clone(bolt::memory::owner_of(dest)) : nullptr;
        bolt::memory::destroy(read<Text*>(dest));
        write(dest, next);
        return;
    }
    Box copy(t,src,bolt::memory::owner_of(dest));drop(t,dest);relocate(t,dest,copy.data());
}

std::size_t size(BrType* t,const void* slot){
    switch(t->kind){case String:{auto* v=read<Text*>(slot);return v?v->length():0;}case Array:case List:{auto* v=read_sequence(slot);return v?v->count:0;}case HashSet:{auto* v=read<Set*>(slot);return v?v->size():0;}case Dictionary:{auto* v=read<Dict*>(slot);return v?v->size():0;}case Queue:{auto* v=read<QueueValues*>(slot);return v?v->size():0;}default:throw std::runtime_error("type does not have a count");}
}
std::size_t array_length(BrType* t,const Sequence* value,std::size_t dimension){
    if(!value)return 0;
    if(t->rank==1)return value->count;
    if(value->dimensions.empty())return 0;
    return value->dimensions.at(dimension);
}
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
bool equal(BrType* t,const void* a,const void* b){
    if(t->kind==Optional){const bool x=read<std::uint8_t>(a)!=0,y=read<std::uint8_t>(b)!=0;return x==y&&(!x||equal(t->key,static_cast<const std::byte*>(a)+8,static_cast<const std::byte*>(b)+8));}
    if(t->equal_value){auto* active=active_worker_context?active_worker_context:t->context;auto result=t->equal_value(active,a,b);if(active->failed)throw std::runtime_error("value equality failed");return result!=0;}
    if(t->kind==Interface){auto [ta,va]=unwrapped(t,a);auto [tb,vb]=unwrapped(t,b);return ta==tb&&equal(ta,va,vb);}
    if(t->kind==LibraryValue){auto* x=slot_ops(a);auto* y=slot_ops(b);return x==y&&(!x||x->equals(library_data(a),library_data(b)));}
    if(t->kind==String)return text(a)==text(b);
    if(t->kind==Float)return t->size==4?read<float>(a)==read<float>(b):read<double>(a)==read<double>(b);
    if(t->kind==Class||t->kind==Struct){for(auto [off,f]:t->fields)if(!equal(f,static_cast<const std::byte*>(a)+off,static_cast<const std::byte*>(b)+off))return false;return true;}
    if(t->kind==Array||t->kind==List){if(size(t,a)!=size(t,b))return false;auto* x=read_sequence(a);auto* y=read_sequence(b);if(t->kind==Array)for(std::size_t d=0;d<t->rank;++d)if(array_length(t,x,d)!=array_length(t,y,d))return false;if(!x||!x->count)return true;for(std::size_t i=0;i<x->count;++i)if(!equal(t->key,x->at(i),y->at(i)))return false;return true;}
    if(t->kind==HashSet){if(size(t,a)!=size(t,b))return false;auto* x=read<Set*>(a);auto* y=read<Set*>(b);if(!x||x->empty())return true;for(const auto& v:*x)if(!y->contains(v))return false;return true;}
    if(t->kind==Dictionary){if(size(t,a)!=size(t,b))return false;auto* x=read<Dict*>(a);auto* y=read<Dict*>(b);if(!x||x->empty())return true;for(const auto& [k,v]:*x){auto i=y->find(k);if(i==y->end()||!equal(t->value,v.data(),i->second.data()))return false;}return true;}
    if(t->kind==Queue){if(size(t,a)!=size(t,b))return false;auto* x=read<QueueValues*>(a);auto* y=read<QueueValues*>(b);if(!x||x->empty())return true;for(std::size_t i=0;i<x->size();++i)if(!equal(t->key,(*x)[i].data(),(*y)[i].data()))return false;return true;}
    return std::memcmp(a,b,t->size)==0;
}
// Raw scalar hashes use the native object representation and size_t arithmetic.
std::size_t hash_bytes(const void* value, std::size_t count) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(value);
    std::size_t result = 0;
    for (std::size_t i = 0; i < count; ++i) result = bolt::native::hash_combine(result, bytes[i]);
    return result;
}
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
std::size_t hash(BrType* t,const void* p){
    if(t->kind==Optional)return read<std::uint8_t>(p)?(hash(t->key,static_cast<const std::byte*>(p)+8)^std::size_t{0x6d2b79f5}):std::size_t{0x63bd};
    if(t->hash_value){auto* active=active_worker_context?active_worker_context:t->context;auto result=t->hash_value(active,p);if(active->failed)throw std::runtime_error("value hashing failed");return static_cast<std::size_t>(result);}
    if(t->equal_value)return 0; // Correct (although collision-heavy) when only equality is customized.
    if(t->kind==Interface){auto [dynamic,value]=unwrapped(t,p);return hash(dynamic,value);}
    // Opaque value equality is exact; a conservative constant hash is valid
    // even when C++ object padding differs. Future adapters may add semantic hashes.
    if(t->kind==LibraryValue){auto* ops=slot_ops(p);return ops?ops->hash_value(library_data(p)):0;}
    if(t->kind==String){const auto* value=read<Text*>(p);return value?value->hash():std::hash<std::string_view>{}({});}
    if(t->kind==Float)return t->size==4?std::hash<float>{}(read<float>(p)):std::hash<double>{}(read<double>(p));
    std::size_t h=0;auto mix=[&](std::size_t x){h=bolt::native::hash_combine(h,x);};
    if(t->kind==Class||t->kind==Struct){for(auto [off,f]:t->fields)mix(hash(f,static_cast<const std::byte*>(p)+off));return h;}
    if(t->kind==Array||t->kind==List){auto* a=read_sequence(p);if(t->kind==Array&&t->rank>1)for(std::size_t d=0;d<t->rank;++d)mix(array_length(t,a,d));if(a)for(std::size_t i=0;i<a->count;++i)mix(hash(t->key,a->at(i)));return h;}
    if(t->kind==HashSet){auto* a=read<Set*>(p);if(a)for(const auto& v:*a)h^=hash(t->key,v.data());return h;}
    if(t->kind==Dictionary){auto* a=read<Dict*>(p);if(a)for(const auto& [k,v]:*a)h^=bolt::native::hash_combine(hash(t->key,k.data()),hash(t->value,v.data()));return h;}
    if(t->kind==Queue){auto* a=read<QueueValues*>(p);if(a)for(const auto& v:*a)mix(hash(t->key,v.data()));return h;}
    return hash_bytes(p, t->size);
}
template<class F> void numeric(BrType* t,F&& f){
    if(t->kind==Signed){switch(t->size){case 1:f.template operator()<std::int8_t>();return;case 2:f.template operator()<std::int16_t>();return;case 4:f.template operator()<std::int32_t>();return;case 8:f.template operator()<std::int64_t>();return;}}
    if(t->kind==Unsigned){switch(t->size){case 1:f.template operator()<std::uint8_t>();return;case 2:f.template operator()<std::uint16_t>();return;case 4:f.template operator()<std::uint32_t>();return;case 8:f.template operator()<std::uint64_t>();return;}}
    if(t->kind==Float){if(t->size==4)f.template operator()<float>();else f.template operator()<double>();return;}
    throw std::runtime_error("expected a numeric native type");
}
std::string value_text(BrType* t,const void* p){if(t->kind==Pointer)return bolt::native::pointer_text(read<void*>(p));if(t->kind==LibraryValue){auto* ops=slot_ops(p);if(!ops)throw std::runtime_error("uninitialized native library value");return ops->format(library_data(p));}if(t->name=="char")return bolt::unicode::encode(read<char32_t>(p));if(t->kind==String)return std::string(text(p));if(t->kind==Bool)return read<std::uint8_t>(p)?"true":"false";std::string s;numeric(t,[&]<class T>(){s=bolt::native::text(read<T>(p));});return s;}

// Adapt arguments to the precompiled standard library. Numeric values never
// become interpreter Values. Strings/arrays are boundary copies, with mutable
// arrays written back into their original allocation to preserve element aliases.
struct BridgeCall{Context* context;BrType* result;void* output;void* self;BrType* const* types;void* const* args;std::size_t count;};
template<class T,class Enable=void> struct BridgeCodec{
    static T get(BrType*,const void* p){return library_object<T>(const_cast<void*>(p));}
    static void put(BrType* t,void* p,const T& value){
        const auto* ops=library_ops<T>();
        if(!t||t->kind!=LibraryValue||t->size<ops->bytes)throw std::runtime_error("native library result storage mismatch");
        // Copy first: the source may alias the destination.
        T copy(value);drop(t,p);std::construct_at(static_cast<T*>(library_data(p)),std::move(copy));write(p,t);set_ops(p,ops);
    }
};
template<class T> struct BridgeCodec<T,std::enable_if_t<std::is_arithmetic_v<T>||std::is_enum_v<T>>>{
    static T get(BrType* t,const void* p){if(!t||!p||t->size!=sizeof(T)||t->kind==LibraryValue)throw std::runtime_error("native numeric argument layout mismatch");return read<T>(p);}
    static void put(BrType* t,void* p,T value){if(!t||!p||t->size!=sizeof(T)||t->kind==LibraryValue)throw std::runtime_error("native numeric result layout mismatch");write(p,value);}
};
template<class T> struct BridgeCodec<T*> {
    static T* get(BrType*,const void* source){return read<T*>(source);}
    static void put(BrType*,void* target,T* value){write(target,value);}
};
template<> struct BridgeCodec<std::string_view> {
    static std::string_view get(BrType* type, const void* slot) {
        if (!type || type->kind != String || !slot) throw std::runtime_error("expected a string");
        const auto value = text(slot);
        return value.empty() ? std::string_view("") : value;
    }
};
template<> struct BridgeCodec<bolt::native::String>{
    static bolt::native::String get(BrType* t,const void* p){if(!t||t->kind!=String)throw std::runtime_error("expected a string");return bolt::native::String(text(p));}
    static void put(BrType* t,void* p,const bolt::native::String& value){if(!t||t->kind!=String)throw std::runtime_error("expected string result storage");put_text(p,std::string_view(value.data(),value.size()));}
};
template<class T> struct BridgeCodec<bolt::native::Array<T>>{
    static bolt::native::Array<T> get(BrType* t,const void* p){
        if(!t||t->kind!=Array||t->rank!=1)throw std::runtime_error("expected a rank-one array");
        const auto* seq=read_sequence(p);const auto n=seq?seq->count:0;
        auto result=bolt::native::Array<T>::Create(bolt::native::cast<std::int32_t>(n));
        if constexpr ((std::is_arithmetic_v<T> || std::is_enum_v<T>) && !std::is_same_v<T,bool>) {
            if (scalar_storage(t->key) && t->key->size == sizeof(T)) {
                if (n) std::memcpy(std::to_address(result.begin()), seq->data, n * sizeof(T));
                return result;
            }
        }
        for(std::size_t i=0;i<n;++i)result.At(static_cast<std::int32_t>(i))=BridgeCodec<T>::get(t->key,seq->at(i));
        return result;
    }
    static void put(BrType* t,void* p,const bolt::native::Array<T>& value){
        if(!t||t->kind!=Array||t->rank!=1)throw std::runtime_error("expected array result storage");
        auto& seq=sequence(t,p);const auto n=static_cast<std::size_t>(value.Count());
        if(seq.count!=n){seq.clear();seq.reserve(n);seq.count=n;}
        if constexpr ((std::is_arithmetic_v<T> || std::is_enum_v<T>) && !std::is_same_v<T,bool>) {
            if (scalar_storage(t->key) && t->key->size == sizeof(T)) {
                if (n) std::memcpy(seq.data, std::to_address(value.begin()), n * sizeof(T));
                return;
            }
        }
        for(std::size_t i=0;i<n;++i)BridgeCodec<T>::put(t->key,seq.at(i),value.At(static_cast<std::int32_t>(i)));
    }
};
// UTF-8 strings and byte arrays both store contiguous bytes. Keep strict
// Unicode validation on decode and give each result independent owned storage.
void bytes_from_string(BridgeCall& call) {
    if (call.count != 1 || !call.result || call.result->kind != Array || call.result->rank != 1 ||
        !call.result->key || call.result->key->name != "byte")
        throw std::runtime_error("invalid UTF-8 byte result");
    const auto bytes = BridgeCodec<std::string_view>::get(call.types[0], call.args[0]);
    const auto count = bolt::native::cast<std::int32_t>(bytes.size());
    auto& result = sequence(call.result, call.output);
    result.reserve_snapshot(*call.context, static_cast<std::size_t>(count), 4096);
    if (count) std::memcpy(result.data, bytes.data(), bytes.size());
    result.count = bytes.size();
}
void replace_utf8(Context& context, void* dest, const void* array, std::size_t first, std::size_t end) {
    const auto* seq = read_sequence(array);
    if (seq) (void)bolt::native::cast<std::int32_t>(seq->count);
    const auto bytes = seq && seq->count ?
        std::string_view(reinterpret_cast<const char*>(seq->data), seq->count) : std::string_view{};
    // Validate before touching the destination. A failed conversion must
    // preserve its bytes, cached indices and any active enumerator.
    auto* previous = read<Text*>(dest);
    Text* next;
    try {
        next = Text::from_utf8(bolt::memory::owner_of(dest), previous, bytes, first, end);
    } catch (const std::invalid_argument& invalid) {
        throw bolt::library::FormatException(invalid.what());
    }
    bolt::memory::destroy(previous);
    write(dest, next);
    mutated(context, dest);
}
void string_from_bytes(BridgeCall& call) {
    if (call.count != 1 || !call.result || call.result->kind != String)
        throw std::runtime_error("invalid UTF-8 string result");
    const auto* type = call.types[0];
    if (!type || type->kind != Array || type->rank != 1 || !type->key || type->key->name != "byte")
        throw std::runtime_error("expected UTF-8 bytes");
    replace_utf8(*call.context, call.output, call.args[0], 0, std::string_view::npos);
}
// Fill the final ABI array directly. Length is a reservation hint; EOF is
// determined by ReadRaw, even when the backing file grows or shrinks.
void stream_read_all_bytes(BridgeCall& call) {
    if (call.count || !call.result || call.result->kind != Array || call.result->rank != 1 ||
        !call.result->key || call.result->key->name != "byte")
        throw std::runtime_error("expected a byte array result");
    auto& stream = library_object<bolt::library::Stream>(call.self);
    auto* arena = bolt::memory::owner_of(call.output);
    bolt::memory::Unique<Sequence> result(bolt::memory::make<Sequence>(arena, call.result->key, arena));
    constexpr auto limit = std::size_t{INT32_MAX};
    bool eof = false;
    if ((dynamic_cast<bolt::library::FileStream*>(&stream) || dynamic_cast<bolt::library::MemoryStream*>(&stream)) &&
        stream.CanRead() && stream.CanSeek()) {
        const auto remaining = stream.Length() - stream.Position();
        if (remaining > 0 && static_cast<std::uint64_t>(remaining) <= limit) {
            const auto size = static_cast<std::size_t>(remaining);
            result->reserve(size, Sequence::Spare::Overwritten);
            while (result->count < size) {
                const auto n = stream.ReadRaw(static_cast<std::uint8_t*>(result->at(result->count)), size - result->count);
                if (!n) { eof = true; break; }
                result->count += n;
            }
        }
    }
    std::uint8_t block[8192];
    while (!eof) {
        const auto n = stream.ReadRaw(block, sizeof(block));
        if (!n) break;
        if (n > limit - result->count)
            throw bolt::library::IOException("The stream is too large for a Bolt array.");
        result->grow_for(result->count + n);
        std::memcpy(result->at(result->count), block, n);
        result->count += n;
    }
    drop(call.result, call.output);
    write_sequence(call.output, result.release());
}
#include "LibraryDispatch.inc"


void* RangeState::next(){
    auto& epochs=impl(context).range_epochs;
    auto current=epochs.find(slot);
    if(read<void*>(slot)!=identity||current==epochs.end()||current->second.value!=epoch)
        throw bolt::library::InvalidOperationException("collection was structurally modified during iteration");
    if(!identity)return nullptr;
    if(type->kind==Array||type->kind==List){auto* value=read_sequence(slot);return index<value->count?value->at(index++):nullptr;}
    if(type->kind==HashSet){auto* value=static_cast<Set*>(identity);if(*set_iterator==value->end())return nullptr;auto* item=const_cast<void*>((*set_iterator)->data());++*set_iterator;return item;}
    if(type->kind==Queue){auto* value=static_cast<QueueValues*>(identity);if(*queue_iterator==value->end())return nullptr;auto* item=(*queue_iterator)->data();++*queue_iterator;return item;}
    if(type->kind==Dictionary){
        auto* value=static_cast<Dict*>(identity);if(*dict_iterator==value->end())return nullptr;
        auto* view=pair_view.type;
        if(!view||(view->kind!=Class&&view->kind!=Struct)||view->fields.size()!=2)throw std::runtime_error("invalid dictionary entry layout");
        const auto& entry=**dict_iterator;if(view->kind==Class)write(pair_view.data(),view);
        assign(view->fields[0].second,static_cast<std::byte*>(pair_view.data())+view->fields[0].first,entry.first.data());
        assign(view->fields[1].second,static_cast<std::byte*>(pair_view.data())+view->fields[1].first,entry.second.data());
        ++*dict_iterator;return pair_view.data();
    }
    if(type->kind==String){const auto value=text(slot);if(index>=value.size())return nullptr;character=bolt::unicode::decode(value,index);return &character;}
    throw std::runtime_error("this collection does not support shared-runtime iteration");
}

// Formatting runs user methods against snapshots, including the collection
// itself: a ToString method may otherwise invalidate the collection's storage.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
std::string formatted_text(BrContext* c,BrType* t,const void* p){
    if(c->failed)return {};
    auto [actual,data]=unwrapped(t,p);
    if(actual->kind==Optional)return read<std::uint8_t>(data)?formatted_text(c,actual->key,static_cast<const std::byte*>(data)+8):"null";
    if(actual->kind==Class||actual->kind==Struct){
        if(!actual->format)throw std::runtime_error("type '"+actual->name+"' does not implement IStringable");
        Box copy(actual,data);
        struct Result{Text* value=nullptr;~Result(){bolt::memory::destroy(value);}} result;
        actual->format(c,&result.value,copy.data());
        return c->failed||!result.value?std::string{}:std::string(result.value->view());
    }
    if(actual->kind!=List&&actual->kind!=HashSet&&actual->kind!=Dictionary&&actual->kind!=Queue)return value_text(actual,data);
    Box copy(actual,data);
    std::vector<std::string> entries;
    if(actual->kind==List){
        if(auto* values=read_sequence(copy.data()))for(std::size_t i=0;i<values->count&&!c->failed;++i)entries.push_back(formatted_text(c,actual->key,values->at(i)));
    }else if(actual->kind==Queue){
        if(auto* values=read<QueueValues*>(copy.data()))for(const auto& value:*values){entries.push_back(formatted_text(c,actual->key,value.data()));if(c->failed)break;}
    }else if(actual->kind==HashSet){
        if(auto* values=read<Set*>(copy.data()))for(const auto& value:*values){entries.push_back(formatted_text(c,actual->key,value.data()));if(c->failed)break;}
    }else{
        if(auto* values=read<Dict*>(copy.data()))for(const auto& [key,value]:*values){
            auto rendered_key=formatted_text(c,actual->key,key.data());if(c->failed)break;
            auto rendered_value=formatted_text(c,actual->value,value.data());if(c->failed)break;
            entries.push_back(std::move(rendered_key)+": "+std::move(rendered_value));
        }
    }
    if(c->failed)return {};
    const bool unordered=actual->kind==HashSet||actual->kind==Dictionary;
    if(unordered)std::sort(entries.begin(),entries.end());
    std::string result=unordered?"{":"[";
    for(std::size_t i=0;i<entries.size();++i){if(i)result+=", ";result+=entries[i];}
    result+=unordered?"}":"]";
    return result;
}
// This callback points into generated code, which has no Clang UBSan
// function-type prefix. Do not probe before its entry address.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
void initialize(BrContext* c,BrType* t,void* p){if(t->initialize)t->initialize(c,p);else std::memset(p,0,t->size);}
std::size_t index(std::int64_t i,std::size_t length,bool end=false){if(i<0||static_cast<std::uint64_t>(i)>length||(!end&&static_cast<std::uint64_t>(i)==length))throw std::runtime_error("index out of range");return static_cast<std::size_t>(i);}
} // namespace
#include "ReflectionRuntime.inc"
extern "C" {
void br_range(BrContext* c,const char* site,BrType* type,void* source,BrType* element,void* output)noexcept{
    checked(c,site,[&]{auto value=std::make_unique<RangeState>(*c,type,source,element);delete read<RangeState*>(output);write(output,value.release());});
}
void* br_range_next(BrContext* c,const char* site,void* range)noexcept {
    void* value=nullptr;checked(c,site,[&]{auto* state=read<RangeState*>(range);if(!state)throw std::runtime_error("uninitialized range");value=state->next();});return value;
}
std::uint64_t br_library_schema() noexcept{return bolt::native_abi::library_schema();}
void br_process_arguments(BrContext* c,std::int32_t count,const char* const* arguments)noexcept{
    checked(c,nullptr,[&]{
        auto& dest=impl(c).arguments;dest.clear();
#ifdef _WIN32
        if(count<0){
            int n=0;auto** args=CommandLineToArgvW(GetCommandLineW(),&n);
            if(!args)throw std::runtime_error("could not read process arguments");
            struct Guard{wchar_t** p;~Guard(){LocalFree(p);}} guard{args};
            for(int i=1;i<n;++i) {
                // Arguments are text; generic path formatting would replace
                // backslashes even when they are part of the application's data.
                const auto text=std::filesystem::path(args[i]).u8string();
                dest.emplace_back(reinterpret_cast<const char*>(text.data()),text.size());
            }
            return;
        }
#endif
        if(count<0||(count>0&&!arguments))throw std::runtime_error("invalid process argument vector");
        for(std::int32_t i=1;i<count;++i){if(!arguments[i])throw std::runtime_error("null process argument");std::string value(arguments[i]);(void)bolt::unicode::length(value);dest.push_back(std::move(value));}
    });
}
void br_arguments(BrContext* c,const char* site,BrType* t,void* output)noexcept{
    checked(c,site,[&]{
        if(!t||t->kind!=Array||!t->key||t->key->kind!=String)throw std::runtime_error("entry arguments require string[] storage");
        auto& seq=sequence(t,output);seq.clear();seq.reserve(impl(c).arguments.size());seq.count=impl(c).arguments.size();
        for(std::size_t i=0;i<seq.count;++i)put_text(seq.at(i),impl(c).arguments[i]);
    });
}
void br_memory_copy(BrContext* c, const char* site, std::uint8_t* dest,
                    const std::uint8_t* src, std::uint64_t count) noexcept {
    checked(c, site, [&] { bolt::system::MemoryOps::Copy(dest, src, count); });
}
void br_memory_move(BrContext* c, const char* site, std::uint8_t* dest,
                    const std::uint8_t* src, std::uint64_t count) noexcept {
    checked(c, site, [&] { bolt::system::MemoryOps::Move(dest, src, count); });
}
void br_memory_set(BrContext* c, const char* site, std::uint8_t* dest,
                   std::uint8_t value, std::uint64_t count) noexcept {
    checked(c, site, [&] { bolt::system::MemoryOps::Set(dest, value, count); });
}
std::int32_t br_memory_compare(BrContext* c, const char* site, const std::uint8_t* a,
                               const std::uint8_t* b, std::uint64_t count) noexcept {
    std::int32_t result = 0;
    checked(c, site, [&] { result = bolt::system::MemoryOps::Compare(a, b, count); });
    return result;
}
std::uint32_t br_abi_version() noexcept {return bolt::native_abi::Version;}
#if defined(BOLT_WINDOWS_AOT_RUNTIME)
// Individual entries let the COFF linker discard unused adapters and their dependencies.
#define BOLT_LIBRARY_ENTRY(id, signature) \
void br_static_library_##id(BrContext* c,const char* site,BrType* result,void* output,void* self, \
                            BrType* const* types,void* const* args,std::uint64_t count) noexcept { \
    checked(c,site,[&]{ \
        if(count&&(!types||!args))throw std::runtime_error("missing native library arguments"); \
        BridgeCall call{&impl(c),result,output,self,types,args,static_cast<std::size_t>(count)}; \
        library_op_##id(call); \
    }); \
}
#include "LibraryManifest.inc"
#undef BOLT_LIBRARY_ENTRY
#else
void br_library(BrContext* c,const char* site,std::int32_t operation,BrType* result,void* output,void* self,BrType* const* types,void* const* args,std::uint64_t count)noexcept {
    checked(c,site,[&]{
        if(operation<0||static_cast<std::size_t>(operation)>=std::size(library_operations))throw std::runtime_error("unknown native library ABI operation; rebuild with the matching Bolt runtime");
        if(count&&(!types||!args))throw std::runtime_error("missing native library arguments");
        BridgeCall call{&impl(c),result,output,self,types,args,static_cast<std::size_t>(count)};
        library_operations[operation](call);
    });
}
#endif

BrContext* br_create(std::uint64_t steps,std::uint32_t depth) noexcept {
    try {
        auto c=std::make_unique<BrContext>();
        auto p=std::make_unique<Context>();
        c->flags=steps ? 1u : 0u;
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
        if (bolt::native_abi::has_avx2()) c->flags |= BrContext::Avx2;
#ifdef PF_AVX512F_INSTRUCTIONS_AVAILABLE
        static const bool zmm_available = IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE) != FALSE;
        if (zmm_available) c->flags |= BrContext::Avx512F;
#endif
#endif
        c->steps=steps;
        c->max_depth=depth;
        p->options.max_steps=static_cast<std::size_t>(steps);
        p->options.max_call_depth=depth;
        p->budget=std::make_shared<bolt::detail::ExecutionBudget>(p->options.max_steps,&c->steps);
        c->implementation=p.release();
        return c.release();
    } catch (...) { return nullptr; }
}
void br_destructor(BrContext* c,BrType* type,void* function,std::uint64_t marker)noexcept {checked(c,nullptr,[&]{const auto value=static_cast<std::size_t>(marker);if(value==std::numeric_limits<std::size_t>::max()){if(type->kind!=Struct)throw std::runtime_error("a marker-less destructor is only valid for structs");}else if(value>type->size||8>type->size-value)throw std::runtime_error("invalid destructor marker");type->marker=value;type->destructor=reinterpret_cast<void(*)(BrContext*,void*)>(function);});}
void br_new_failed(BrContext* c,BrType* type,void* p)noexcept {
    if(!c||!p)return;drop(type,p);if(bolt::memory::owner_of(p))return;
    if(remove_allocation(impl(c),p))::operator delete(p);
}
BrContext* br_context_child(BrContext* parent) noexcept {
    auto* child=br_create(0,parent->max_depth);
    if(child){
        auto& local=impl(child);auto& caller=impl(parent);
        local.type_owner=&allocation_owner(caller);local.input=caller.input;local.output=caller.output;
        local.options=caller.options;local.budget=caller.budget;local.owns_budget=false;
        parent->flags &= ~BR_PRIVATE_STEPS;
        child->flags=parent->flags;
    }
    return child;
}
BrContext* br_context_activate(BrContext* c) noexcept {return std::exchange(active_worker_context,c);}
void* br_context_registry(BrContext* c) noexcept {return allocation_owner(impl(c)).thread_registry;}
std::uint64_t br_type_size(BrType* t) noexcept {return t->size;}
void br_context_cleanup(BrContext* c,void* fn) noexcept {impl(c).drain=reinterpret_cast<void(*)(BrContext*)>(fn);}
void br_shared_hooks(BrContext*,BrType* t,void* retain,void* release) noexcept {
    t->retain_resource=reinterpret_cast<decltype(t->retain_resource)>(retain);
    t->release_resource=reinterpret_cast<decltype(t->release_resource)>(release);
}
void br_base(BrContext* c,BrType* type,BrType* base)noexcept {checked(c,nullptr,[&]{if(type==base)throw std::runtime_error("recursive native base");type->base=base;});}
void br_raise(BrContext* c,const char* site,BrType* type,const void* value)noexcept {
    checked(c,site,[&]{auto [actual,source]=unwrapped(type,value);auto payload=std::make_shared<Box>(actual,source);
        std::string message="Bolt exception";auto* root=actual;while(root->base)root=root->base;if(root->kind==LibraryValue)if(auto* ops=slot_ops(source))if(auto* native=ops->exception(library_data(payload->data()))){message=native->what();native->SetSource(location(site));}
        failure(c,site,message);impl(c).error_type=actual->name;impl(c).pending_exception=std::move(payload);
    });
}
std::int32_t br_exception_match(BrContext* c,BrType* target)noexcept {
    if(!c||!c->failed)return 0;if(!target)return 1;auto& state=impl(c);
    BrType* actual=state.pending_exception?state.pending_exception->type:nullptr;
    if(!actual)for(auto& type:registered_types(state))if(type->name==state.error_type){actual=type.get();break;}
    for(auto* type=actual;type;type=type->base)if(type==target)return 1;
    return !actual&&target->name=="System.Exception"?1:0;
}
void* br_catch_begin(BrContext* c,const char* site)noexcept {
    if(!c||!c->failed)return nullptr;
    try {auto& state=impl(c);
        if(!state.pending_exception){BrType* type=nullptr;for(auto& item:registered_types(state))if(item->name==state.error_type){type=item.get();break;}
            if(!type)for(auto& item:registered_types(state))if(item->name=="System.Exception"){type=item.get();break;}
            if(!type)throw std::runtime_error("No exception type descriptor is registered.");
            auto* implementation=exception_ops("bolt::library::"+type->name.substr(type->name.rfind('.')+1));auto payload=std::make_shared<Box>(type);
            implementation->make_exception(library_data(payload->data()),state.error?state.error->message:"Bolt runtime failure",state.error?state.error->location:location(site));
            write(payload->data(),type);set_ops(payload->data(),implementation);state.pending_exception=std::move(payload);
            if(auto* exception=implementation->exception(library_data(state.pending_exception->data())))if(state.error)for(const auto& frame:state.error->frames)exception->AddFrame(frame.location,frame.message);
        }
        state.caught.push_back({std::move(state.error),std::move(state.error_type),std::move(state.pending_exception)});c->failed=0;
        return state.caught.back().payload->data();
    }catch(...){return nullptr;}
}
void br_catch_end(BrContext* c)noexcept {if(c&&!impl(c).caught.empty())impl(c).caught.pop_back();}
void br_rethrow(BrContext* c,const char* site)noexcept {if(!c)return;try{auto& s=impl(c);if(s.caught.empty()){failure(c,site,"No active exception to rethrow.");return;}const auto& caught=s.caught.back();s.error=caught.diagnostic;s.error_type=caught.type_name;s.pending_exception=caught.payload;c->failed=1;}catch(...){c->failed=1;}}
void br_error_suspend(BrContext* c,void* slot)noexcept {
    if(!c)return;try{auto& s=impl(c);auto* saved=new ExceptionRecord{std::move(s.error),std::move(s.error_type),std::move(s.pending_exception)};write(slot,saved);c->failed=0;}catch(...){c->failed=1;}
}
void br_error_restore(BrContext* c,void* slot)noexcept {if(!c)return;auto* saved=read<ExceptionRecord*>(slot);if(!saved)return;
    if(!c->failed){auto& s=impl(c);s.error=std::move(saved->diagnostic);s.error_type=std::move(saved->type_name);s.pending_exception=std::move(saved->payload);c->failed=1;}delete saved;write(slot,static_cast<ExceptionRecord*>(nullptr));
}
void br_destroy(BrContext* c)noexcept{
    if(!c)return;
    auto* p=static_cast<Context*>(c->implementation);
    p->caught.clear();p->pending_exception.reset();
    if(!p->type_owner){
        // A joined worker may transfer a new allocation to its caller. Only the root
        // execution owns that ledger; returning from a worker must not free it.
        for(;;){
            // A user destructor can submit more work while reclaiming the root heap.
            if(p->drain)bolt::native_backend::detail::call_generated(p->drain,c);
            void* pointer=nullptr;BrType* type=nullptr;
            {
                std::lock_guard lock(p->allocation_mutex);
                if(p->allocations.empty())break;
                auto node=p->allocations.extract(p->allocations.begin());
                pointer=node.key();type=node.mapped();
            }
            drop(type,pointer);::operator delete(pointer);
        }
    }
    delete p;delete c;
}
std::int32_t br_finish(BrContext* c,std::int32_t status)noexcept{if(!c){write_diagnostic("Bolt: could not allocate a native execution context\n");return 1;}if(c->failed){try{if(impl(c).error)write_diagnostic(bolt::format_diagnostic(*impl(c).error));else write_diagnostic("Bolt: native execution failed\n");}catch(...){}status=1;}br_destroy(c);return status;}
void br_process_exit(std::int32_t status)noexcept{std::exit(status);}
void br_fail(BrContext* c,const char* site,const char* msg)noexcept{failure(c,site,msg?msg:"native error");}
void br_frame(BrContext* c,const char* site,const char* method)noexcept{if(!c||!c->failed)return;try{auto& state=impl(c);auto at=location(site);auto name=method?method:"<native>";if(state.error)state.error->frames.push_back({at,name});
    if(auto& payload=state.pending_exception){auto* root=payload->type;while(root->base)root=root->base;if(root->kind==LibraryValue)if(auto* ops=slot_ops(payload->data()))if(auto* error=ops->exception(library_data(payload->data())))error->AddFrame(at,name);}
}catch(...) {}}
void br_tick(BrContext* c,const char* site)noexcept{if(!c||c->failed||!(c->flags&1))return;
    if(!impl(c).budget->consume_step())failure(c,site,"runtime instruction budget exceeded");}
std::int32_t br_enter(BrContext* c,const char* site)noexcept {
    if(!c||c->failed)return 0;
    if(c->max_depth&&c->depth>=c->max_depth){failure(c,site,"runtime call-depth limit exceeded");return 0;}
    ++c->depth;return 1;
}
void br_leave(BrContext* c,std::int32_t entered)noexcept{if(c&&entered&&c->depth)--c->depth;}
void br_project_bind(BrContext* c,const char* key,void* address) noexcept {
    checked(c,nullptr,[&]{allocation_owner(impl(c)).project_symbols.emplace(key,address);});
}
void* br_project_lookup(BrContext* c,const char* key) noexcept {
    // Workers and cleanup paths can read the table, including while an exception is active.
    const auto& symbols=allocation_owner(impl(c)).project_symbols;
    if(auto it=symbols.find(key);it!=symbols.end())return it->second;
    failure(c,nullptr,"unresolved Bolt project symbol");
    return nullptr;
}
BrType* br_type(BrContext* c,const char* name,std::int32_t kind,std::uint64_t bytes)noexcept{BrType* out=nullptr;checked(c,nullptr,[&]{if(!bytes||bytes>std::numeric_limits<std::size_t>::max())throw std::runtime_error("invalid native type size");auto t=std::make_unique<BrType>();t->context=c;t->name=name;t->kind=kind;t->size=static_cast<std::size_t>(bytes);if(kind==LibraryValue){
        // Layout checks need only sizes. Avoid constructing a map at startup
        // and referencing every library implementation from a static program.
        struct Layout { std::string_view name; std::size_t bytes; };
        static constexpr Layout layouts[] = {
#define BOLT_LIBRARY_LAYOUT(name, type) {name,library_size<type>()},
#include "LibraryTypes.inc"
#undef BOLT_LIBRARY_LAYOUT
        };
        std::size_t expected=0;
        for(const auto& layout:layouts)if(layout.name==t->name){expected=layout.bytes;break;}
        if(expected!=t->size)
            throw std::runtime_error("Bolt runtime library layout mismatch; rebuild/install compiler and runtime together");
    }
    if(kind==Array)t->rank=std::max<std::size_t>(1,bolt::array_rank(t->name));
    auto& state=impl(c);
    state.types.push_back(std::move(t));
    state.type_addresses.insert(state.types.back().get());
    out=state.types.back().get();
});return out;}
void br_field(BrContext* c,BrType* t,std::uint64_t offset,BrType* field)noexcept{checked(c,nullptr,[&]{if(!t||!field||offset>t->size||field->size>t->size-offset)throw std::runtime_error("invalid native field layout");t->fields.emplace_back(static_cast<std::size_t>(offset),field);});}
void br_elements(BrContext* c,BrType* t,BrType* key,BrType* value)noexcept{checked(c,nullptr,[&]{t->key=key;t->value=value;});}
void br_initializer(BrContext* c,BrType* t,void* fn)noexcept{checked(c,nullptr,[&]{t->initialize=reinterpret_cast<void(*)(BrContext*,void*)>(fn);});}
void br_virtual(BrContext* c,BrType* t,std::int32_t slot,void* fn)noexcept{checked(c,nullptr,[&]{if(slot<0)throw std::runtime_error("invalid native virtual slot");if(t->methods.size()<=static_cast<std::size_t>(slot))t->methods.resize(static_cast<std::size_t>(slot)+1);t->methods[static_cast<std::size_t>(slot)]=fn;});}
void* br_dispatch(BrContext* c,const char* site,void* self,std::int32_t slot)noexcept{void* result=nullptr;checked(c,site,[&]{
    if(!self)throw std::runtime_error("null pointer dereference");
    auto* t=read<BrType*>(self);
    if(!allocation_owner(impl(c)).type_addresses.contains(t))
        throw std::runtime_error("virtual dispatch requires a class or interface receiver");
    t=unwrapped(t,self).first;
    if(slot<0||static_cast<std::size_t>(slot)>=t->methods.size()||!t->methods[static_cast<std::size_t>(slot)])throw std::runtime_error("invalid virtual dispatch");
    result=t->methods[static_cast<std::size_t>(slot)];
});return result;}
void br_box(BrContext* c,const char* site,BrType* target,void* dest,BrType* source,const void* value)noexcept{checked(c,site,[&]{
    if(bolt::capture_cell_type(target->name)) {
        CaptureBox* boxed;
        if(source->kind==Interface&&read<BrType*>(value)==target) {
            boxed=static_cast<CaptureBox*>(interface_box(value));
            if(boxed)boxed->references.fetch_add(1,std::memory_order_relaxed);
        }else {
            auto [actual,data]=unwrapped(source,value);
            boxed=new CaptureBox(actual,data);
        }
        drop(target,dest);write(dest,target);write(static_cast<std::byte*>(dest)+8,boxed);
        return;
    }
    auto [actual,data]=unwrapped(source,value);auto* owner=bolt::memory::owner_of(dest);bolt::memory::Unique<Box> copy(bolt::memory::make<Box>(owner,actual,data,owner));drop(target,dest);write(dest,target);write(static_cast<std::byte*>(dest)+8,copy.release());
});}
void* br_unbox(BrContext* c,const char* site,void* self)noexcept{void* result=nullptr;checked(c,site,[&]{
    if(!self)throw std::runtime_error("null pointer dereference");auto* t=read<BrType*>(self);
    if(!allocation_owner(impl(c)).type_addresses.contains(t))throw std::runtime_error("interface unbox requires an interface receiver");
    result=const_cast<void*>(unwrapped(t,self).second);
});return result;}
void br_value_contract(BrContext* c,BrType* type,void* eq,void* hash_fn)noexcept{
    checked(c,nullptr,[&]{type->context=c;type->equal_value=reinterpret_cast<std::uint8_t(*)(BrContext*,const void*,const void*)>(eq);type->hash_value=reinterpret_cast<std::uint64_t(*)(BrContext*,const void*)>(hash_fn);});
}
void br_formatter(BrContext* c,BrType* type,void* fn)noexcept{checked(c,nullptr,[&]{type->format=reinterpret_cast<void(*)(BrContext*,void*,void*)>(fn);});}
void br_zero(void* p,std::uint64_t n)noexcept{std::memset(p,0,static_cast<std::size_t>(n));}
void br_drop(BrType* t,void* p)noexcept{drop(t,p);}
void br_reset(BrType* t,void* p)noexcept{if(t&&t->kind==Struct)std::memset(p,0,t->size);else drop(t,p);}
void br_copy(BrContext* c,const char* site,BrType* t,void* d,const void* s)noexcept{checked(c,site,[&]{mutated(c,d);assign(t,d,s);});}
void* br_new(BrContext* c,const char* site,BrType* t)noexcept{
    void* result=nullptr;
    checked(c,site,[&]{
        auto* p=::operator new(t->size);std::memset(p,0,t->size);
        try{auto& owner=allocation_owner(impl(c));std::lock_guard lock(owner.allocation_mutex);owner.allocations.emplace(p,t);}
        catch(...){::operator delete(p);throw;}
        result=p;
    });
    return result;
}
void br_arena_create(BrContext* c,const char* site,void* slot,std::uint64_t initial,std::uint64_t maximum,std::int32_t grow)noexcept{checked(c,site,[&]{
    auto owner=std::make_unique<bolt::memory::Arena>(bolt::native::cast<std::size_t>(initial),bolt::native::cast<std::size_t>(maximum),grow!=0);
    write(slot,owner.release());
});}
void br_arena_action(BrContext* c,const char* site,void* slot,std::int32_t dispose)noexcept{checked(c,site,[&]{auto* owner=read<bolt::memory::Arena*>(slot);if(!owner)throw std::runtime_error("uninitialized arena");if(dispose)owner->dispose();else owner->reset();});}
std::uint64_t br_arena_stat(BrContext* c,const char* site,void* slot,std::int32_t property)noexcept{std::uint64_t result=0;checked(c,site,[&]{auto* owner=read<bolt::memory::Arena*>(slot);if(!owner)throw std::runtime_error("uninitialized arena");result=property==0?owner->used():property==1?owner->capacity():property==2?owner->peak():owner->limit();});return result;}
void* br_arena_new(BrContext* c,const char* site,void* slot,BrType* t)noexcept{void* result=nullptr;checked(c,site,[&]{auto* owner=read<bolt::memory::Arena*>(slot);if(!owner)throw std::runtime_error("uninitialized arena");auto* p=owner->allocate(t->size,8);std::memset(p,0,t->size);owner->add_root(p,[t](void* object){drop(t,object);});result=p;});return result;}
void br_delete(BrContext* c,const char* site,void* p)noexcept{
    checked(c,site,[&]{
        if(!p)return;
        if(bolt::memory::owner_of(p))throw std::runtime_error("delete cannot release an arena allocation; use Reset or Dispose");
        auto* allocated_type=remove_allocation(impl(c),p);
        if(!allocated_type)throw std::runtime_error("delete requires a live allocation created with new");
        drop(allocated_type,p);::operator delete(p);
    });
}
void br_string(BrContext* c,const char* site,void* dest,const char* bytes,std::uint64_t n)noexcept{checked(c,site,[&]{
    if(n&&!bytes)throw bolt::library::ArgumentNullException("UTF-8 bytes cannot be null when length is nonzero.");
    put_text(dest,n?std::string_view(bytes,static_cast<std::size_t>(n)):std::string_view{});
});}
std::uint32_t br_string_at(BrContext* c,const char* site,const void* slot,std::int32_t index) noexcept {
    std::uint32_t result = 0;
    checked(c,site,[&]{ result = text_at(slot,index); });
    return result;
}
void br_edit_utf8_byte(BrContext* c, const char* site, void* dest, std::uint64_t offset, std::uint8_t byte) noexcept {
    checked(c,site,[&]{
        auto* previous = read<Text*>(dest);
        if (!previous || offset >= previous->view().size()) throw std::runtime_error("index out of range");
        auto* owner = bolt::memory::owner_of(dest);
        if (!owner && !c->active_ranges && previous->replace_ascii_byte(offset,byte)) return;
        // Preserve the old value if the edit invalidates UTF-8 or allocation fails.
        std::string bytes(previous->view());
        bytes[offset] = static_cast<char>(byte);
        auto* next = Text::from_utf8(owner,previous,bytes,offset,offset+1);
        bolt::memory::destroy(previous);
        write(dest,next);
        mutated(c,dest);
    });
}
void br_replace_utf8_edits(BrContext* c, const char* site, void* dest, const void* array,
                           std::uint64_t first, std::uint64_t end) noexcept {
    checked(c,site,[&]{ replace_utf8(impl(c), dest, array, first, end); });
}
const char* br_string_data(const void* slot,std::uint64_t* size)noexcept{auto* value=read<Text*>(slot);*size=value?value->view().size():0;return *size?value->expose():"";}
void br_text(BrContext* c,const char* site,void* dest,BrType* t,const void* value)noexcept{checked(c,site,[&]{
    if (t->kind == String) { assign(t,dest,value); return; }
    if ((t->kind == Signed || t->kind == Unsigned) && t->name != "char") {
        // The longest representation is 20 bytes, including a possible minus sign.
        char buffer[20];
        numeric(t, [&]<class T>() {
            if constexpr (std::is_integral_v<T>) {
                const auto end = std::to_chars(buffer, buffer + sizeof(buffer), read<T>(value)).ptr;
                const std::string_view digits(buffer, static_cast<std::size_t>(end - buffer));
                put_text(dest, digits)->remember_validated_length(digits.size());
            }
        });
        return;
    }
    auto rendered=formatted_text(c,t,value);if(!c->failed)put_text(dest,std::move(rendered));
});}
void br_concat(BrContext* c,const char* site,void* dest,const void* a,const void* b)noexcept{checked(c,site,[&]{put_text(dest,text(a),text(b));});}
std::int32_t br_compare(BrContext* c,const char* site,BrType* t,const void* a,const void* b,std::int32_t op)noexcept{
    if(c && !c->failed && t->kind==String && !t->equal_value && (op==0 || op==1))
        return (text(a)==text(b)) ^ (op==1);
    std::int32_t result=0;checked(c,site,[&]{if(op==0||op==1){result=(equal(t,a,b)^(op==1))?1:0;return;}if(t->kind!=String)throw std::runtime_error("ordered comparison is not defined for this type");auto v=text(a).compare(text(b));result=(op==2?v<0:op==3?v<=0:op==4?v>0:v>=0)?1:0;});return result;}
void bolt_write_stream(BrContext* c,const char* site,const void* s,std::int32_t newline,std::ostream& fallback) noexcept {
    checked(c,site,[&]{auto& out=impl(c).output?*impl(c).output:fallback;out<<text(s);if(newline)out<<'\n';});
}
void bolt_read_stream(BrContext* c,const char* site,void* dest,std::istream& fallback) noexcept {
    checked(c,site,[&]{std::string s;std::getline(impl(c).input?*impl(c).input:fallback,s);put_text(dest,std::move(s));});
}
#if !defined(BOLT_WINDOWS_AOT_RUNTIME)
void br_write(BrContext* c,const char* site,const void* s,std::int32_t newline) noexcept {bolt_write_stream(c,site,s,newline,std::cout);}
void br_read(BrContext* c,const char* site,void* dest) noexcept {bolt_read_stream(c,site,dest,std::cin);}
#endif
std::int32_t br_count(BrContext* c,const char* site,BrType* t,const void* p)noexcept{std::int32_t result=0;checked(c,site,[&]{result=bolt::native::cast<std::int32_t>(size(t,p));});return result;}
void br_cast(BrContext* c,const char* site,BrType* target,void* dest,BrType* source,const void* p)noexcept{checked(c,site,[&]{if(target->name=="char"){numeric(source,[&]<class U>(){write(dest,bolt::native::cast<char32_t>(read<U>(p)));});return;}
    numeric(target,[&]<class T>(){numeric(source,[&]<class U>(){write(dest,bolt::native::cast<T>(read<U>(p)));});});});}
void br_parse(BrContext* c,const char* site,BrType* target,void* dest,const void* input)noexcept{checked(c,site,[&]{numeric(target,[&]<class T>(){write(dest,bolt::native::parse_number<T>(text(input)));});});}
std::int32_t br_try_parse(BrContext* c,const char* site,BrType* target,void* dest,const void* input)noexcept{
    std::int32_t result=0;
    checked(c,site,[&]{numeric(target,[&]<class T>(){T value{};if(bolt::native::try_parse_number<T>(text(input),value)){write(dest,value);result=1;}});});
    return result;
}
void br_array(BrContext* c,const char* site,BrType* t,void* dest,std::uint64_t n,std::int32_t init)noexcept{checked(c,site,[&]{auto* owner=bolt::memory::owner_of(dest);bolt::memory::Unique<Sequence> a(bolt::memory::make<Sequence>(owner,t->key,owner));a->reserve(bolt::native::cast<std::size_t>(n));a->count=static_cast<std::size_t>(n);if(init && t->key->initialize)for(std::size_t i=0;i<a->count&&!c->failed;++i)initialize(c,t->key,a->at(i));if(c->failed)return;drop(t,dest);write_sequence(dest,a.release());});}
void br_array_shape(BrContext* c,const char* site,BrType* t,void* dest,const std::uint64_t* dimensions,std::uint64_t rank,std::int32_t init)noexcept{checked(c,site,[&]{
    if(t->kind!=Array||rank!=t->rank||!dimensions)throw std::runtime_error("incorrect array rank");
    auto* owner=bolt::memory::owner_of(dest);
    bolt::memory::Unique<Sequence> a(bolt::memory::make<Sequence>(owner,t->key,owner));
    bool empty=false;
    for(std::size_t d=0;d<t->rank;++d){if(dimensions[d]>std::numeric_limits<std::size_t>::max())throw std::runtime_error("array size is too large");a->dimensions.push_back(static_cast<std::size_t>(dimensions[d]));empty|=dimensions[d]==0;}
    std::size_t count=empty?0:1;
    if(!empty)for(auto length:a->dimensions){if(count>std::numeric_limits<std::size_t>::max()/length)throw std::runtime_error("array size is too large");count*=length;}
    if(t->key->size&&count>static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())/t->key->size)throw std::runtime_error("array size is too large");
    a->reserve(count);a->count=count;
    if(init && t->key->initialize)for(std::size_t i=0;i<count&&!c->failed;++i)initialize(c,t->key,a->at(i));
    if(c->failed)return;
    drop(t,dest);write_sequence(dest,a.release());
});}
std::int32_t br_capacity(BrContext* c,const char* site,BrType* t,const void* slot)noexcept{
    std::int32_t result=0;checked(c,site,[&]{if(t->kind!=List)throw std::runtime_error("capacity requires a raw buffer");auto* value=read_sequence(slot);result=bolt::native::cast<std::int32_t>(value?value->capacity:0);});return result;
}
std::uint64_t br_value_hash(BrContext* c,const char* site,BrType* t,const void* value)noexcept{
    if (c && !c->failed && !t->hash_value && !t->equal_value) {
        if (t->kind == String) {
            const auto* text = read<Text*>(value);
            return text ? text->hash() : std::hash<std::string_view>{}({});
        }
        // Fixed widths let the compiler unroll the same byte-wise hash used by hash().
        if (t->kind == Signed || t->kind == Unsigned) {
            switch (t->size) {
            case 1: return hash_bytes(value, 1);
            case 2: return hash_bytes(value, 2);
            case 4: return hash_bytes(value, 4);
            case 8: return hash_bytes(value, 8);
            }
        }
    }
    std::uint64_t result=0;checked(c,site,[&]{result=static_cast<std::uint64_t>(hash(t,value));});return result;
}
std::int32_t br_array_length(BrContext* c,const char* site,BrType* t,const void* slot,std::int64_t dimension)noexcept{std::int32_t result=0;checked(c,site,[&]{if(t->kind!=Array)throw std::runtime_error("array required");auto d=index(dimension,t->rank);result=bolt::native::cast<std::int32_t>(array_length(t,read_sequence(slot),d));});return result;}
void* br_array_index(BrContext* c,const char* site,BrType* t,void* slot,const std::int64_t* indices,std::uint64_t rank)noexcept{void* result=nullptr;checked(c,site,[&]{
    if(t->kind!=Array||rank!=t->rank||!indices)throw std::runtime_error("incorrect array rank");
    auto* value=read_sequence(slot);std::size_t flat=0;
    for(std::size_t d=0;d<t->rank;++d){auto length=array_length(t,value,d);flat=flat*length+index(indices[d],length);}
    result=value->at(flat);
});return result;}
void* br_index(BrContext* c,const char* site,BrType* t,void* slot,std::int64_t i)noexcept{void* result=nullptr;checked(c,site,[&]{auto& s=sequence(t,slot);result=s.at(index(i,s.count));});return result;}
void* br_key(BrContext* c,const char* site,BrType* t,void* slot,const void* key,std::int32_t create)noexcept{void* result=nullptr;checked(c,site,[&]{auto& d=container<Dict>(slot);Box k(t->key,key);auto it=d.find(k);if(it==d.end()){if(!create)throw std::runtime_error("dictionary key not found");Box v(t->value);initialize(c,t->value,v.data());if(c->failed)return;auto* owner=bolt::memory::owner_of(slot);it=d.emplace(Box(t->key,key,owner),Box(t->value,v.data(),owner)).first;}result=it->second.data();});return result;}
void br_buffer_append(BrContext* c,const char* site,BrType* t,void* slot,const void* array,std::int32_t offset,std::int32_t count)noexcept{
    checked(c,site,[&]{
        const auto* input=read_sequence(array);const auto length=input?input->count:0;
        if(offset<0||count<0||static_cast<std::size_t>(offset)>length||static_cast<std::size_t>(count)>length-static_cast<std::size_t>(offset))throw std::runtime_error("invalid buffer source range");
        if(!count)return;
        auto& output=sequence(t,slot);const auto begin=output.count;
        if(begin > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - count))throw std::runtime_error("buffer size overflow");
        output.grow_for(begin+count);mutated(c,slot);
        if (output.bitwise) {
            std::memcpy(output.at(begin),input->at(offset),static_cast<std::size_t>(count)*t->key->size);output.count+=count;
        }else {
            try{for(std::int32_t i=0;i<count;++i){clone(t->key,output.at(output.count),input->at(offset+i),output.owner);++output.count;}}
            catch(...){drop(t->key,output.at(output.count));while(output.count>begin){--output.count;drop(t->key,output.at(output.count));}throw;}
        }
    });
}
static void take_value(BrContext* c,BrType* t,void* slot,void* source) {
    if (slot == source) return;
    mutated(c,slot);
    const auto* data = read<void*>(source);
    if (data && bolt::memory::owner_of(data) != bolt::memory::owner_of(slot)) {
        assign(t,slot,source);
    } else {
        drop(t,slot);
        relocate(t,slot,source);
    }
}
#if defined(BOLT_WINDOWS_AOT_RUNTIME)
// Keep the generic collection dispatcher out of static programs which only
// transfer ownership of string and array returns.
extern "C" void br_static_take_value(BrContext* c,const char* site,BrType* type,void* slot,void* source) noexcept {
    checked(c,site,[&]{take_value(c,type,slot,source);});
}
#endif
void br_collection(BrContext* c,const char* site,BrType* t,void* slot,const char* name,void* result,void* a,void* b)noexcept{checked(c,site,[&]{
    if ((t->kind == String || t->kind == Array) && std::strcmp(name, "TakeValue") == 0) {
        take_value(c,t,slot,a);
        return;
    }
    if (t->kind == String && std::strcmp(name, "TryScalarView") == 0) {
        const auto* value = read<Text*>(slot);
        write(result, value ? value->scalar_view() : BrTextView{});
        return;
    }
    if (std::strcmp(name, "FindInteger") == 0) {
        if ((t->kind != Array && t->kind != List) || !t->key ||
            (t->key->kind != Signed && t->key->kind != Unsigned && t->key->kind != Bool))
            throw std::runtime_error("integer sequence required");
        const auto* seq = read_sequence(slot);
        const auto count = seq ? seq->count : 0;
        (void)bolt::native::cast<std::int32_t>(count); // Preserve Length's overflow check.
        std::size_t pos = count;
        if (count) switch (t->key->size) {
        case 1: if (auto* found = static_cast<const std::byte*>(std::memchr(seq->data, read<std::uint8_t>(a), count)))
                    pos = static_cast<std::size_t>(found - seq->data);
                break;
        case 2: pos = find_integer(seq->data, count, read<std::uint16_t>(a)); break;
        case 4: pos = find_integer(seq->data, count, read<std::uint32_t>(a)); break;
        case 8: pos = find_integer(seq->data, count, read<std::uint64_t>(a)); break;
        default: throw std::runtime_error("unsupported integer width");
        }
        write(result, pos == count ? std::int32_t(-1) : static_cast<std::int32_t>(pos));
        return;
    }
    std::string_view op(name);
    if (t->kind == List && op == "Add") {
        mutated(c, slot);
        sequence(t, slot).add(a);
        return;
    }
    if(op!="Contains"&&op!="ContainsKey"&&op!="IndexOf"&&op!="Peek"&&op!="TryGetValue")mutated(c,slot);
    auto boolean=[&](bool v){write(result,static_cast<std::uint8_t>(v));};
    if(t->kind==List&&op=="Swap"){
        auto* other=const_cast<void*>(a);if(!other)throw std::runtime_error("null storage swap receiver");
        if(slot==other)return;
        if(bolt::memory::owner_of(slot)==bolt::memory::owner_of(other)){
            auto* left=read_sequence(slot);auto* right=read_sequence(other);write_sequence(slot,right);write_sequence(other,left);
        }else{
            Box left(t,other,bolt::memory::owner_of(slot)),right(t,slot,bolt::memory::owner_of(other));
            drop(t,slot);drop(t,other);relocate(t,slot,left.data());relocate(t,other,right.data());
        }
        mutated(c,slot);mutated(c,other);return;
    }
    if(op=="Clear"){if(t->kind==List){auto* value=read_sequence(slot);if(value)value->clear();}else drop(t,slot);return;}
    if(t->kind==Dictionary){auto& d=container<Dict>(slot);Box k(t->key,a);auto it=d.find(k);
        if(op=="ContainsKey"){boolean(it!=d.end());return;}
        if(op=="Remove"){boolean(it!=d.end());if(it!=d.end())d.erase(it);return;}
        if(op=="TryGetValue"){boolean(it!=d.end());if(it!=d.end())assign(t->value,b,it->second.data());return;}
        if(op=="Add"){if(it!=d.end())throw std::runtime_error("duplicate dictionary key");auto* owner=bolt::memory::owner_of(slot);d.emplace(Box(t->key,a,owner),Box(t->value,b,owner));return;}
        if(op=="Set"){if(it==d.end()){auto* owner=bolt::memory::owner_of(slot);d.emplace(Box(t->key,a,owner),Box(t->value,b,owner));}else assign(t->value,it->second.data(),b);return;}
    }else if(t->kind==HashSet){auto& d=container<Set>(slot);Box v(t->key,a);
        if(op=="Add"){if(d.contains(v)){boolean(false);return;}boolean(d.insert(Box(t->key,a,bolt::memory::owner_of(slot))).second);return;}
        if(op=="Contains"){boolean(d.contains(v));return;}
        if(op=="Remove"){boolean(d.erase(v)!=0);return;}
    }else if(t->kind==Queue){auto& d=container<QueueValues>(slot);
        if(op=="Enqueue"){d.emplace_back(t->key,a,bolt::memory::owner_of(slot));return;}
        if(op=="Contains"){bool found=false;for(const auto& v:d)found|=equal(t->key,v.data(),a);boolean(found);return;}
        if(op=="TryDequeue"){boolean(!d.empty());if(!d.empty()){assign(t->key,a,d.front().data());d.pop_front();}return;}
        if(op=="Dequeue"||op=="Peek"){if(d.empty())throw std::runtime_error("queue is empty");assign(t->key,result,d.front().data());if(op=="Dequeue")d.pop_front();return;}
    }else if(t->kind==List){auto& d=sequence(t,slot);
        if(op=="Reserve"){const auto n=read<std::int32_t>(a);if(n<0)throw std::runtime_error("capacity must be nonnegative");d.reserve(static_cast<std::size_t>(n));return;}
        if(op=="Truncate"){const auto n=read<std::int32_t>(a);if(n<0||static_cast<std::size_t>(n)>d.count)throw std::runtime_error("invalid buffer truncation");d.truncate(static_cast<std::size_t>(n));return;}
        if (op == "ToArray") {
            auto& out = sequence(t, result);
            out.clear();
            out.reserve_snapshot(impl(c), d.count, 64*1024);
            if (!out.copy_trivial_from(d))
                for (std::size_t i = 0; i < d.count; ++i) out.add(d.at(i));
            out.dimensions = {out.count};
            return;
        }
        if(op=="Insert"){auto pos=index(read<std::int32_t>(a),d.count,true);d.insert(pos,b);return;}
        if(op=="RemoveAt"){d.erase(index(read<std::int32_t>(a),d.count));return;}
        std::size_t pos=0;while(pos<d.count&&!equal(t->key,d.at(pos),a))++pos;
        if(op=="Contains"){boolean(pos<d.count);return;}
        if(op=="IndexOf"){write(result,pos<d.count?bolt::native::cast<std::int32_t>(pos):std::int32_t(-1));return;}
        if(op=="Remove"){boolean(pos<d.count);if(pos<d.count)d.erase(pos);return;}
    }
    throw std::runtime_error("unknown native collection operation: "+std::string(op));
});}
#if !defined(BOLT_WINDOWS_AOT_RUNTIME)
std::int32_t br_runtime(BrContext* c,const char* site,std::int32_t mode,const void* source)noexcept{std::int32_t result=0;checked(c,site,[&]{
    auto& parent=impl(c);bolt::detail::RuntimeDepthGuard depth(*parent.budget,parent.options.max_runtime_depth);
    if(!depth)throw bolt::Error(location(site),"runtime execution nesting limit exceeded","BLT3000");
    const std::string s(text(source));
    bolt::PreprocessorOptions preprocessing;
    if(mode<3){
        if(mode==0)result=bolt::detail::execute_statements_nested(s,parent.options,preprocessing,std::cin,std::cout,parent.budget);
        else if(mode==1)result=bolt::detail::execute_program_nested(s,parent.options,preprocessing,std::cin,std::cout,parent.budget);
        else result=bolt::detail::execute_file_nested(bolt::path_from_utf8(s),parent.options,preprocessing,std::cin,std::cout,parent.budget);
    } else {
        bolt::JitOptions options;options.execution=parent.options;options.input=parent.input;options.output=parent.output;
        if(mode==3)result=bolt::detail::jit_statements_nested(s,std::move(options),parent.budget);
        else if(mode==4)result=bolt::detail::jit_program_nested(s,std::move(options),parent.budget);
        else result=bolt::detail::jit_file_nested(bolt::path_from_utf8(s),std::move(options),parent.budget);
    }
});return result;}
#endif
} // extern C
#if !defined(BOLT_WINDOWS_AOT_RUNTIME)
namespace bolt::native_abi {

void configure_execution(BrContext* c,const ExecutionOptions& options,detail::SharedExecutionBudget inherited){
    auto& state=impl(c);state.options=options;
    if(inherited){state.budget=std::move(inherited);state.owns_budget=false;}
}
void reset_execution(BrContext* c) noexcept {if(c&&impl(c).owns_budget)impl(c).budget->reset();}

void rethrow(BrContext* c){if(c&&c->failed){if(impl(c).error)throw bolt::Error(*impl(c).error);throw std::runtime_error("native execution failed without a diagnostic");}}
void arguments(BrContext* c,const std::vector<std::string>& args){
    for(const auto& v:args)(void)bolt::unicode::length(v);
    impl(c).arguments=args;
}
void streams(BrContext* c,std::istream* in,std::ostream* out){if(in)impl(c).input=in;if(out)impl(c).output=out;}
std::vector<native_backend::Symbol> symbols(){std::vector<native_backend::Symbol> s;
#define S(n) s.push_back({#n,reinterpret_cast<void*>(&n)})
S(br_string_data);S(br_string_at);S(br_replace_utf8_edits);S(br_edit_utf8_byte);
S(br_context_child);S(br_context_activate);S(br_context_registry);S(br_type_size);S(br_context_cleanup);S(br_shared_hooks);S(br_destructor);S(br_new_failed);S(br_base);S(br_raise);S(br_exception_match);S(br_catch_begin);S(br_catch_end);S(br_rethrow);S(br_error_suspend);S(br_error_restore);S(br_reflected_type);S(br_reflected_field);S(br_reflected_method);S(br_serialization_method);S(br_reflection);S(br_tree_object);S(br_library_schema);S(br_process_arguments);S(br_arguments);S(br_range);S(br_range_next);S(br_abi_version);S(br_library);S(br_create);S(br_destroy);S(br_finish);S(br_process_exit);S(br_fail);S(br_frame);S(br_tick);S(br_enter);S(br_leave);S(br_project_bind);S(br_project_lookup);S(br_type);S(br_field);S(br_elements);S(br_initializer);S(br_virtual);S(br_dispatch);S(br_box);S(br_unbox);S(br_formatter);S(br_value_contract);S(br_memory_copy);S(br_memory_move);S(br_memory_set);S(br_memory_compare);S(br_zero);S(br_drop);S(br_reset);S(br_copy);S(br_new);S(br_arena_create);S(br_arena_action);S(br_arena_stat);S(br_arena_new);S(br_delete);S(br_string);S(br_text);S(br_concat);S(br_compare);S(br_write);S(br_read);S(br_count);S(br_capacity);S(br_value_hash);S(br_cast);S(br_parse);S(br_try_parse);S(br_array);S(br_array_shape);S(br_array_length);S(br_array_index);S(br_index);S(br_key);S(br_buffer_append);S(br_collection);S(br_runtime);
#undef S
return s;}
} // namespace bolt::native_abi
#endif
