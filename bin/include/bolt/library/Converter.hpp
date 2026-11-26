#pragma once
#include "Exceptions.hpp"
#include "../Native.hpp"
#include "../Unicode.hpp"
#include <algorithm>
#include <string_view>

namespace bolt::library {
    class Converter {
        static std::string_view trim(std::string_view s) {
            const auto white=[](unsigned char c) {
                return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f';
            };
            while(!s.empty()&&white(static_cast<unsigned char>(s.front())))s.remove_prefix(1);
            while(!s.empty()&&white(static_cast<unsigned char>(s.back())))s.remove_suffix(1);
            return s;
        }
        public:
        Converter()=delete;
        template<class T> static T To(const native::String& s) {
            const auto original=std::string_view(s.data(),s.size());
            const auto view=std::is_same_v<T,char32_t>?original:trim(original);
            if constexpr(std::is_same_v<T,bool>) {
                std::string lower(view);
                for(char& c:lower)if(c>='A'&&c<='Z')c=static_cast<char>(c-'A'+'a');
                if(lower=="true")return true;
                if(lower=="false")return false;
                throw FormatException("Expected 'true' or 'false'.");
            } else if constexpr(std::is_same_v<T,char32_t>) {
                try {
                    std::size_t offset=0;
                    const auto value=unicode::decode(view,offset);
                    if(offset!=view.size())throw FormatException("Expected exactly one Unicode character.");
                    return value;
                }
                catch(const Exception&) {
                    throw;
                }catch(const std::exception&) {
                    throw FormatException("Expected one valid Unicode character.");
                }
            } else if constexpr(std::is_same_v<T,native::String>)return s;
            else {
                T value {
                };
                if(native::try_parse_number<T>(view,value))return value;
                // Distinguish a syntactically invalid number from a valid number
                // which cannot fit the requested representation.
                auto number=view;
                if(!number.empty()&&number.front()=='+')number.remove_prefix(1);
                if(!number.empty()) {
                    std::from_chars_result parsed;
                    if constexpr(std::is_floating_point_v<T>)parsed=native::from_decimal(number,value);
                    else parsed=std::from_chars(number.data(),number.data()+number.size(),value,10);
                    if(parsed.ptr==number.data()+number.size()&&parsed.ec==std::errc::result_out_of_range)
                    throw OverflowException("The number is outside the target type's range.");
                    if constexpr(std::is_unsigned_v<T>) {
                        if(number.size()>1&&number.front()=='-'&&std::all_of(number.begin()+1,number.end(),[](char c) {
                            return c>='0'&&c<='9';
                        }))
                        throw OverflowException("A negative number cannot be represented by an unsigned type.");
                    }
                }
                throw FormatException("The text is not a valid decimal number.");
            }
        }
        template<class T> static bool TryTo(const native::String& s,T& result) {
            try {
                T value=To<T>(s);
                result=std::move(value);
                return true;
            }catch(const FormatException&) {
                return false;
            }catch(const OverflowException&) {
                return false;
            }
        }
        static bool ToBool(const native::String& s) {
            return To<bool>(s);
        }
        static char32_t ToChar(const native::String& s) {
            return To<char32_t>(s);
        }
        static std::int32_t ToInt(const native::String& s) {
            return To<std::int32_t>(s);
        }
        static std::int8_t ToInt8(const native::String& s) {
            return To<std::int8_t>(s);
        }
        static std::int16_t ToInt16(const native::String& s) {
            return To<std::int16_t>(s);
        }
        static std::int64_t ToInt64(const native::String& s) {
            return To<std::int64_t>(s);
        }
        static std::uint8_t ToUInt8(const native::String& s) {
            return To<std::uint8_t>(s);
        }
        static std::uint16_t ToUInt16(const native::String& s) {
            return To<std::uint16_t>(s);
        }
        static std::uint32_t ToUInt32(const native::String& s) {
            return To<std::uint32_t>(s);
        }
        static std::uint64_t ToUInt64(const native::String& s) {
            return To<std::uint64_t>(s);
        }
        static float ToFloat(const native::String& s) {
            return To<float>(s);
        }
        static double ToDouble(const native::String& s) {
            return To<double>(s);
        }
        template<class T> static native::String ToString(const T& value) {
            return native::text(value);
        }
    };
}
