#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
namespace bolt::library { class Exception; }
namespace bolt::native {
// Type operations for Bolt values that cross a type-erased generated call.
struct ErasedValue {
    std::size_t size;
    void (*copy)(void*,const void*);
    void (*drop)(void*);
    library::Exception* (*as_exception)(void*);
};
template<class T> const ErasedValue& erased_value() {
    static const ErasedValue ops{
        sizeof(T),
        [](void* to,const void* from){std::construct_at(static_cast<T*>(to),*static_cast<const T*>(from));},
        [](void* value){std::destroy_at(static_cast<T*>(value));},
        [](void* value) -> library::Exception* {
            if constexpr(std::is_base_of_v<library::Exception,T>)return static_cast<T*>(value);
            else return nullptr;
        }
    };
    return ops;
}
template<class T> std::uint8_t* erased_descriptor() {
    return reinterpret_cast<std::uint8_t*>(const_cast<ErasedValue*>(&erased_value<T>()));
}
}
