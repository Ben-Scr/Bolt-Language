#pragma once
#include <charconv>
#include <string_view>
#include <type_traits>
#include "detail/fast_float.h"

namespace bolt::native {
// Parse directly at the destination precision. Preserve the destination on error.
template<class T>
std::from_chars_result from_decimal(std::string_view input,T& output) {
    static_assert(std::is_same_v<T,float> || std::is_same_v<T,double>);
    if(input.empty())return {input.data(),std::errc::invalid_argument};
    const auto* first=input.data();
    const auto* last=first+input.size();
    T value{};
    const auto result=fast_float::from_chars(first,last,value,fast_float::chars_format::general);
    if(result.ec==std::errc{})output=value;
    return {result.ptr,result.ec};
}
}
