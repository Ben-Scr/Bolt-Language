#pragma once
#include "Exceptions.hpp"
#include "detail/UnicodeCase.hpp"
#include "../Std.hpp"
#include "../Unicode.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bolt::library {
    class Strings {
        static std::size_t offset(std::string_view s,std::int32_t index) {
            if(index<0)throw ArgumentOutOfRangeException("String indices must be non-negative.");
            return unicode::offset(std::string_view(s.data(),s.size()),static_cast<std::size_t>(index));
        }
        static native::String trim(std::string_view s,bool left,bool right) {
            std::size_t i=0,start=0,end=s.size();
            bool leading=true;
            std::size_t last_nonspace=0;
            while(i<s.size()) {
                auto begin=i;
                auto c=unicode::decode(s,i);
                bool space=unicode::whitespace(c);
                if(leading&&space) {
                    if(left)start=i;
                }else leading=false;
                if(!space)last_nonspace=i;
                (void)begin;
            }
            if(right)end=last_nonspace;
            if(end<start)end=start;
            return std::string_view(s.data()+start,end-start);
        }
        public:
        Strings()=delete;
        static std::int32_t Length(std::string_view s) {
            return native::cast<std::int32_t>(unicode::length(s));
        }
        static std::int32_t ByteLength(std::string_view s) {
            return native::cast<std::int32_t>(s.size());
        }
        static bool Contains(std::string_view s,std::string_view value) {
            return s.find(value)!=std::string_view::npos;
        }
        static bool Contains(std::string_view s,char32_t value) {
            return Contains(s,native::String(unicode::encode(value)));
        }
        static bool StartsWith(std::string_view s,std::string_view value) {
            return s.starts_with(std::string_view(value));
        }
        static bool EndsWith(std::string_view s,std::string_view value) {
            return s.ends_with(std::string_view(value));
        }
        static std::int32_t IndexOf(std::string_view s,std::string_view value,std::int32_t start=0) {
            auto begin=offset(s,start);
            auto p=s.find(value,begin);
            return p==std::string_view::npos?-1:native::cast<std::int32_t>(unicode::length(std::string_view(s.data(),p)));
        }
        static std::int32_t IndexOf(std::string_view s,char32_t value,std::int32_t start=0) {
            return IndexOf(s,native::String(unicode::encode(value)),start);
        }
        static std::int32_t LastIndexOf(std::string_view s,std::string_view value) {
            auto p=s.rfind(value);
            return p==std::string_view::npos?-1:native::cast<std::int32_t>(unicode::length(std::string_view(s.data(),p)));
        }
        static std::int32_t LastIndexOf(std::string_view s,char32_t value) {
            return LastIndexOf(s,native::String(unicode::encode(value)));
        }
        static native::String Substring(std::string_view s,std::int32_t start,std::int32_t count=-1) {
            auto from=offset(s,start);
            if(count<-1)throw ArgumentOutOfRangeException("Count must be -1 or non-negative.");
            auto tail=std::string_view(s.data()+from,s.size()-from);
            auto n=count==-1?tail.size():unicode::offset(tail,static_cast<std::size_t>(count));
            return tail.substr(0,n);
        }
        static native::String Remove(std::string_view s,std::int32_t start,std::int32_t count=-1) {
            auto from=offset(s,start);
            if(count<-1)throw ArgumentOutOfRangeException("Count must be -1 or non-negative.");
            auto n=count==-1?s.size()-from:unicode::offset(std::string_view(s.data()+from,s.size()-from),static_cast<std::size_t>(count));
            std::string result(s.data(),s.size());
            result.erase(from,n);
            return result;
        }
        static native::String Insert(std::string_view s,std::int32_t start,std::string_view value) {
            auto from=offset(s,start);
            std::string result(s.data(),s.size());
            result.insert(from,value.data(),value.size());
            return result;
        }
        static native::String Replace(std::string_view s,std::string_view oldValue,std::string_view newValue) {
            if(oldValue.empty())throw ArgumentException("Replace needs a non-empty old value.");
            std::string result;
            std::size_t start=0;
            for(;;) {
                auto pos=s.find(oldValue,start);
                if(pos==std::string_view::npos) {
                    result.append(s.data()+start,s.size()-start);
                    break;
                }
                result.append(s.data()+start,pos-start);
                result.append(newValue.data(),newValue.size());
                start=pos+oldValue.size();
            }
            return result;
        }
        static native::String Replace(std::string_view s,char32_t oldValue,char32_t newValue) {
            return Replace(s,native::String(unicode::encode(oldValue)),native::String(unicode::encode(newValue)));
        }
        static native::String Trim(std::string_view s) {
            return trim(s,true,true);
        }
        static native::String TrimStart(std::string_view s) {
            return trim(s,true,false);
        }
        static native::String TrimEnd(std::string_view s) {
            return trim(s,false,true);
        }
        template<std::size_t N> static native::String MapCase(std::string_view s,const std::array<unicode_case::Mapping,N>& table) {
            std::string result;
            result.reserve(s.size());
            std::size_t at=0;
            while(at<s.size()) {
                const auto scalar=unicode::decode(s,at);
                if(auto* m=unicode_case::find(table,scalar)) {
                    for(std::uint8_t n=0;n<m->count;++n)result+=unicode::encode(m->to[n]);
                }else result+=unicode::encode(scalar);
            }
            return result;
        }
        static native::String ToUpper(std::string_view s) {
            return MapCase(s,unicode_case::Upper);
        }
        static native::String ToLower(std::string_view s) {
            return MapCase(s,unicode_case::Lower);
        }
        static native::String PadLeft(std::string_view s,std::int32_t totalWidth,char32_t padding=U' ') {
            auto count=Length(s);
            if(totalWidth<0)throw ArgumentOutOfRangeException("Width must be non-negative.");
            if(totalWidth<=count)return s;
            auto pad=unicode::encode(padding);
            std::string result;
            for(auto i=count;i<totalWidth;++i)result+=pad;
            result.append(s.data(),s.size());
            return result;
        }
        static native::String PadRight(std::string_view s,std::int32_t totalWidth,char32_t padding=U' ') {
            auto count=Length(s);
            if(totalWidth<0)throw ArgumentOutOfRangeException("Width must be non-negative.");
            if(totalWidth<=count)return s;
            auto pad=unicode::encode(padding);
            std::string result(s.data(),s.size());
            for(auto i=count;i<totalWidth;++i)result+=pad;
            return result;
        }
        static native::Array<native::String> Split(std::string_view s,std::string_view separator,bool removeEmpty=false) {
            if(separator.empty())throw ArgumentException("Split needs a non-empty separator.");
            std::vector<native::String> parts;
            std::size_t start=0;
            for(;;) {
                auto pos=s.find(separator,start);
                auto end=pos==std::string_view::npos?s.size():pos;
                if(!removeEmpty||end!=start)parts.emplace_back(s.data()+start,end-start);
                if(pos==std::string_view::npos)break;
                start=pos+separator.size();
            }
            auto result=native::Array<native::String>::Create(parts.size());
            for(std::size_t i=0;i<parts.size();++i)result.At(i)=std::move(parts[i]);
            return result;
        }
        static native::Array<native::String> Split(std::string_view s,char32_t separator,bool removeEmpty=false) {
            return Split(s,native::String(unicode::encode(separator)),removeEmpty);
        }
        static native::Array<char32_t> ToCharArray(std::string_view s) {
            auto result=native::Array<char32_t>::Create(Length(s));
            std::size_t offset=0;
            for(std::int32_t i=0;i<result.Count();++i)result.At(i)=unicode::decode(s,offset);
            return result;
        }
        static native::Array<std::uint8_t> ToUtf8(std::string_view text) {
            if (text.empty()) return {};
            const auto* first = reinterpret_cast<const std::uint8_t*>(text.data());
            return native::Array<std::uint8_t>::FromRange(first,first + text.size());
        }
        static native::String FromUtf8(const native::Array<std::uint8_t>& bytes) {
            const auto count = static_cast<std::size_t>(bytes.Count());
            if (!count) return {};
            const auto* first = reinterpret_cast<const char*>(std::to_address(bytes.begin()));
            const std::string_view text(first,count);
            (void)unicode::length(text);
            return native::String(text);
        }
        static native::String Join(std::string_view separator,const native::Array<native::String>& values) {
            std::string s;
            for(std::int32_t i=0;i<values.Count();++i) {
                if(i)s.append(separator.data(),separator.size());
                auto& v=values.At(i);
                s.append(v.data(),v.size());
            }
            return s;
        }
        static bool IsNullOrEmpty(std::string_view value) {
            return value.empty();
        }
        static bool IsNullOrWhiteSpace(std::string_view value) {
            std::size_t i=0;
            while(i<value.size())if(!unicode::whitespace(unicode::decode(value,i)))return false;
            return true;
        }
        static bool Equals(std::string_view a,std::string_view b) {
            return a==b;
        }
        static std::int32_t Compare(std::string_view a,std::string_view b) {
            const auto n=a.compare(b);
            return n<0?-1:n>0?1:0;
        }
    };

    struct UnicodeRange {
        const native::String& value;
        native::detail::RangeObserver observer;
        explicit UnicodeRange(const native::String& text) : value(text), observer(&text) {}
        std::string_view checked_text() const {
            if (!observer.valid()) throw InvalidOperationException("collection was structurally modified during iteration");
            return {value.data(),value.size()};
        }
        struct Iterator {
            const UnicodeRange* range;
            std::size_t pos;
            mutable std::size_t next;
            char32_t operator*() const {
                next = pos;
                return unicode::decode(range->checked_text(),next);
            }
            Iterator& operator++() {
                const auto text = range->checked_text();
                if (next > pos) pos = next;
                else (void)unicode::decode(text,pos);
                return *this;
            }
            bool operator!=(const Iterator& other) const {
                (void)range->checked_text();
                return pos != other.pos;
            }
        };
        Iterator begin() const { return {this,0,0}; }
        Iterator end() const { return {this,value.size(),value.size()}; }
    };
    template<class R> R& range_view(R& value) {
        return value;
    }
    inline UnicodeRange range_view(const native::String& value) { return UnicodeRange(value); }
    inline UnicodeRange range_view(native::String& value) { return UnicodeRange(value); }
    // A Bolt foreach observes the collection object. Assignment may reuse an
    // arena allocation, so equal length and storage address do not prove the
    // collection has remained valid. Individual element writes stay legal.
    template<class Collection> struct IndexedRange {
        Collection& value;
        native::detail::RangeObserver observer;
        explicit IndexedRange(Collection& source) : value(source), observer(&source) {}
        void check() const {
            if (!observer.valid()) throw InvalidOperationException("collection was structurally modified during iteration");
        }
        struct Iterator {
            const IndexedRange* range;
            std::int32_t pos;
            decltype(auto) operator*() const { range->check(); return range->value.At(pos); }
            Iterator& operator++() { range->check(); ++pos; return *this; }
            bool operator!=(const Iterator& other) const { range->check(); return pos != other.pos; }
        };
        Iterator begin() const { return {this,0}; }
        Iterator end() const { return {this,value.Count()}; }
    };
    template<class Buffer> using BufferRange = IndexedRange<Buffer>;
    template<class Array> using ArrayRange = IndexedRange<Array>;
    template<class T> auto range_view(memory::Buffer<T>& value) { return BufferRange<memory::Buffer<T>>(value); }
    template<class T> auto range_view(const memory::Buffer<T>& value) { return BufferRange<const memory::Buffer<T>>(value); }
    template<class T> auto range_view(native::Array<T>& value) { return ArrayRange<native::Array<T>>(value); }
    template<class T> auto range_view(const native::Array<T>& value) { return ArrayRange<const native::Array<T>>(value); }

    template<class K,class V> struct KeyValuePair {
        K Key;
        V Value;
    };
    template<class K,class V> struct DictionaryRange {
        const native::Dictionary<K,V>& values;
        using BaseIterator=decltype(values.begin());
        struct Iterator {
            BaseIterator value;
            KeyValuePair<K,V> operator*()const {
                return {
                    value->first,value->second
                };
            }
            Iterator& operator++() {
                ++value;
                return *this;
            }
            bool operator!=(const Iterator& other)const {
                return value!=other.value;
            }
        };
        Iterator begin()const {
            return {
                values.begin()
            };
        }
        Iterator end()const {
            return {
                values.end()
            };
        }
    };
    template<class K,class V> DictionaryRange<K,V> range_view(const native::Dictionary<K,V>& values) {
        return {
            values
        };
    }
    template<class K,class V> DictionaryRange<K,V> range_view(native::Dictionary<K,V>& values) {
        return {
            values
        };
    }
}
