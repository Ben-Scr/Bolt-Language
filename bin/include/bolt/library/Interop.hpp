#pragma once
#include "Exceptions.hpp"
#include "../Unicode.hpp"
#include <cstring>
#include <codecvt>
#include <locale>
namespace bolt::library {
    class NativeInterop {
        public:
        static native::String Argument(const char* value) {
            return CopyUtf8(value);
        }
#ifdef _WIN32
        static native::String Argument(const wchar_t* value) {
            if(!value)return {
            };
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
            return converter.to_bytes(value);
        }
#endif
        static const char* Utf8(const native::String& text) {
            if(text.find('\0')!=native::String::npos)throw ArgumentException("An embedded NUL cannot be passed to a null-terminated C string function.");
            (void)unicode::length(text);
            return text.c_str();
        }
        static native::String CopyUtf8(const char* value) {
            if(!value)return {
            };
            const std::string_view text(value);
            (void)unicode::length(text);
            return text;
        }
    };
}
