#pragma once
#include <cstddef>
namespace bolt::native_abi {
// Two 64-bit descriptor fields precede each inline C++ library value.
inline constexpr std::size_t library_value_offset=16;
template<class T> constexpr std::size_t library_size() {
    static_assert(alignof(T)<=8);
    return ((sizeof(T)+library_value_offset+7)/8)*8;
}
}
