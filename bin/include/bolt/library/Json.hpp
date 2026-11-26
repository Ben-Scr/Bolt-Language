#pragma once
#include "Reflection.hpp"
#include "Time.hpp"
#include "Strings.hpp"
#include "detail/JsonValue.hpp"
#include <bit>
#include <cstring>
#include <limits>
#include <set>
#include <type_traits>

namespace bolt::library {
    namespace json_detail {
        template<class T> struct SourceContainer { static constexpr int Kind=0; };
        template<class T> struct Container {
            static constexpr int Kind=0;
        };
        template<class V> struct Container<native::Array<V>> {
            static constexpr int Kind=1;
            using Element=V;
        };
        template<class V> struct Container<native::List<V>> {
            static constexpr int Kind=2;
            using Element=V;
        };
        template<class V> struct Container<native::HashSet<V>> {
            static constexpr int Kind=3;
            using Element=V;
        };
        template<class V> struct Container<native::Queue<V>> {
            static constexpr int Kind=4;
            using Element=V;
        };
        template<class K,class V> struct Container<native::Dictionary<K,V>> {
            static constexpr int Kind=5;
            using Key=K;
            using Element=V;
        };
        template<class T> inline constexpr bool dependent_false=false;
        inline void depth_check(int depth) {
            if(depth>96)throw JsonException("JSON nesting exceeds 96 levels.");
        }
        inline std::string validated_string(const native::String& value) {
            (void)unicode::length(value);
            return std::string(value);
        }
        template<class T> Json encode(const T& value,int depth=0) {
            using V=std::remove_cvref_t<T>;
            using C=Container<V>;
            using S=SourceContainer<V>;
            depth_check(depth);
            if constexpr(native::optional_traits<V>::value)return value.HasValue()?encode(value.Value(),depth+1):Json(nullptr);
            else if constexpr(S::Kind==3)return encode(S::Export(value),depth+1);
            else if constexpr(std::is_same_v<V,bool>)return Json(value);
            else if constexpr(std::is_same_v<V,char32_t>)return Json(unicode::encode(value));
            else if constexpr(std::is_integral_v<V> && std::is_signed_v<V>)return Json(static_cast<std::int64_t>(value));
            else if constexpr(std::is_integral_v<V> && !std::is_signed_v<V>)return Json(static_cast<std::uint64_t>(value));
            else if constexpr(std::is_floating_point_v<V>)return Json(static_cast<double>(value));
            else if constexpr(std::is_enum_v<V>)return encode(static_cast<std::underlying_type_t<V>>(value),depth);
            else if constexpr(std::is_same_v<V,native::String>)return Json(validated_string(value));
            else if constexpr(std::is_same_v<V,TimeSpan>)return Json(value.Ticks());
            else if constexpr(std::is_same_v<V,DateTime>)return Json(validated_string(value.ToUniversalTime().ToString()));
            else if constexpr(S::Kind==1) {
                Json::Array array;const auto count=S::Count(value);array.reserve(static_cast<std::size_t>(count));
                for(std::int32_t i=0;i<count;++i)array.push_back(encode(S::At(value,i),depth+1));
                return array;
            }
            else if constexpr(S::Kind==2) {
                const auto count=S::Count(value);
                if constexpr(std::is_same_v<typename S::Key,native::String>) {
                    Json::Object result;
                    for(std::int32_t i=0;i<count;++i){auto pair=S::At(value,i);result.emplace(validated_string(S::GetKey(pair)),encode(S::GetValue(pair),depth+1));}
                    return result;
                }else{
                    Json::Array result;result.reserve(static_cast<std::size_t>(count));
                    for(std::int32_t i=0;i<count;++i){auto pair=S::At(value,i);result.emplace_back(Json::Object{{"Key",encode(S::GetKey(pair),depth+1)},{"Value",encode(S::GetValue(pair),depth+1)}});}
                    return result;
                }
            }
            else if constexpr(C::Kind>=1 && C::Kind<=4) {
                Json::Array a;
                a.reserve(static_cast<std::size_t>(value.Count()));
                for(const auto& item:value)a.push_back(encode(item,depth+1));
                return a;
            }
            else if constexpr(C::Kind==5) {
                if constexpr(std::is_same_v<typename C::Key,native::String>) {
                    Json::Object obj;
                    for(const auto& pair:value)obj.emplace(validated_string(pair.first),encode(pair.second,depth+1));
                    return obj;
                }
                else {
                    Json::Array pairs;
                    for(const auto& pair:value)pairs.emplace_back(Json::Object {
                        {
                            "Key",encode(pair.first,depth+1)
                        }, {
                            "Value",encode(pair.second,depth+1)
                        }
                    });
                    return pairs;
                }
            }
            else if constexpr(Metadata<V>::Reflected) {
                Json::Object obj;
                std::apply([&](const auto&... f) {
                    ([&] {
                        if(!f.JsonIgnore) {
                            if(!obj.emplace(f.JsonName,encode(f.Get(value),depth+1)).second)throw JsonException("Multiple reflected fields use the JSON name '"+std::string(f.JsonName)+"'.");
                        }
                    }
                    (),...);
                },Metadata<V>::Fields());
                return obj;
            }
            else throw JsonException("Serialization requires a supported value/collection type or a type marked [Reflect]. Raw pointers are not serialized.");
        }
        template<class T, class Input> T decode(const Input& input,int depth=0) {
            using V=std::remove_cvref_t<T>;
            using C=Container<V>;
            using S=SourceContainer<V>;
            depth_check(depth);
            if constexpr(native::optional_traits<V>::value)return input.is_null()?V{}:V(decode<typename native::optional_traits<V>::element>(input,depth+1));
            else if constexpr(S::Kind==3)return S::Import(decode<typename S::Payload>(input,depth+1));
            else if constexpr(std::is_same_v<V,bool>) {
                if(!input.is_bool())throw JsonException("Expected JSON true or false.");
                return input.boolean();
            }
            else if constexpr(std::is_same_v<V,char32_t>) {
                if(!input.is_string())throw JsonException("Expected a JSON string for char.");
                auto text=input.string();
                if(unicode::length(text)!=1)throw JsonException("A char needs exactly one Unicode scalar.");
                return unicode::at(text,0);
            }
            else if constexpr(std::is_integral_v<V>) {
                if(input.is_integer())return native::cast<V>(input.integer());
                if(input.is_unsigned())return native::cast<V>(input.unsigned_integer());
                if(input.is_real()) {
                    auto n=input.real();
                    if(std::trunc(n)!=n)throw JsonException("A fractional JSON number cannot be deserialized as an integer.");
                    return native::cast<V>(n);
                }
                throw JsonException("Expected a JSON integer.");
            }
            else if constexpr(std::is_floating_point_v<V>) {
                auto n=input.number();
                if(!std::isfinite(n)||n>std::numeric_limits<V>::max()||n< -std::numeric_limits<V>::max())throw JsonException("JSON number is outside the target floating-point range.");
                return static_cast<V>(n);
            }
            else if constexpr(std::is_enum_v<V>)return static_cast<V>(decode<std::underlying_type_t<V>>(input,depth));
            else if constexpr(std::is_same_v<V,native::String>) {
                if(!input.is_string())throw JsonException("Expected a JSON string.");
                return input.string();
            }
            else if constexpr(std::is_same_v<V,TimeSpan>)return TimeSpan(decode<std::int64_t>(input,depth));
            else if constexpr(std::is_same_v<V,DateTime>)return DateTime::Parse(decode<native::String>(input,depth));
            else if constexpr(S::Kind==1) {
                if(!input.is_array())throw JsonException("Expected a JSON array.");
                if(input.array().size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw JsonException("Array exceeds the Bolt length limit.");
                V result;for(const auto& item:input.array())S::Add(result,decode<typename S::Element>(item,depth+1));return result;
            }
            else if constexpr(S::Kind==2) {
                V result;
                if constexpr(std::is_same_v<typename S::Key,native::String>){
                    if(!input.is_object())throw JsonException("Expected a JSON object for a string-key map.");
                    for(const auto& [k,v]:input.object())S::Add(result,native::String(k),decode<typename S::Element>(v,depth+1));
                }else{
                    if(!input.is_array())throw JsonException("Expected key/value pairs for a map.");
                    for(const auto& pair:input.array()){
                        if(!pair.is_object()||!pair.contains("Key")||!pair.contains("Value"))throw JsonException("Map entries require Key and Value.");
                        S::Add(result,decode<typename S::Key>(pair["Key"],depth+1),decode<typename S::Element>(pair["Value"],depth+1));
                    }
                }
                return result;
            }
            else if constexpr(C::Kind>=1 && C::Kind<=4) {
                if(!input.is_array())throw JsonException("Expected a JSON array.");
                if(input.array().size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw JsonException("Array length exceeds the Bolt limit.");
                V result;
                if constexpr(C::Kind==1)result=V::Create(input.array().size());
                std::size_t index=0;
                for(const auto& j:input.array()) {
                    auto item=decode<typename C::Element>(j,depth+1);
                    if constexpr(C::Kind==1)result.At(index++)=std::move(item);
                    else if constexpr(C::Kind==4)result.Enqueue(item);
                    else result.Add(item);
                }
                return result;
            }
            else if constexpr(C::Kind==5) {
                V result;
                if constexpr(std::is_same_v<typename C::Key,native::String>) {
                    if(!input.is_object())throw JsonException("Expected an object for Dictionary<string,T>.");
                    for(const auto& [k,v]:input.object())result.Add(native::String(k),decode<typename C::Element>(v,depth+1));
                }
                else {
                    if(!input.is_array())throw JsonException("Expected key/value pairs for the Dictionary.");
                    for(const auto& p:input.array()) {
                        if(!p.is_object()||!p.contains("Key")||!p.contains("Value"))throw JsonException("Dictionary entries require Key and Value.");
                        result.Add(decode<typename C::Key>(p["Key"],depth+1),decode<typename C::Element>(p["Value"],depth+1));
                    }
                }
                return result;
            }
            else if constexpr(Metadata<V>::Reflected) {
                if(!input.is_object())throw JsonException("Expected an object for the reflected type.");
                if constexpr(!std::is_default_constructible_v<V>)throw JsonException("Deserialization requires an accessible parameterless constructor.");
                else {
                    V result {
                    };
                    std::set<std::string> known;
                    std::apply([&](const auto&... f) {
                        ([&] {
                            if(!known.emplace(f.JsonName).second)throw JsonException("Duplicate reflected JSON name.");if(!f.JsonIgnore&&input.contains(f.JsonName)) {
                                using D=std::remove_cvref_t<decltype(f)>;using F=typename D::ValueType;
                                if(f.ReadOnly) {
                                    // Read-only properties are outputs (including computed properties), not writable storage.
                                    if constexpr(!D::IsProperty)throw JsonException("Cannot deserialize into a read-only field: "+std::string(f.Name));
                                } else f.Set(result,decode<F>(input[f.JsonName],depth+1));
                            }
                        }
                        (),...);
                    },Metadata<V>::Fields());
                    return result;
                }
            }
            else throw JsonException("Deserialization requires a supported value/collection type or a type marked [Reflect].");
        }
        // Bolt Binary JSON v1: B J S 1 + tagged little-endian values. This format is
        // versioned and deterministic; it is deliberately NOT BSON, CBOR or MessagePack.
        class Binary {
            std::vector<std::uint8_t> bytes_;
            std::size_t position_=0;
            void need(std::size_t n)const {
                if(n>bytes_.size()-position_)throw JsonException("Truncated Binary JSON.");
            }
            void u64(std::uint64_t n) {
                for(int i=0;i<8;++i) {
                    bytes_.push_back(static_cast<std::uint8_t>(n));
                    n>>=8;
                }
            }
            std::uint64_t read64() {
                need(8);
                std::uint64_t n=0;
                for(int i=0;i<8;++i)n|=static_cast<std::uint64_t>(bytes_[position_++])<<(i*8);
                return n;
            }
            void string(const std::string& s) {
                (void)unicode::length(s);
                u64(s.size());
                bytes_.insert(bytes_.end(),s.begin(),s.end());
            }
            std::string string() {
                auto n=read64();
                if(n>bytes_.size()-position_)throw JsonException("Invalid Binary JSON string length.");
                auto count=static_cast<std::size_t>(n);
                std::string s(reinterpret_cast<const char*>(bytes_.data()+position_),count);
                position_+=count;
                (void)unicode::length(s);
                return s;
            }
            void write(const Json& j,int depth) {
                depth_check(depth);
                if(j.is_null())bytes_.push_back(0);
                else if(j.is_bool())bytes_.push_back(j.boolean()?2:1);
                else if(j.is_integer()) {
                    bytes_.push_back(3);
                    u64(std::bit_cast<std::uint64_t>(j.integer()));
                }else if(j.is_unsigned()) {
                    bytes_.push_back(4);
                    u64(j.unsigned_integer());
                }else if(j.is_real()) {
                    bytes_.push_back(5);
                    u64(std::bit_cast<std::uint64_t>(j.real()));
                }else if(j.is_string()) {
                    bytes_.push_back(6);
                    string(j.string());
                }else if(j.is_array()) {
                    bytes_.push_back(7);
                    u64(j.array().size());
                    for(const auto& v:j.array())write(v,depth+1);
                }else {
                    bytes_.push_back(8);
                    u64(j.object().size());
                    for(const auto& [k,v]:j.object()) {
                        string(k);
                        write(v,depth+1);
                    }
                }
            }
            Json read(int depth) {
                depth_check(depth);
                need(1);
                auto tag=bytes_[position_++];
                switch(tag) {
                    case 0:return nullptr;
                    case 1:return false;
                    case 2:return true;
                    case 3:return Json(std::bit_cast<std::int64_t>(read64()));
                    case 4:return Json(read64());
                    case 5:return Json(std::bit_cast<double>(read64()));
                    case 6:return Json(string());
                    case 7: {
                        auto n=read64();
                        if(n>bytes_.size()-position_)throw JsonException("Invalid Binary JSON array length.");
                        Json::Array a;
                        a.reserve(static_cast<std::size_t>(n));
                        for(std::uint64_t i=0;i<n;++i)a.push_back(read(depth+1));
                        return a;
                    }
                    case 8: {
                        auto n=read64();
                        if(n>(bytes_.size()-position_)/9)throw JsonException("Invalid Binary JSON object length.");
                        Json::Object o;
                        for(std::uint64_t i=0;i<n;++i) {
                            auto key=string();
                            auto value=read(depth+1);
                            if(!o.emplace(std::move(key),std::move(value)).second)throw JsonException("Duplicate Binary JSON object key.");
                        }
                        return o;
                    }
                    default:throw JsonException("Unknown Binary JSON tag.");
                }
            }
            public:
            static native::Array<std::uint8_t> Encode(const Json& j) {
                Binary b;
                b.bytes_= {
                    'B','J','S',1
                };
                b.write(j,0);
                auto out=native::Array<std::uint8_t>::Create(b.bytes_.size());
                for(std::size_t i=0;i<b.bytes_.size();++i)out.At(i)=b.bytes_[i];
                return out;
            }
            static Json Decode(const native::Array<std::uint8_t>& input) {
                Binary b;
                b.bytes_.assign(input.begin(),input.end());
                b.need(4);
                if(b.bytes_[0]!='B'||b.bytes_[1]!='J'||b.bytes_[2]!='S'||b.bytes_[3]!=1)throw JsonException("Unknown Binary JSON signature or version.");
                b.position_=4;
                auto j=b.read(0);
                if(b.position_!=b.bytes_.size())throw JsonException("Trailing bytes after Binary JSON value.");
                return j;
            }
        };
    }
    class Json {
        template<class F> static decltype(auto) checked(F&& f) {
            try {
                return std::forward<F>(f)();
            }catch(const JsonException&) {
                throw;
            }catch(const std::exception& e) {
                throw JsonException(e.what());
            }
        }
        public:
        Json()=delete;
        template<class T> static native::String Serialize(const T& value) {
            return checked([&] {
                return native::String(json_detail::encode(value).dump());
            });
        }
        template<class T> static native::Array<std::uint8_t> SerializeBytes(const T& value) {
            return Strings::ToUtf8(Serialize(value));
        }
        template<class T> static native::Array<std::uint8_t> SerializeBinary(const T& value) {
            return checked([&] {
                return json_detail::Binary::Encode(json_detail::encode(value));
            });
        }
        template<class T> static T Deserialize(const native::String& text) {
            return checked([&] {
                return json_detail::decode<T>(json_detail::Json::parse(text));
            });
        }
        template<class T> static T Deserialize(const native::Array<std::uint8_t>& bytes) {
            return Deserialize<T>(Strings::FromUtf8(bytes));
        }
        template<class T> static T DeserializeBinary(const native::Array<std::uint8_t>& bytes) {
            return checked([&] {
                return json_detail::decode<T>(json_detail::Binary::Decode(bytes));
            });
        }
        static bool IsValid(const native::String& text) {
            try {
                (void)json_detail::Json::parse(text);
                return true;
            }catch(const std::exception&) {
                return false;
            }
        }
    };
}
