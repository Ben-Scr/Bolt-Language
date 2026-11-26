#pragma once
// Shared checked primitive operations. This header does not contain the interpreter.
#include "ArenaCpp.hpp"
#include "Unicode.hpp"
#include "Decimal.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <functional>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bolt::native {
// Adapt Bolt's fixed-width FFI values to the exact C/C++ parameter and
// result types declared by an included native header. C APIs often spell the
// same scalar ABI differently (for example uint32_t* vs Windows DWORD*, or
// uint8_t* vs char*). C++ rejects those pointer conversions even when their
// representation is ABI-identical.
//
// Deliberately do NOT reinterpret arbitrary struct pointers merely because two
// structs happen to have the same size. Foreign opaque structs should use their
// real [Native] C++ type; only scalar/enum pointees (plus void) are adaptable.
template<class T>
inline constexpr bool ffi_scalar_pointee_v=
    std::is_arithmetic_v<std::remove_cv_t<T>>||std::is_enum_v<std::remove_cv_t<T>>;

template<class To,class From> To ffi_pointer_cast(From value) {
    static_assert(std::is_pointer_v<To>&&std::is_pointer_v<From>);
    using DestinationElement=std::remove_pointer_t<To>;
    using SourceElement=std::remove_pointer_t<From>;
    constexpr bool source_const=std::is_const_v<SourceElement>;
    constexpr bool destination_const=std::is_const_v<DestinationElement>;
    constexpr bool source_volatile=std::is_volatile_v<SourceElement>;
    constexpr bool destination_volatile=std::is_volatile_v<DestinationElement>;
    static_assert(!source_const||destination_const,"Bolt native FFI cannot cast away const");
    static_assert(!source_volatile||destination_volatile,"Bolt native FFI cannot cast away volatile");
    using Destination=std::remove_cv_t<DestinationElement>;
    using Source=std::remove_cv_t<SourceElement>;
    if constexpr(std::is_void_v<Destination>||std::is_void_v<Source>) {
        return reinterpret_cast<To>(value);
    } else {
        static_assert(ffi_scalar_pointee_v<DestinationElement>&&ffi_scalar_pointee_v<SourceElement>,
            "Bolt native FFI only adapts ABI-equivalent scalar/enum pointers; use the exact native struct type");
        static_assert(sizeof(Destination)==sizeof(Source),"Bolt native FFI pointer pointee sizes differ");
        static_assert(alignof(Destination)==alignof(Source),"Bolt native FFI pointer pointee alignments differ");
        return reinterpret_cast<To>(value);
    }
}

template<class To,class From> To ffi_argument(From&& value) {
    using Source=std::remove_cvref_t<From>;
    if constexpr(std::is_convertible_v<From,To>) {
        return static_cast<To>(std::forward<From>(value));
    } else if constexpr(std::is_pointer_v<To>&&std::is_pointer_v<Source>) {
        return ffi_pointer_cast<To>(static_cast<Source>(value));
    } else if constexpr(std::is_pointer_v<To> && std::is_unsigned_v<Source> && sizeof(Source)>=sizeof(void*)) {
        return reinterpret_cast<To>(static_cast<std::uintptr_t>(value));
    } else if constexpr(std::is_unsigned_v<To> && sizeof(To)>=sizeof(void*) && std::is_pointer_v<Source>) {
        return static_cast<To>(reinterpret_cast<std::uintptr_t>(value));
    } else {
        return static_cast<To>(std::forward<From>(value));
    }
}

template<class To,class From> To ffi_result(From&& value) {
    using Source=std::remove_cvref_t<From>;
    if constexpr(std::is_convertible_v<From,To>) {
        return static_cast<To>(std::forward<From>(value));
    } else if constexpr(std::is_pointer_v<To>&&std::is_pointer_v<Source>) {
        return ffi_pointer_cast<To>(static_cast<Source>(value));
    } else if constexpr(std::is_pointer_v<To> && std::is_unsigned_v<Source> && sizeof(Source)>=sizeof(void*)) {
        return reinterpret_cast<To>(static_cast<std::uintptr_t>(value));
    } else if constexpr(std::is_unsigned_v<To> && sizeof(To)>=sizeof(void*) && std::is_pointer_v<Source>) {
        return static_cast<To>(reinterpret_cast<std::uintptr_t>(value));
    } else {
        return static_cast<To>(std::forward<From>(value));
    }
}

template<class R,class... Parameters,class... Arguments>
decltype(auto) ffi_call(R (*function)(Parameters...),Arguments&&... arguments) {
    static_assert(sizeof...(Parameters)==sizeof...(Arguments),"Bolt native FFI argument count mismatch");
    return function(ffi_argument<Parameters>(std::forward<Arguments>(arguments))...);
}

inline std::string text(const std::string&);
inline std::string text(const String&);
inline std::string text(char32_t);
inline std::string text(bool);
template<class T> std::string text(const T&);
template<class T> struct Hasher;
template<class T> class Optional {
    std::optional<T> storage_;
public:
    using value_type=T;
    Optional() noexcept=default;
    Optional(std::nullptr_t) noexcept {}
    explicit Optional(const T& value):storage_(value) {}
    explicit Optional(T&& value):storage_(std::move(value)) {}
    bool HasValue() const noexcept {return storage_.has_value();}
    T& Value() & {if(!storage_)throw std::runtime_error("optional value is empty");return *storage_;}
    const T& Value() const & {if(!storage_)throw std::runtime_error("optional value is empty");return *storage_;}
    T Value() && {if(!storage_)throw std::runtime_error("optional value is empty");return std::move(*storage_);}
    std::string BoltFormat() const {return storage_?text(*storage_):"null";}
    std::size_t BoltHash() const {return storage_?(Hasher<T>{}(*storage_)^std::size_t{0x6d2b79f5}):std::size_t{0x63bd};}
    bool operator==(const Optional& other) const {return storage_==other.storage_;}
    bool operator!=(const Optional& other) const {return !(*this==other);}
    bool operator==(std::nullptr_t) const noexcept {return !storage_;}
    bool operator!=(std::nullptr_t) const noexcept {return !!storage_;}
    friend bool operator==(std::nullptr_t,const Optional& value) noexcept {return !value.storage_;}
    friend bool operator!=(std::nullptr_t,const Optional& value) noexcept {return !!value.storage_;}
};
template<class T> struct optional_traits {static constexpr bool value=false;};
template<class T> struct optional_traits<Optional<T>> {static constexpr bool value=true;using element=T;};

// Retained for C++ callers and the br_parse/br_try_parse ABI of older binaries.
// New Bolt primitive parsing calls bind to Bolt.Runtime.NumericParsing.
template<class T> bool try_parse_number(std::string_view input, T& output) {
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
    auto space=[](char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f' || c=='\v'; };
    while (!input.empty() && space(input.front())) input.remove_prefix(1);
    while (!input.empty() && space(input.back())) input.remove_suffix(1);
    if (input.empty()) return false;

    // A leading plus is part of Bolt's grammar. Keep exactly one sign.
    if(input.front()=='+') {
        input.remove_prefix(1);
        if(input.empty() || input.front()=='-' || input.front()=='+')return false;
    }
    T parsed{};
    std::from_chars_result result;
    if constexpr (std::is_floating_point_v<T>)
        result=from_decimal(input,parsed);
    else result=std::from_chars(input.data(),input.data()+input.size(),parsed,10);
    if (result.ec!=std::errc{} || result.ptr!=input.data()+input.size()) return false;
    if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(parsed)) return false;
    output=parsed;
    return true;
}
template<class T> T parse_number(std::string_view input) {
    T result{};
    if (!try_parse_number(input,result)) throw std::runtime_error("invalid or out-of-range numeric text");
    return result;
}

// Preserve moves through identity conversions. The const-reference overload
// below still copies lvalues and performs checked numeric conversions.
template<class T,class U> requires (std::is_same_v<T,std::remove_cvref_t<U>> && !std::is_lvalue_reference_v<U>)
T cast(U&& value) noexcept(std::is_nothrow_constructible_v<T,U&&>) {return std::forward<U>(value);}

template<class T, class U> T cast(const U& value) {
    if constexpr (std::is_same_v<T, U>) {
        // Identity conversions preserve the exact value, including deliberate
        // NaN/infinity bit patterns created by Reinterpret. Arithmetic is checked.
        return value;
    }
    else if constexpr(optional_traits<T>::value) {
        using Element=typename optional_traits<T>::element;
        if constexpr(std::is_same_v<U,std::nullptr_t>)return T{};
        else if constexpr(optional_traits<U>::value)return value.HasValue()?T(cast<Element>(value.Value())):T{};
        else return T(cast<Element>(value));
    }
    else if constexpr(std::is_same_v<T,char32_t>) {
        auto n=cast<std::uint32_t>(value);if(!unicode::scalar(n))throw std::runtime_error("char conversion out of Unicode range");return static_cast<char32_t>(n);
    } else if constexpr(std::is_same_v<U,char32_t> && (std::is_arithmetic_v<T> || std::is_enum_v<T>)) {
        return cast<T>(static_cast<std::uint32_t>(value));
    }
    else if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<U> &&
                       !std::is_same_v<T, bool> && !std::is_same_v<U, bool>) {
        if constexpr (std::is_integral_v<T> && std::is_integral_v<U>) {
            if (!std::in_range<T>(value)) throw std::runtime_error("integer conversion out of range");
        } else if constexpr (std::is_integral_v<T>) {
            // Exclusive power-of-two upper bound stays correct even on MSVC, where
            // long double has double precision. Never cast an out-of-range float.
            const long double x = std::trunc(static_cast<long double>(value));
            const long double upper = std::ldexp(1.0L, std::numeric_limits<T>::digits);
            const long double lower = std::is_signed_v<T> ? -upper : 0.0L;
            if (!std::isfinite(x) || x < lower || x >= upper)
                throw std::runtime_error("integer conversion out of range");
        }
        const T result = static_cast<T>(value);
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(result)) throw std::runtime_error("floating-point overflow");
        }
        return result;
    } else if constexpr (std::is_enum_v<T> && (std::is_integral_v<U> || std::is_enum_v<U>)) {
        return static_cast<T>(cast<std::underlying_type_t<T>>(value));
    } else if constexpr (std::is_enum_v<U> && std::is_arithmetic_v<T>) {
        return cast<T>(static_cast<std::underlying_type_t<U>>(value));
    } else if constexpr (std::is_convertible_v<U, T>) {
        return static_cast<T>(value);
    } else {
        static_assert(std::is_same_v<T, U>, "invalid Bolt conversion");
    }
}
template<class T> T finite(T value) {
    if constexpr (std::is_floating_point_v<T>)
        if (!std::isfinite(value)) throw std::runtime_error("floating-point overflow");
    return value;
}
// The full sum, difference or product of two signed 32-bit operands fits
// in 64 bits. Only the narrowing can fail, including INT_MIN * -1.
template<char Op,class Fail> std::int32_t checked_int32(std::int32_t a,std::int32_t b,Fail fail) {
    static_assert(Op=='+' || Op=='-' || Op=='*');
    const std::int64_t left=a;
    const std::int64_t result=Op=='+' ? left+b : Op=='-' ? left-b : left*b;
    if (!std::in_range<std::int32_t>(result)) return fail();
    return static_cast<std::int32_t>(result);
}
template<class T, class A, class B> T add(A av, B bv) {
    const T a = cast<T>(av), b = cast<T>(bv);
    if constexpr (std::is_same_v<T,std::int32_t>)
        return checked_int32<'+'>(a,b,[]()->T{throw std::runtime_error("integer overflow in +");});
    else if constexpr (std::is_integral_v<T>) {
        const auto lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
        if constexpr (std::is_unsigned_v<T>) {
            if (a > hi - b) throw std::runtime_error("integer overflow in +");
        } else if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b))
            throw std::runtime_error("integer overflow in +");
    }
    return finite<T>(static_cast<T>(a + b));
}
template<class T, class A, class B> T sub(A av, B bv) {
    const T a = cast<T>(av), b = cast<T>(bv);
    if constexpr (std::is_same_v<T,std::int32_t>)
        return checked_int32<'-'>(a,b,[]()->T{throw std::runtime_error("integer overflow in -");});
    else if constexpr (std::is_integral_v<T>) {
        const auto lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
        if constexpr (std::is_unsigned_v<T>) {
            if (a < b) throw std::runtime_error("unsigned underflow in -");
        } else if ((b > 0 && a < lo + b) || (b < 0 && a > hi + b))
            throw std::runtime_error("integer overflow in -");
    }
    return finite<T>(static_cast<T>(a - b));
}
template<class T, class A, class B> T mul(A av, B bv) {
    const T a = cast<T>(av), b = cast<T>(bv);
    if constexpr (std::is_same_v<T,std::int32_t>)
        return checked_int32<'*'>(a,b,[]()->T{throw std::runtime_error("integer overflow in *");});
    else if constexpr (std::is_integral_v<T>) {
        const auto lo = std::numeric_limits<T>::min(), hi = std::numeric_limits<T>::max();
        bool bad = false;
        if constexpr (std::is_unsigned_v<T>) bad = b != 0 && a > hi / b;
        else if (a > 0) bad = b > 0 ? a > hi / b : b < lo / a;
        else if (a < 0) bad = b > 0 ? a < lo / b : b < 0 && a < hi / b;
        if (bad) throw std::runtime_error("integer overflow in *");
    }
    return finite<T>(static_cast<T>(a * b));
}
template<class T, class A, class B> T div(A av, B bv) {
    const T a = cast<T>(av), b = cast<T>(bv);
    if (b == 0) throw std::runtime_error("division by zero");
    if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
        if (a == std::numeric_limits<T>::min() && b == T(-1))
            throw std::runtime_error("integer overflow in /");
    return finite<T>(static_cast<T>(a / b));
}
template<class T, class A, class B> T mod(A av, B bv) {
    static_assert(std::is_integral_v<T>);
    const T a = cast<T>(av), b = cast<T>(bv);
    if (b == 0) throw std::runtime_error("remainder by zero");
    if constexpr (std::is_signed_v<T>)
        if (a == std::numeric_limits<T>::min() && b == T(-1)) return 0;
    return static_cast<T>(a % b);
}
// Bit shifts have defined bit-pattern behavior and reject negative/oversized counts.
template<class T,class A,class B> T bitwise(std::string_view op,A av,B bv) {
    static_assert(std::is_integral_v<T>);using U=std::make_unsigned_t<T>;
    const T a=cast<T>(av);const U x=static_cast<U>(a);
    if(op=="<<"||op==">>") {
        if constexpr(std::is_signed_v<B>)if(bv<0)throw std::runtime_error("negative shift count");
        const auto count=static_cast<std::uint64_t>(bv);constexpr auto width=sizeof(T)*8;
        if(count>=width)throw std::runtime_error("shift count is outside the integer bit width");
        U r;
        if(op=="<<")r=static_cast<U>(x<<count);
        else {r=static_cast<U>(x>>count);if constexpr(std::is_signed_v<T>)if(a<0&&count)r|=static_cast<U>(~U{0}<<(width-count));}
        return std::bit_cast<T>(r);
    }
    const U y=static_cast<U>(cast<T>(bv));
    return std::bit_cast<T>(op=="&"?static_cast<U>(x&y):op=="|"?static_cast<U>(x|y):static_cast<U>(x^y));
}
inline std::string text(const std::string& v) { return v; }
inline std::string text(const String& v) { return static_cast<std::string>(v); }
inline std::string text(char32_t v) {return unicode::encode(v);}
inline std::string text(bool v) { return v ? "true" : "false"; }
inline std::string pointer_text(const void* pointer) {
    char digits[2*sizeof(std::uintptr_t)];
    const auto result=std::to_chars(std::begin(digits),std::end(digits),reinterpret_cast<std::uintptr_t>(pointer),16);
    return "0x"+std::string(digits,result.ptr);
}
template<class T> std::string text(const T& v) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_pointer_v<T> || requires { v.BoltFormat(); } || requires { v.ToString(); });
    if constexpr (std::is_pointer_v<T>)return pointer_text(v);
    else if constexpr (requires { v.BoltFormat(); }) return v.BoltFormat();
    else if constexpr(requires {v.ToString();})return static_cast<std::string>(v.ToString());
    else if constexpr (std::is_enum_v<T>) return text(static_cast<std::underlying_type_t<T>>(v));
    else if constexpr (std::is_integral_v<T>) {
        if constexpr (std::is_signed_v<T>) return std::to_string(static_cast<std::int64_t>(v));
        else return std::to_string(static_cast<std::uint64_t>(v));
    } else {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<T>::digits10) << v;
        return out.str();
    }
}
inline void write(const std::string& s) { std::cout << s; }
inline void write_line(const std::string& s = {}) { std::cout << s << '\n'; }
inline std::string read_line() { std::string s; std::getline(std::cin, s); return s; }
} // namespace bolt::native
