#pragma once
#include <utility>

namespace bolt::native_backend::detail {
// A generated Bolt function follows its declared machine ABI. Clang's
// function-type sanitizer otherwise reads its own metadata before the entry
// address, which can be the first byte of an executable page.
template<class Function, class... Args>
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
decltype(auto) call_generated(Function function, Args&&... args) {
    return function(std::forward<Args>(args)...);
}
}
