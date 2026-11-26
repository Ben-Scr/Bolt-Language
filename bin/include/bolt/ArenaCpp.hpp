#pragma once
#include "Arena.hpp"
#include "detail/RangeObserver.hpp"
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bolt::native {
// Allocators belong to the destination value. In particular, a value copied out
// of arena storage must not retain a buffer owned by the source arena.
template<class T> class DestinationAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::false_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::false_type;
    memory::Arena* arena = nullptr;

    DestinationAllocator() noexcept = default;
    explicit DestinationAllocator(memory::Arena* owner) noexcept : arena(owner) {}
    template<class U> DestinationAllocator(const DestinationAllocator<U>& other) noexcept : arena(other.arena) {}
    T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            throw std::runtime_error("arena allocation size overflow");
        if (arena) return static_cast<T*>(arena->allocate(count * sizeof(T), alignof(T)));
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        if (!arena) std::allocator<T>{}.deallocate(pointer, count);
    }
    DestinationAllocator select_on_container_copy_construction() const noexcept { return {}; }
    template<class U> bool operator==(const DestinationAllocator<U>& other) const noexcept { return arena == other.arena; }
};

class String : public std::basic_string<char, std::char_traits<char>, DestinationAllocator<char>> {
    using Base = std::basic_string<char, std::char_traits<char>, DestinationAllocator<char>>;
    static auto allocator(const void* destination) { return DestinationAllocator<char>(memory::owner_of(destination)); }
public:
    String() : Base(allocator(this)) {}
    String(const char* value) : Base(value, allocator(this)) {}
    String(const char* value, std::size_t count) : Base(value, count, allocator(this)) {}
    String(const std::string& value) : Base(value.data(), value.size(), allocator(this)) {}
    String(std::string_view value) : Base(value.data(), value.size(), allocator(this)) {}
    String(const String& value) : Base(value.data(), value.size(), allocator(this)) {}
    String(String&& value) : String(static_cast<const String&>(value)) {}
    String& operator=(const String& value) { Base::assign(value.data(), value.size()); detail::RangeObserver::changed(this); return *this; }
    String& operator=(String&& value) { return *this = static_cast<const String&>(value); }
    String& operator=(const std::string& value) { Base::assign(value.data(), value.size()); detail::RangeObserver::changed(this); return *this; }
    String& operator=(const char* value) { Base::assign(value); detail::RangeObserver::changed(this); return *this; }
    operator std::string() const { return std::string(data(), size()); }
    std::size_t BoltHash() const { return std::hash<std::string_view>{}(std::string_view(data(), size())); }
    friend bool operator==(const String& a, const String& b) { return a.compare(b) == 0; }
    friend auto operator<=>(const String& a, const String& b) { return a.compare(b) <=> 0; }
};

template<class T, class... Args> T* owned_new(memory::Arena* arena, Args&&... args) {
    if (!arena) return new T(std::forward<Args>(args)...);
    return ::new (arena->allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
}

template<class T, class... Args> T* arena_new(memory::Arena& arena, Args&&... args) {
    T* value = owned_new<T>(&arena, std::forward<Args>(args)...);
    try { arena.add_root(value, [](void* p) { static_cast<T*>(p)->~T(); }); }
    catch (...) { value->~T(); throw; }
    return value;
}

template<class T> void checked_delete(T* pointer) {
    if (pointer && memory::owner_of(pointer))
        throw std::runtime_error("cannot delete an arena allocation; reset or dispose its arena");
    delete pointer;
}
} // namespace bolt::native
