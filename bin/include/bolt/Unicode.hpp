#pragma once
#include <cstdint>
#include <cstring>
#include "detail/Utf8Block.hpp"
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bolt::unicode {
    inline bool scalar(char32_t c) noexcept {
        return c<=0x10FFFF && !(c>=0xD800&&c<=0xDFFF);
    }
    inline char32_t decode(std::string_view s,std::size_t& i) {
        if(i>=s.size())throw std::out_of_range("character index out of range");
        auto first=static_cast<unsigned char>(s[i++]);
        if(first<0x80)return first;
        unsigned count=first>=0xC2&&first<=0xDF?1:first>=0xE0&&first<=0xEF?2:first>=0xF0&&first<=0xF4?3:0;
        if(!count||s.size()-i<count)throw std::invalid_argument("invalid UTF-8 text");
        char32_t result=first&((1u<<(6-count))-1u);
        for(unsigned n=0;n<count;++n) {
            auto c=static_cast<unsigned char>(s[i++]);
            if((c&0xC0)!=0x80)throw std::invalid_argument("invalid UTF-8 continuation");
            result=(result<<6)|(c&0x3F);
        }
        if(!scalar(result)||(count==1&&result<0x80)||(count==2&&result<0x800)||(count==3&&result<0x10000))throw std::invalid_argument("invalid UTF-8 scalar");
        return result;
    }
    inline std::string encode(char32_t c) {
        if(!scalar(c))throw std::invalid_argument("invalid Unicode scalar value");
        std::string s;
        if(c<0x80)s+=static_cast<char>(c);
        else if(c<0x800) {
            s+=static_cast<char>(0xC0|(c>>6));
            s+=static_cast<char>(0x80|(c&0x3F));
        }
        else if(c<0x10000) {
            s+=static_cast<char>(0xE0|(c>>12));
            s+=static_cast<char>(0x80|((c>>6)&0x3F));
            s+=static_cast<char>(0x80|(c&0x3F));
        }
        else {
            s+=static_cast<char>(0xF0|(c>>18));
            s+=static_cast<char>(0x80|((c>>12)&0x3F));
            s+=static_cast<char>(0x80|((c>>6)&0x3F));
            s+=static_cast<char>(0x80|(c&0x3F));
        }
        return s;
    }
    // A high bit in any byte marks a non-ASCII scalar. memcpy permits an
    // unaligned, endian-independent test without reading beyond the string.
    inline bool eight_ascii(const char* data) noexcept {
        std::uint64_t bytes;
        std::memcpy(&bytes, data, sizeof(bytes));
        return (bytes & UINT64_C(0x8080808080808080)) == 0;
    }
    inline std::size_t length(std::string_view s) {
        std::size_t count = 0, i = 0;
        if constexpr (detail::has_utf8_blocks) {
            if (s.size() >= 16) {
                const int first = bolt_utf8_first_block_count(s.data());
                const auto blocks = s.size() / 16 - 1;
                std::size_t starts = 0;
                // A rejected prefix is replayed by decode below, preserving
                // the kind and order of malformed-input diagnostics.
                if (first >= 0 && (!blocks || bolt_utf8_scan_blocks(s.data()+16,blocks,&starts))) {
                    count = static_cast<std::size_t>(first) + starts;
                    i = (1 + blocks) * 16;
                }
                if (i && static_cast<unsigned char>(s[i-1]) >= 0x80) {
                    // The final block may split a scalar. Recheck its last
                    // leader and suffix with the ordinary bounds-checked decoder.
                    --i;
                    while ((static_cast<unsigned char>(s[i]) & 0xc0) == 0x80) --i;
                    --count;
                }
            }
        }
        while (i < s.size()) {
            if (s.size() - i >= 8 && eight_ascii(s.data() + i)) {
                i += 8;
                count += 8;
            } else {
                decode(s, i);
                ++count;
            }
        }
        return count;
    }
    inline std::size_t offset(std::string_view s, std::size_t index) {
        std::size_t i = 0;
        while (index) {
            if (index >= 8 && s.size() - i >= 8 && eight_ascii(s.data() + i)) {
                i += 8;
                index -= 8;
            } else {
                if (i == s.size()) throw std::out_of_range("string index out of range");
                decode(s, i);
                --index;
            }
        }
        return i;
    }
    inline char32_t at(std::string_view s,std::size_t index) {
        auto i=offset(s,index);
        return decode(s,i);
    }
    inline bool whitespace(char32_t c) noexcept {
        return (c>=9&&c<=13)||c==32||c==0x85||c==0xA0||c==0x1680||(c>=0x2000&&c<=0x200A)||c==0x2028||c==0x2029||c==0x202F||c==0x205F||c==0x3000;
    }
}
