#include <bolt/Value.hpp>
#include <bolt/Arena.hpp>
#include <bit>
#include <bolt/Unicode.hpp>
#include <bolt/Native.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
namespace bolt {
    namespace {
        thread_local std::vector<ValueArena*> value_arenas;
        ValueArena* value_owner(const void* p) {
            for(auto* arena:value_arenas)if(arena->owns(p))return arena;
            return nullptr;
        }
        template<class T> ValueOwned<T> owned_value(ValueArena* arena) {
            auto* storage=allocate_value_storage(arena,sizeof(T),alignof(T));
            try{return ValueOwned<T>(::new(storage) T(arena),ValueDeleter<T>{arena});}
            catch(...){deallocate_value_storage(arena,storage,alignof(T));throw;}
        }
    }
    struct ValueArena::Impl {
        memory::Arena arena;
        Impl(std::size_t initial,std::size_t limit):arena(initial,limit,initial!=limit){}
    };
    ValueArena::ValueArena(std::size_t initial,std::size_t limit):impl(std::make_unique<Impl>(initial,limit)){value_arenas.push_back(this);}
    ValueArena::~ValueArena(){impl->arena.dispose();std::erase(value_arenas,this);}
    void* ValueArena::allocate(std::size_t bytes,std::size_t alignment){return impl->arena.allocate(bytes,alignment);}
    void ValueArena::add_root(Value* value){impl->arena.add_root(value,[](void* p){static_cast<Value*>(p)->~Value();});}
    void ValueArena::reset(){impl->arena.reset();}
    void ValueArena::dispose(){impl->arena.dispose();}
    bool ValueArena::owns(const void* p)const{return impl->arena.owns(p);}
    std::uint64_t ValueArena::used()const{return impl->arena.used();}
    std::uint64_t ValueArena::capacity()const{return impl->arena.capacity();}
    std::uint64_t ValueArena::peak()const{return impl->arena.peak();}
    std::uint64_t ValueArena::limit()const{return impl->arena.limit();}
    void* allocate_value_storage(ValueArena* arena,std::size_t size,std::size_t alignment){
        return arena?arena->allocate(size,alignment):memory::allocate_bytes(nullptr,size,alignment);
    }
    void deallocate_value_storage(ValueArena* arena,void* p,std::size_t alignment)noexcept{
        if(!arena)memory::deallocate_bytes(p,alignment);
    }
    Value& unbox(Value& v) {
        auto* obj=std::get_if<ValueOwned<Object>>(&v.data);
        if(obj&&*obj&&(*obj)->capture_cell)return *(*obj)->capture_cell;
        return obj&&*obj&&(*obj)->interface_box?unbox((*obj)->fields.at("@value")):v;
    }
    const Value& unbox(const Value& v) {
        const auto* obj=std::get_if<ValueOwned<Object>>(&v.data);
        if(obj&&*obj&&(*obj)->capture_cell)return *(*obj)->capture_cell;
        return obj&&*obj&&(*obj)->interface_box?unbox((*obj)->fields.at("@value")):v;
    }
    Value::~Value() = default;
    static std::string_view string_view_of(const Value& value) {
        if (auto* text=std::get_if<ArenaString>(&value.data)) return {text->data,text->size};
        return std::get<std::string>(value.data);
    }
    static Value::Data copy_data(const Value& source,ValueArena* arena) {
        return std::visit([&](const auto& v) -> Value::Data {
            using T=std::decay_t<decltype(v)>;
            if constexpr(std::is_same_v<T,ValueOwned<Object>>) {
                auto object=owned_value<Object>(arena);
                object->interface_box=v->interface_box;
                object->capture_cell=v->capture_cell;
                object->dispatch_type=v->dispatch_type;
                for(const auto& [name,field]:v->fields)object->fields.emplace(name,field.copy_to(arena));
                return object;
            } else if constexpr(std::is_same_v<T,ValueOwned<Collection>>) {
                auto collection=owned_value<Collection>(arena);
                collection->dimensions.assign(v->dimensions.begin(),v->dimensions.end());
                collection->values.reserve(v->values.capacity());
                collection->entries.reserve(v->entries.size());
                for(const auto& field:v->values)collection->values.push_back(field.copy_to(arena));
                for(const auto& [key,field]:v->entries)collection->entries.emplace_back(key.copy_to(arena),field.copy_to(arena));
                return collection;
            } else if constexpr(std::is_same_v<T,std::string>||std::is_same_v<T,ArenaString>) {
                const auto text=string_view_of(source);
                if(arena) {
                    auto* bytes=static_cast<char*>(arena->allocate(text.size()+1,alignof(char)));
                    std::copy(text.begin(),text.end(),bytes);bytes[text.size()]='\0';
                    return ArenaString{bytes,text.size()};
                }
                return std::string(text);
            } else if constexpr(std::is_same_v<T,std::unique_ptr<ValueArena>>)throw std::runtime_error("Arena owners cannot be copied");
            else return v;
        },source.data);
    }
    Value Value::copy_to(ValueArena* arena)const {
        Value result;
        result.type=type;
        result.bit_pattern=bit_pattern;
        result.allocation_arena=arena;
        result.data=copy_data(*this,arena);
        return result;
    }
    Value::Value(const Value& other)
        : type(other.type),allocation_arena(value_owner(this)),bit_pattern(other.bit_pattern) {
        data=copy_data(other,allocation_arena);
    }
    Value& Value::operator=(const Value& other) {
        if (this != &other) {
            auto copy=copy_data(other,allocation_arena);
            type=other.type;
            bit_pattern=other.bit_pattern;
            data=std::move(copy);
            ++range_version;
        }
        return *this;
    }
    Value::Value(Value&& other) {
        auto* destination=value_owner(this);
        if(destination&&destination!=other.allocation_arena) {
            auto copy=other.copy_to(destination);
            type=std::move(copy.type);data=std::move(copy.data);bit_pattern=copy.bit_pattern;allocation_arena=destination;
        } else {
            type=std::move(other.type);data=std::move(other.data);bit_pattern=other.bit_pattern;allocation_arena=other.allocation_arena;
        }
    }
    Value& Value::operator=(Value&& other) {
        if (this != &other) {
            if(allocation_arena!=other.allocation_arena)return *this=static_cast<const Value&>(other);
            ++range_version;
            type=std::move(other.type);
            data=std::move(other.data);
            bit_pattern=other.bit_pattern;
        }
        return *this;
    }
    Value Value::signed_integer(const std::string& t, std::int64_t x) {
        Value v;
        v.type = t;
        v.data = x;
        return v;
    }
    Value Value::unsigned_integer(const std::string& t, std::uint64_t x) {
        Value v;
        v.type = t;
        v.data = x;
        return v;
    }
    Value Value::object_value(const std::string& t,ValueArena* arena) {
        Value v;
        v.type = t;
        v.allocation_arena=arena;
        v.data = owned_value<Object>(arena);
        v.object().dispatch_type=t;
        return v;
    }
    Value Value::collection_value(const std::string& t,ValueArena* arena) {
        Value v;
        v.type=t;
        v.allocation_arena=arena;
        v.data=owned_value<Collection>(arena);
        if(is_array(t))v.collection().dimensions.resize(array_rank(t),0);
        return v;
    }
    Value Value::arena_value(std::size_t initial,std::size_t limit) {
        Value v;v.type="Arena";v.data=std::make_unique<ValueArena>(initial,limit);return v;
    }
    ValueArena& Value::arena()const{return *std::get<std::unique_ptr<ValueArena>>(data);}
    Value Value::pointer_value(const std::string& t,Value* target) {
        Value v;
        v.type=t;
        v.data=Pointer {
            target,target?std::weak_ptr<char>(target->lifetime):std::weak_ptr<char> {
            }
        };
        return v;
    }
    Collection& Value::collection() {
        return *std::get<ValueOwned<Collection>>(data);
    }
    const Collection& Value::collection()const {
        return *std::get<ValueOwned<Collection>>(data);
    }
    Value& Value::dereference()const {
        const auto& p=std::get<Pointer>(data);
        if(!p.address)throw std::runtime_error("null pointer dereference");
        if(p.lifetime.expired())throw std::runtime_error("dangling pointer/reference");
        return *p.address;
    }
    Value Value::optional_value(const std::string& type,const Value* value,ValueArena* arena) {
        auto result=object_value(type,arena);
        if(value)result.object().fields.emplace("@value",value->copy_to(arena));
        return result;
    }
    bool Value::has_value()const {return is_optional(type)&&object().fields.contains("@value");}
    Value& Value::optional_payload() {if(!has_value())throw std::runtime_error("optional value is empty");return object().fields.at("@value");}
    const Value& Value::optional_payload()const {if(!has_value())throw std::runtime_error("optional value is empty");return object().fields.at("@value");}
    Object& Value::object() {
        return *std::get<ValueOwned<Object>>(data);
    }
    const Object& Value::object() const {
        return *std::get<ValueOwned<Object>>(data);
    }
    std::int64_t Value::as_int() const {
        if (const auto* v = std::get_if<std::int64_t>(&data)) return *v;
        if (const auto* v = std::get_if<std::uint64_t>(&data)) return native::cast<std::int64_t>(*v);
        throw std::runtime_error("value is not an integer");
    }
    bool Value::as_bool() const {
        return std::get<bool>(data);
    }
    std::string Value::as_string() const {
        return std::string(string_view_of(*this));
    }
    std::string Value::to_string() const {
        if(is_reference(type))return dereference().to_string();
        if(is_pointer(type))return native::pointer_text(std::get_if<Pointer>(&data)?std::get<Pointer>(data).address:nullptr);
        if(is_optional(type))return has_value()?optional_payload().to_string():"null";
        if(type=="string")return as_string();
        if(type=="char")return unicode::encode(static_cast<char32_t>(as_int()));
        return std::visit([](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>; if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, ValueOwned<Object>> || std::is_same_v<T, ValueOwned<Collection>> || std::is_same_v<T, Pointer> || std::is_same_v<T,ArenaString> || std::is_same_v<T,std::unique_ptr<ValueArena>>) throw std::runtime_error("only primitive values can be formatted"); else return native::text(v);
        }, data);
    }
    namespace {
        template<class T> T number(const Value& v) {
            return std::visit([](const auto& x) -> T {
                using X = std::decay_t<decltype(x)>; if constexpr (std::is_arithmetic_v<X> && !std::is_same_v<X, bool>) return native::cast<T>(x); else throw std::runtime_error("expected a numeric value");
            }, v.data);
        }
        template<class T> Value operation(const std::string& op, const std::string& type, const Value& a, const Value& b) {
            const T x = number<T>(a), y = number<T>(b);
            T result;
            if (op == "+") result = native::add<T>(x, y);
            else if (op == "-") result = native::sub<T>(x, y);
            else if (op == "*") result = native::mul<T>(x, y);
            else if (op == "/") result = native::div<T>(x, y);
            else if (op == "%") {
                if constexpr (std::is_integral_v<T>) result = native::mod<T>(x, y);
                else throw std::runtime_error("% requires integers");
            } else if(op=="&"||op=="|"||op=="^"||op=="<<"||op==">>") {
                if constexpr(std::is_integral_v<T>)result=native::bitwise<T>(op,x,y);
                else throw std::runtime_error("bitwise operations require integers");
            } else throw std::runtime_error("unknown arithmetic operator");
            if constexpr (std::is_floating_point_v<T>) return Value(result);
            else if constexpr (std::is_unsigned_v<T>) return Value::unsigned_integer(type, result);
            else return Value::signed_integer(type, result);
        }
        template<class T> bool compare(const std::string& op, const T& a, const T& b) {
            if (op == "==") return a == b;
            if (op == "!=") return a != b;
            if (op == "<") return a < b;
            if (op == ">") return a > b;
            if (op == "<=") return a <= b;
            if (op == ">=") return a >= b;
            throw std::runtime_error("unknown comparison operator");
        }
    }
    // namespace
    Value convert_value(const std::string& type, const Value& v) {
        if(is_optional(type)) {
            if(v.type=="null"||(is_optional(v.type)&&!v.has_value()))return Value::optional_value(type);
            auto payload=convert_value(element_type(type),is_optional(v.type)?v.optional_payload():v);
            return Value::optional_value(type,&payload);
        }
        // Always range-check integers: the tagged value also crosses the C++ embedding API.
        if (type == "char") {auto c=number<std::uint32_t>(v);if(!unicode::scalar(c))throw std::runtime_error("char conversion out of Unicode range");return Value::signed_integer(type,c);}
        if (type == "int") return Value::signed_integer(type, number<std::int32_t>(v));
        if (type == "short") return Value::signed_integer(type, number<std::int16_t>(v));
        if (type == "long") return Value::signed_integer(type, number<std::int64_t>(v));
        if (type == "byte") return Value::unsigned_integer(type, number<std::uint8_t>(v));
        if (type == "ushort") return Value::unsigned_integer(type, number<std::uint16_t>(v));
        if (type == "uint") return Value::unsigned_integer(type, number<std::uint32_t>(v));
        if (type == "ulong") return Value::unsigned_integer(type, number<std::uint64_t>(v));
        if (type == "float" || type == "double") {
            if(!v.bit_pattern) {
                if(const auto* f=std::get_if<float>(&v.data);f&&!std::isfinite(*f))throw std::runtime_error("non-finite floating-point value");
                if(const auto* d=std::get_if<double>(&v.data);d&&!std::isfinite(*d))throw std::runtime_error("non-finite floating-point value");
            }
            auto result=type=="float"?Value(number<float>(v)):Value(number<double>(v));
            result.bit_pattern=v.bit_pattern;
            return result;
        }
        if (is_pointer(type) && (is_pointer(v.type) || v.type=="null")) {
            Value result=v;
            result.type=type;
            return result;
        }
        if (type == v.type) return v;
        throw std::runtime_error("cannot convert '" + v.type + "' to '" + type + "'");
    }
    Value reinterpret_value(const std::string& target,const std::string& source,const Value& value) {
        std::uint64_t raw=0;
        if(source=="float")raw=std::bit_cast<std::uint32_t>(std::get<float>(value.data));
        else if(source=="double")raw=std::bit_cast<std::uint64_t>(std::get<double>(value.data));
        else if(is_unsigned(source))raw=std::get<std::uint64_t>(value.data);
        else raw=static_cast<std::uint64_t>(std::get<std::int64_t>(value.data));
        if(target=="float")return Value(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
        if(target=="double")return Value(std::bit_cast<double>(raw));
        if(is_unsigned(target))return Value::unsigned_integer(target,raw&(~std::uint64_t{0}>>(64-bits(target))));
        if(bits(target)==8)return Value::signed_integer(target,std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(raw)));
        if(bits(target)==16)return Value::signed_integer(target,std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(raw)));
        if(bits(target)==32)return Value::signed_integer(target,std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(raw)));
        return Value::signed_integer(target,std::bit_cast<std::int64_t>(raw));
    }
    Value numeric_limit_value(const std::string& type,bool maximum) {
        // lowest() is the most negative finite floating-point value; min()
        // would instead be the smallest positive normal value.
        const auto limit=[maximum]<class T>() {
            return Value(maximum?std::numeric_limits<T>::max():std::numeric_limits<T>::lowest());
        };
        if(type=="short")return limit.operator()<std::int16_t>();
        if(type=="int")return limit.operator()<std::int32_t>();
        if(type=="long")return limit.operator()<std::int64_t>();
        if(type=="byte")return limit.operator()<std::uint8_t>();
        if(type=="ushort")return limit.operator()<std::uint16_t>();
        if(type=="uint")return limit.operator()<std::uint32_t>();
        if(type=="ulong")return limit.operator()<std::uint64_t>();
        if(type=="float")return limit.operator()<float>();
        if(type=="double")return limit.operator()<double>();
        throw std::runtime_error("numeric limits require a numeric type");
    }
    Value default_primitive(const std::string& type) {
        if (type=="char")return Value::signed_integer(type,0);
        if (is_unsigned(type)) return Value::unsigned_integer(type, 0);
        if (is_integer(type)) return Value::signed_integer(type, 0);
        if (type == "float") return Value(0.0f);
        if (type == "double") return Value(0.0);
        if (type == "bool") return Value(false);
        if (type == "string") return Value(std::string {
        });
        return {
        };
    }
    Value literal_value(const Expr& e) {
        if (e.token.kind == Token::Kind::String) return Value(e.text);
        if(e.token.kind==Token::Kind::Character){std::size_t i=0;return Value::signed_integer("char",unicode::decode(e.text,i));}
        if (e.text == "null") return Value::pointer_value("null");
        if (e.text == "true" || e.text == "false") return Value(e.text == "true");
        std::string digits = e.text, suffix;
        while (!digits.empty() && std::isalpha(static_cast<unsigned char>(digits.back()))) {
            suffix.insert(suffix.begin(), static_cast<char>(std::tolower(static_cast<unsigned char>(digits.back()))));
            digits.pop_back();
        }
        const bool floating = digits.find_first_of(".eE") != std::string::npos || suffix == "f";
        if (floating) {
            if (!suffix.empty() && suffix != "f") throw Error(e.token.at, "invalid floating-point suffix");
            // Classic locale parsing makes .bolt numbers independent of the OS locale.
            std::istringstream input(digits);
            input.imbue(std::locale::classic());
            double d = 0;
            input >> d;
            if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(d)) throw Error(e.token.at, "invalid or out-of-range floating-point literal");
            if (suffix == "f") {
                try {
                    return Value(native::cast<float>(d));
                } catch (const std::exception& ex) {
                    throw Error(e.token.at, ex.what());
                }
            }
            return Value(d);
        }
        if (suffix != "" && suffix != "u" && suffix != "l" && suffix != "ul" && suffix != "lu") throw Error(e.token.at, "invalid integer suffix (use u, l, or ul)");
        const bool negative = !digits.empty() && digits.front() == '-';
        const bool explicitly_unsigned = suffix.find('u') != std::string::npos;
        const bool wide = suffix.find('l') != std::string::npos;
        if (negative) {
            if (explicitly_unsigned) throw Error(e.token.at, "an unsigned literal cannot be negative");
            std::int64_t n = 0;
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), n);
            if (parsed.ec != std::errc {
            }
            || parsed.ptr != digits.data() + digits.size()) throw Error(e.token.at, "integer literal out of range");
            return Value::signed_integer(!wide && n >= std::numeric_limits<std::int32_t>::min() ? "int" : "long", n);
        }
        std::uint64_t n = 0;
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), n);
        if (parsed.ec != std::errc {
        }
        || parsed.ptr != digits.data() + digits.size()) throw Error(e.token.at, "integer literal out of range");
        if (explicitly_unsigned) return Value::unsigned_integer(!wide && n <= std::numeric_limits<std::uint32_t>::max() ? "uint" : "ulong", n);
        if (n > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            if (wide) throw Error(e.token.at, "signed long literal out of range; use ul for ulong");
            return Value::unsigned_integer("ulong", n);
        }
        return Value::signed_integer(!wide && n <= std::numeric_limits<std::int32_t>::max() ? "int" : "long", static_cast<std::int64_t>(n));
    }
    Value numeric_binary(const std::string& op, const std::string& type, const Value& a, const Value& b) {
        if (type == "int") return operation<std::int32_t>(op, type, a, b);
        if (type == "long") return operation<std::int64_t>(op, type, a, b);
        if (type == "uint") return operation<std::uint32_t>(op, type, a, b);
        if (type == "ulong") return operation<std::uint64_t>(op, type, a, b);
        if (type == "float") return operation<float>(op, type, a, b);
        if (type == "double") return operation<double>(op, type, a, b);
        throw std::runtime_error("invalid arithmetic result type");
    }
    Value compare_values(const std::string& op, const Value& a, const Value& b) {
        if (a.type == "string" && b.type == "string") return Value(compare(op, string_view_of(a), string_view_of(b)));
        if (a.type == "bool" && b.type == "bool") return Value(compare(op, a.as_bool(), b.as_bool()));
        const auto type = common_numeric(a.type, b.type);
        if (type == "int") return Value(compare(op, number<std::int32_t>(a), number<std::int32_t>(b)));
        if (type == "long") return Value(compare(op, number<std::int64_t>(a), number<std::int64_t>(b)));
        if (type == "uint") return Value(compare(op, number<std::uint32_t>(a), number<std::uint32_t>(b)));
        if (type == "ulong") return Value(compare(op, number<std::uint64_t>(a), number<std::uint64_t>(b)));
        if (type == "float") return Value(compare(op, number<float>(a), number<float>(b)));
        if (type == "double") return Value(compare(op, number<double>(a), number<double>(b)));
        if(a.type==b.type && std::holds_alternative<std::int64_t>(a.data)) return Value(compare(op,std::get<std::int64_t>(a.data),std::get<std::int64_t>(b.data)));
        if(a.type==b.type && std::holds_alternative<std::uint64_t>(a.data)) return Value(compare(op,std::get<std::uint64_t>(a.data),std::get<std::uint64_t>(b.data)));
        if(op=="=="||op=="!=") {
            bool same=equal_values(a,b);
            return Value(op=="=="?same:!same);
        }
        throw std::runtime_error("incompatible comparison operands");
    }
    bool equal_values(const Value& a,const Value& b) {
        if(is_optional(a.type)&&b.type=="null")return !a.has_value();
        if(is_optional(b.type)&&a.type=="null")return !b.has_value();
        if (auto* p=std::get_if<Pointer>(&a.data)) {
            auto* q=std::get_if<Pointer>(&b.data);
            return q&&p->address==q->address;
        }
        if(a.type!=b.type) {
            if(is_numeric(a.type)&&is_numeric(b.type))return compare_values("==",a,b).as_bool();
            return false;
        }
        if(auto* obj=std::get_if<ValueOwned<Object>>(&a.data)) {
            const auto& other=b.object().fields;
            if((*obj)->fields.size()!=other.size())return false;
            for(const auto& [k,v]:(*obj)->fields) {
                auto i=other.find(k);
                if(i==other.end()||!equal_values(v,i->second))return false;
            }
            return true;
        }
        if(std::holds_alternative<ValueOwned<Collection>>(a.data)) {
            const auto& x=a.collection();
            const auto& y=b.collection();
            if(array_rank(a.type)>1&&x.dimensions!=y.dimensions)return false;
            if(x.values.size()!=y.values.size()||x.entries.size()!=y.entries.size())return false;
            if(collection_kind(a.type)=="Dictionary") {
                for(const auto& [k,v]:x.entries) {
                    bool found=false;
                    for(const auto& [l,w]:y.entries)if(equal_values(k,l)&&equal_values(v,w)) {
                        found=true;
                        break;
                    }
                    if(!found)return false;
                }
                return true;
            }
            if(collection_kind(a.type)=="HashSet") {
                for(const auto& v:x.values) {
                    bool found=false;
                    for(const auto& w:y.values)if(equal_values(v,w)) {
                        found=true;
                        break;
                    }
                    if(!found)return false;
                }
                return true;
            }
            for(std::size_t i=0;i<x.values.size();++i)if(!equal_values(x.values[i],y.values[i]))return false;
            return true;
        }
        if(a.type=="void")return true;
        if(auto* p=std::get_if<bool>(&a.data))return *p==std::get<bool>(b.data);
        if(a.type=="string")return string_view_of(a)==string_view_of(b);
        if(auto* p=std::get_if<std::int64_t>(&a.data))return *p==std::get<std::int64_t>(b.data);
        if(auto* p=std::get_if<std::uint64_t>(&a.data))return *p==std::get<std::uint64_t>(b.data);
        if(auto* p=std::get_if<float>(&a.data))return *p==std::get<float>(b.data);
        if(auto* p=std::get_if<double>(&a.data))return *p==std::get<double>(b.data);
        return false;
    }
    void assign_in_place(Value& destination,const Value& source) {
        if(is_optional(destination.type)&&destination.type==source.type) {
            if(source.has_value()) {
                if(destination.has_value())assign_in_place(destination.optional_payload(),source.optional_payload());
                else destination.object().fields.emplace("@value",source.optional_payload().copy_to(destination.allocation_arena));
            } else destination.object().fields.erase("@value");
            return;
        }
        if(destination.type==source.type && std::holds_alternative<ValueOwned<Object>>(destination.data)) {
            for(auto& [name,field]:destination.object().fields)assign_in_place(field,source.object().fields.at(name));
        } else destination=source;
    }
}
// namespace bolt
