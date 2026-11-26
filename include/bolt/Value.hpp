#pragma once
#include "Ast.hpp"
#include <cstdint>
#include <memory>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <variant>
namespace bolt {
    struct Object;
    struct Collection;
    class Value;
    class ValueArena {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        ValueArena(std::size_t initial,std::size_t limit);
        ~ValueArena();
        ValueArena(const ValueArena&)=delete;
        ValueArena& operator=(const ValueArena&)=delete;
        void* allocate(std::size_t bytes,std::size_t alignment);
        void add_root(Value* value);
        void reset();
        void dispose();
        bool owns(const void*)const;
        std::uint64_t used()const;
        std::uint64_t capacity()const;
        std::uint64_t peak()const;
        std::uint64_t limit()const;
    };
    void* allocate_value_storage(ValueArena*,std::size_t,std::size_t);
    void deallocate_value_storage(ValueArena*,void*,std::size_t)noexcept;
    template<class T> struct ValueAllocator {
        using value_type=T;
        ValueArena* arena=nullptr;
        ValueAllocator()=default;
        explicit ValueAllocator(ValueArena* a):arena(a){}
        template<class U> ValueAllocator(const ValueAllocator<U>& a):arena(a.arena){}
        T* allocate(std::size_t n){
            if(n>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_array_new_length();
            return static_cast<T*>(allocate_value_storage(arena,n*sizeof(T),alignof(T)));
        }
        void deallocate(T* p,std::size_t)noexcept{deallocate_value_storage(arena,p,alignof(T));}
        template<class U> bool operator==(const ValueAllocator<U>& a)const{return arena==a.arena;}
    };
    template<class T> struct ValueDeleter {
        ValueArena* arena=nullptr;
        void operator()(T* value)const noexcept {
            if(value){value->~T();deallocate_value_storage(arena,value,alignof(T));}
        }
    };
    template<class T> using ValueOwned=std::unique_ptr<T,ValueDeleter<T>>;
    struct ArenaString {
        const char* data=nullptr;
        std::size_t size=0;
    };
    struct Pointer {
        Value* address=nullptr;
        std::weak_ptr<char> lifetime;
    };
    class Value {
        public: using Data=std::variant<std::monostate,std::int64_t,std::uint64_t,float,double,bool,std::string,ValueOwned<Object>,ValueOwned<Collection>,Pointer,ArenaString,std::unique_ptr<ValueArena>>;
        std::string type="void";
        Data data;
        ValueArena* allocation_arena=nullptr;
        // Only deliberate language-level bit reinterpretation may introduce
        // non-finite values; ordinary external embedding inputs remain checked.
        bool bit_pattern=false;
        std::shared_ptr<char> lifetime=std::make_shared<char>();
        // Iteration observes assignments to the range and structural collection edits.
        std::uint64_t range_version=0;
        Value()=default;
        explicit Value(std::int16_t x):type("short"),data(static_cast<std::int64_t>(x)) {
        }
        explicit Value(std::uint8_t x):type("byte"),data(static_cast<std::uint64_t>(x)) {
        }
        explicit Value(std::uint16_t x):type("ushort"),data(static_cast<std::uint64_t>(x)) {
        }
        explicit Value(std::uint32_t x):type("uint"),data(static_cast<std::uint64_t>(x)) {
        }
        explicit Value(std::int32_t x):type("int"),data(static_cast<std::int64_t>(x)) {
        }
        explicit Value(std::int64_t x):type("long"),data(x) {
        }
        explicit Value(std::uint64_t x):type("ulong"),data(x) {
        }
        explicit Value(float x):type("float"),data(x) {
        }
        explicit Value(double x):type("double"),data(x) {
        }
        explicit Value(bool x):type("bool"),data(x) {
        }
        explicit Value(std::string x):type("string"),data(std::move(x)) {
        }
        explicit Value(const char* x):Value(std::string(x)) {
        }
        Value(const Value& other);
        Value& operator=(const Value& other);
        Value(Value&& other);
        Value& operator=(Value&& other);
        ~Value();
        static Value signed_integer(const std::string& type,std::int64_t x);
        static Value unsigned_integer(const std::string& type,std::uint64_t x);
        static Value object_value(const std::string& type,ValueArena* arena=nullptr);
        static Value collection_value(const std::string& type,ValueArena* arena=nullptr);
        static Value arena_value(std::size_t initial,std::size_t limit);
        Value copy_to(ValueArena* arena)const;
        ValueArena& arena()const;
        static Value pointer_value(const std::string& type,Value* target=nullptr);
        static Value optional_value(const std::string& type,const Value* value=nullptr,ValueArena* arena=nullptr);
        bool has_value()const;
        Value& optional_payload();
        const Value& optional_payload()const;
        Object& object();
        const Object& object()const;
        Collection& collection();
        const Collection& collection()const;
        Value& dereference()const;
        std::int64_t as_int()const;
        bool as_bool()const;
        std::string as_string()const;
        std::string to_string()const;
    };
    struct Object {
        bool interface_box=false;
        std::shared_ptr<Value> capture_cell;
        std::unordered_map<std::string,Value,std::hash<std::string>,std::equal_to<std::string>,ValueAllocator<std::pair<const std::string,Value>>> fields;
        std::string dispatch_type;
        explicit Object(ValueArena* arena=nullptr):fields(ValueAllocator<std::pair<const std::string,Value>>(arena)){}
    };
    struct Collection {
        std::vector<std::size_t,ValueAllocator<std::size_t>> dimensions;
        std::vector<Value,ValueAllocator<Value>> values;
        std::vector<std::pair<Value,Value>,ValueAllocator<std::pair<Value,Value>>> entries;
        explicit Collection(ValueArena* arena=nullptr):dimensions(ValueAllocator<std::size_t>(arena)),values(ValueAllocator<Value>(arena)),entries(ValueAllocator<std::pair<Value,Value>>(arena)){}
    };
    Value& unbox(Value& value);
    const Value& unbox(const Value& value);
    Value literal_value(const Expr& expression);
    Value numeric_limit_value(const std::string& type,bool maximum);
    Value convert_value(const std::string& type,const Value& value);
    Value reinterpret_value(const std::string& target,const std::string& source,const Value& value);
    Value numeric_binary(const std::string& op,const std::string& type,const Value& a,const Value& b);
    Value compare_values(const std::string& op,const Value& a,const Value& b);
    Value default_primitive(const std::string& type);
    bool equal_values(const Value& a,const Value& b);
    void assign_in_place(Value& destination,const Value& source);
}
// namespace bolt
