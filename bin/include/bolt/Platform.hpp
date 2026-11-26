#pragma once
// Small platform/type primitives used by source-authored Bolt libraries.
#include "Std.hpp"
#include "ValueTree.hpp"
#include "DirectoryOps.hpp"
#include "WriterLease.hpp"
#include "library/Exceptions.hpp"
#include "library/IO.hpp"
#include "library/Converter.hpp"
#include <chrono>
#include <bit>
#include <ctime>
#include <limits>
#include <cstdlib>
namespace bolt::platform {
struct ScalarConversion {
    template<class T> static bool Float(const native::String& text,T& value,std::int32_t& error) {
        try { auto result=library::Converter::To<T>(text); value=result;error=0;return true; }
        catch(const library::OverflowException&){error=2;return false;}
        catch(const library::FormatException&){error=1;return false;}
    }
};

// General string-storage access for Bolt-authored native libraries. The borrowed
// bytes remain owned by the string and must be treated as read-only.
struct NativeString {
    static std::uint8_t* Bytes(const native::String& value,std::uint64_t& length) noexcept {
        length=static_cast<std::uint64_t>(value.size());
        return reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data()));
    }
    static native::String FromUtf8(std::uint8_t* bytes,std::uint64_t length) {
        if(length==0)return {};
        if(!bytes)throw library::ArgumentNullException("UTF-8 bytes cannot be null when length is nonzero.");
        return native::String(reinterpret_cast<const char*>(bytes),native::cast<std::size_t>(length));
    }
};

// Native platform primitives used by source-authored Bolt libraries.
struct FileOps {
    static bool Exists(const native::String& path){return library::File::Exists(path);}
    static void Delete(const native::String& path){library::File::Delete(path);}
    static void Copy(const native::String& from,const native::String& to,bool overwrite){library::File::Copy(from,to,overwrite);}
    static void Move(const native::String& from,const native::String& to,bool overwrite){library::File::Move(from,to,overwrite);}
};
struct Clock {
    static std::int64_t MonotonicTicks(){return std::chrono::duration_cast<std::chrono::duration<std::int64_t,std::ratio<1,10000000>>>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    static std::int64_t UnixTicks(){return std::chrono::duration_cast<std::chrono::duration<std::int64_t,std::ratio<1,10000000>>>(std::chrono::system_clock::now().time_since_epoch()).count();}
};
struct TimeZone {
    static std::int64_t seconds(std::int64_t ticks){auto s=ticks/10000000;return ticks%10000000<0?s-1:s;}
    static std::tm local(std::int64_t seconds){std::tm result{};
#ifdef _WIN32
        const __time64_t time=seconds;if(_localtime64_s(&result,&time))throw library::ArgumentOutOfRangeException("Local time is outside the platform-supported range.");
#else
        const auto time=native::cast<std::time_t>(seconds);if(!localtime_r(&time,&result))throw library::ArgumentOutOfRangeException("Local time is outside the platform-supported range.");
#endif
        return result;
    }
    static std::int64_t OffsetSeconds(std::int64_t ticks){const auto stamp=seconds(ticks);auto value=local(stamp);
#ifdef _WIN32
        const auto wall=_mkgmtime64(&value);if(wall==-1)throw library::ArgumentOutOfRangeException("Local time conversion failed.");return wall-stamp;
#else
        return value.tm_gmtoff;
#endif
    }
    static std::int64_t LocalMidnightTicks(std::int64_t ticks){auto value=local(seconds(ticks));value.tm_hour=0;value.tm_min=0;value.tm_sec=0;value.tm_isdst=-1;
#ifdef _WIN32
        const auto result=_mktime64(&value);
#else
        const auto result=std::mktime(&value);
#endif
        // mktime's -1 also represents a valid instant. Verify by round-trip.
        const auto checked=local(static_cast<std::int64_t>(result));
        if(checked.tm_year!=value.tm_year||checked.tm_mon!=value.tm_mon||checked.tm_mday!=value.tm_mday)throw library::ArgumentOutOfRangeException("Local midnight is outside the platform-supported range.");
        return native::mul<std::int64_t>(static_cast<std::int64_t>(result),10000000);
    }
};
// Compatibility for previously generated br_library operations 166 and 167.
struct Entropy {
    static std::uint64_t* ThreadRandomState(){static thread_local std::uint64_t state=Seed();return &state;}
    static std::uint64_t Seed();
};
struct Bits {
    static std::uint64_t Add64(std::uint64_t a,std::uint64_t b)noexcept{return a+b;}
    static std::uint64_t Multiply64(std::uint64_t a,std::uint64_t b)noexcept{return a*b;}
};
struct Values {
    template<class T> static std::uint64_t Hash(const T& value){return static_cast<std::uint64_t>(bolt::native::Hasher<T>{}(value));}
    template<class T> static bool Equal(const T& a,const T& b){return bolt::native::value_equal(a,b);}
    template<class T> static bolt::native::String Format(const T& value){
        if constexpr(std::is_arithmetic_v<T>||std::is_enum_v<T>||std::is_pointer_v<T>||requires{value.BoltFormat();}||requires{value.ToString();}||std::is_convertible_v<T,std::string_view>)return bolt::native::String(bolt::native::text(value));
        else throw bolt::library::InvalidOperationException("This value has no string representation.");
    }
};
struct Failure {
    [[noreturn]] static void IO(bolt::native::String text){throw bolt::library::IOException(std::move(text));}
    [[noreturn]] static void DirectoryMissing(bolt::native::String text){throw bolt::library::DirectoryNotFoundException(std::move(text));}
    [[noreturn]] static void Argument(bolt::native::String text){throw bolt::library::ArgumentException(std::move(text));}
    [[noreturn]] static void Range(bolt::native::String text){throw bolt::library::ArgumentOutOfRangeException(std::move(text));}
    [[noreturn]] static void Index(bolt::native::String text){throw bolt::library::IndexOutOfRangeException(std::move(text));}
    [[noreturn]] static void MissingKey(bolt::native::String text){throw bolt::library::KeyNotFoundException(std::move(text));}
    [[noreturn]] static void InvalidOperation(bolt::native::String text){throw bolt::library::InvalidOperationException(std::move(text));}
    [[noreturn]] static void Overflow(bolt::native::String text){throw bolt::library::OverflowException(std::move(text));}
    [[noreturn]] static void Format(bolt::native::String text){throw bolt::library::FormatException(std::move(text));}
};
} // namespace bolt::platform

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
#include "HostedPlatform.hpp"
#endif
