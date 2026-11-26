#pragma once
#include "Native.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <typeinfo>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace bolt::native {
    struct ObjectRoot;
    struct ObjectDeleter { void operator()(ObjectRoot* value) const; };
    using ObjectPointer=std::unique_ptr<ObjectRoot,ObjectDeleter>;
    struct ObjectRoot {
        virtual ~ObjectRoot()=default;
        virtual ObjectPointer BoltClone(memory::Arena* destination=nullptr) const=0;
        virtual bool BoltObjectEquals(const ObjectRoot&) const=0;
        virtual std::size_t BoltObjectHash() const=0;
        virtual std::string BoltFormat() const {throw std::runtime_error("value does not implement IStringable");}
    };
    inline void ObjectDeleter::operator()(ObjectRoot* value) const {
        if(!value)return;
        if(memory::owner_of(value))value->~ObjectRoot();
        else delete value;
    }
    template<class Interface,class T> struct PrimitiveInterfaceAdapter;
    template<class Interface,class T> ObjectPointer interface_clone(const T& value,memory::Arena* destination=nullptr) {
        ObjectPointer copy;
        if constexpr(std::is_base_of_v<ObjectRoot,T>)copy=value.BoltClone(destination);
        else copy.reset(owned_new<PrimitiveInterfaceAdapter<Interface,T>>(destination,value));
        if(!dynamic_cast<Interface*>(copy.get()))throw std::runtime_error("value does not implement the requested interface");
        return copy;
    }

    template<class T,class U> T reinterpret_bits(const U& value) noexcept {
        static_assert(sizeof(T)==sizeof(U));
        static_assert(std::is_trivially_copyable_v<T>&&std::is_trivially_copyable_v<U>);
        return std::bit_cast<T>(value);
    }
    inline std::size_t hash_combine(std::size_t a,std::size_t b) noexcept {
        return a ^ (b+std::size_t {
            0x9e3779b9u
        }
        +(a<<6u)+(a>>2u));
    }
    // Defer native-value capabilities until an operation is actually used.
    // Merely storing an opaque foreign resource must not instantiate an
    // unavailable C++ equality/hash operator in an otherwise unused method.
    template<class T> bool value_equal(const T& left,const T& right) {
        if constexpr(requires { { left==right } -> std::convertible_to<bool>; })return left==right;
        else if constexpr(requires {left.Identity();right.Identity();})return left.Identity()==right.Identity();
        else throw std::runtime_error("Equality is not available for this native value type.");
    }
    template<class T> struct Hasher {
        std::size_t operator()(const T& value) const {
            if constexpr(requires {
                value.BoltHash();
            }) return value.BoltHash();
            else if constexpr(requires {value.Identity();})return std::hash<const void*>{}(value.Identity());
            else if constexpr(requires {std::hash<T>{}(value);})return std::hash<T>{}(value);
            else throw std::runtime_error("Hashing is not available for this native value type.");
        }
    };
    template<class T> T& deref(T* pointer) {
        if(!pointer)throw std::runtime_error("null pointer dereference");
        return *pointer;
    }
    template<class I> std::size_t checked_size(I n) {
        return cast<std::size_t>(n);
    }
    template<class I> std::size_t checked_index(I n,std::size_t size,bool end=false) {
        if constexpr(std::is_signed_v<I>)if(n<0)throw std::runtime_error("index out of range");
        const auto index=checked_size(n);
        if(index>size||(!end&&index==size))throw std::runtime_error("index out of range");
        return index;
    }
    template<class I> std::size_t checked_dimension(I value) {
        if constexpr(std::is_signed_v<I>)if(value<0)throw std::runtime_error("array dimensions must be non-negative");
        return checked_size(value);
    }
    template<class Dimensions> std::size_t array_count(const Dimensions& dimensions) {
        if(std::find(dimensions.begin(),dimensions.end(),std::size_t{0})!=dimensions.end())return 0;
        std::size_t count=1;
        for(auto size:dimensions) {
            if(size>std::numeric_limits<std::size_t>::max()/count)throw std::runtime_error("array size is too large");
            count*=size;
        }
        return count;
    }
    template<class Dimensions,class Indices> std::size_t array_offset(const Dimensions& dimensions,const Indices& indices) {
        if(indices.size()!=dimensions.size())throw std::runtime_error("incorrect number of array indices");
        std::size_t offset=0;
        for(std::size_t axis=0;axis<dimensions.size();++axis) {
            const auto index=checked_index(indices[axis],dimensions[axis]);
            if(offset>(std::numeric_limits<std::size_t>::max()-index)/dimensions[axis])throw std::runtime_error("array size is too large");
            offset=offset*dimensions[axis]+index;
        }
        return offset;
    }
    template<class Range,class Formatter> std::string collection_text(Range values,char opening,char closing,bool unordered,Formatter format) {
        // Copy the collection before invoking element formatters, which may mutate
        // other values through user code. Each element retains text()'s semantics.
        std::vector<std::string> parts;
        parts.reserve(values.size());
        for(const auto& value:values)parts.push_back(format(value));
        if(unordered)std::sort(parts.begin(),parts.end());
        std::string result(1,opening);
        for(std::size_t i=0;i<parts.size();++i) {
            if(i)result+=", ";
            result+=parts[i];
        }
        result+=closing;
        return result;
    }
    template<class T> class Array {
        using Allocator=DestinationAllocator<T>;
        std::vector<T,Allocator> values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public: Array():values(allocator()) {}
        Array(const Array& other):values(other.values,allocator()) {}
        Array(Array&& other):values(std::move(other.values),allocator()) {
            detail::RangeObserver::changed(&other);
        }
        Array& operator=(const Array& other) {
            if (this != &other) {
                detail::RangeObserver::changed(this);
                values = other.values;
            }
            return *this;
        }
        Array& operator=(Array&& other) {
            if (this != &other) {
                detail::RangeObserver::changed(this);
                detail::RangeObserver::changed(&other);
                values = std::move(other.values);
            }
            return *this;
        }
        Array(std::initializer_list<T> input):values(input,allocator()) {
        }
        template<class It> static Array FromRange(It first,It last){Array a;a.values.assign(first,last);return a;}
        template<class I> static Array Create(I count) {
            Array a;
            a.values.resize(checked_size(count));
            return a;
        }
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count() const {
            return cast<std::int32_t>(values.size());
        }
        std::int32_t Rank()const {return 1;}
        template<class I> std::int32_t GetLength(I dimension)const {
            (void)checked_index(dimension,std::size_t{1});
            return Count();
        }
        template<class I> T& At(I i) {
            return values[checked_index(i,values.size())];
        }
        template<class I> const T& At(I i) const {
            return values[checked_index(i,values.size())];
        }
        const T* TryAt(std::int32_t index) const noexcept {
            return index>=0 && static_cast<std::size_t>(index)<values.size() ? values.data()+index : nullptr;
        }
        T* TryAt(std::int32_t index) noexcept {
            return const_cast<T*>(static_cast<const Array&>(*this).TryAt(index));
        }
        bool operator==(const Array&) const=default;
        std::size_t BoltHash() const {
            std::size_t h=values.size();
            for(const auto& v:values)h=hash_combine(h,Hasher<T> {
            }
            (v));
            return h;
        }
    };
    // vector<bool> is a proxy specialization, which cannot implement Bolt's bool&.
    // A deque keeps actual bool elements while retaining value semantics.
    template<> class Array<bool> {
        using Allocator=DestinationAllocator<bool>;
        std::deque<bool,Allocator> values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public: Array():values(allocator()) {}
        Array(const Array& other):values(other.values,allocator()) {}
        Array(Array&& other):values(std::move(other.values),allocator()) {
            detail::RangeObserver::changed(&other);
        }
        Array& operator=(const Array& other) {
            if (this != &other) {
                detail::RangeObserver::changed(this);
                values = other.values;
            }
            return *this;
        }
        Array& operator=(Array&& other) {
            if (this != &other) {
                detail::RangeObserver::changed(this);
                detail::RangeObserver::changed(&other);
                values = std::move(other.values);
            }
            return *this;
        }
        Array(std::initializer_list<bool> input):values(input,allocator()) {
        }
        template<class It> static Array FromRange(It first,It last){Array a;a.values.assign(first,last);return a;}
        template<class I> static Array Create(I count) {
            Array a;
            a.values.resize(checked_size(count));
            return a;
        }
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count()const {
            return cast<std::int32_t>(values.size());
        }
        std::int32_t Rank()const {return 1;}
        template<class I> std::int32_t GetLength(I dimension)const {
            (void)checked_index(dimension,std::size_t{1});
            return Count();
        }
        template<class I> bool& At(I i) {
            return values[checked_index(i,values.size())];
        }
        template<class I> const bool& At(I i)const {
            return values[checked_index(i,values.size())];
        }
        bool operator==(const Array&)const=default;
        std::size_t BoltHash()const {
            std::size_t h=values.size();
            for(bool b:values)h=hash_combine(h,Hasher<bool> {
            }
            (b));
            return h;
        }
    };
    template<class T,std::size_t RankValue> class RectArray {
        static_assert(RankValue>1);
        using Allocator=DestinationAllocator<T>;
        using Storage=std::conditional_t<std::is_same_v<T,bool>,std::deque<T,Allocator>,std::vector<T,Allocator>>;
        std::array<std::size_t,RankValue> dimensions{};
        Storage values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public:
        RectArray():values(allocator()) {}
        RectArray(const RectArray& other):dimensions(other.dimensions),values(other.values,allocator()) {}
        RectArray(RectArray&& other):dimensions(other.dimensions),values(std::move(other.values),allocator()) {}
        RectArray& operator=(const RectArray&)=default;
        RectArray& operator=(RectArray&&)=default;
        RectArray(std::array<std::size_t,RankValue> shape,std::initializer_list<T> input):dimensions(shape),values(input,allocator()) {
            if(array_count(dimensions)!=values.size())throw std::runtime_error("array initializer shape does not match its elements");
        }
        static RectArray Create(std::array<std::size_t,RankValue> shape) {
            RectArray result;
            result.dimensions=shape;
            const auto count=array_count(shape);
            if(count>result.values.max_size())throw std::runtime_error("array size is too large");
            result.values.resize(count);
            return result;
        }
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count()const {return cast<std::int32_t>(values.size());}
        std::int32_t Rank()const {return cast<std::int32_t>(RankValue);}
        template<class I> std::int32_t GetLength(I dimension)const {
            return cast<std::int32_t>(dimensions[checked_index(dimension,RankValue)]);
        }
        template<class... I> T& At(I... indices) {
            static_assert(sizeof...(I)==RankValue);
            const std::array<std::size_t,RankValue> keys{checked_size(indices)...};
            return values[array_offset(dimensions,keys)];
        }
        template<class... I> const T& At(I... indices)const {
            static_assert(sizeof...(I)==RankValue);
            const std::array<std::size_t,RankValue> keys{checked_size(indices)...};
            return values[array_offset(dimensions,keys)];
        }
        bool operator==(const RectArray&)const=default;
        std::size_t BoltHash()const {
            std::size_t hash=RankValue;
            for(auto size:dimensions)hash=hash_combine(hash,size);
            for(const auto& value:values)hash=hash_combine(hash,Hasher<T>{}(value));
            return hash;
        }
    };
    template<class T> class List {
        using Allocator=DestinationAllocator<T>;
        using Storage=std::conditional_t<std::is_same_v<T,bool>,std::deque<T,Allocator>,std::vector<T,Allocator>>;
        Storage values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public:
        List():values(allocator()) {}
        List(const List& other):values(other.values,allocator()) {}
        List(List&& other):values(std::move(other.values),allocator()) {}
        List& operator=(const List&)=default;
        List& operator=(List&&)=default;
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count()const {
            return cast<std::int32_t>(values.size());
        }
        void Add(const T& value) {
            values.push_back(value);
        }
        void Clear() {
            values.clear();
        }
        bool Contains(const T& value)const {
            return std::find(values.begin(),values.end(),value)!=values.end();
        }
        std::int32_t IndexOf(const T& value)const {
            auto i=std::find(values.begin(),values.end(),value);
            return i==values.end()?-1:cast<std::int32_t>(std::distance(values.begin(),i));
        }
        bool Remove(const T& value) {
            auto i=std::find(values.begin(),values.end(),value);
            if(i==values.end())return false;
            values.erase(i);
            return true;
        }
        template<class I> void RemoveAt(I i) {
            values.erase(values.begin()+static_cast<std::ptrdiff_t>(checked_index(i,values.size())));
        }
        template<class I> void Insert(I i,const T& value) {
            values.insert(values.begin()+static_cast<std::ptrdiff_t>(checked_index(i,values.size(),true)),value);
        }
        template<class I> T& At(I i) {
            return values[checked_index(i,values.size())];
        }
        template<class I> const T& At(I i)const {
            return values[checked_index(i,values.size())];
        }
        bool operator==(const List&)const=default;
        std::string BoltFormat()const {
            return collection_text(values,'[',']',false,[](const auto& value){return text(value);});
        }
        std::size_t BoltHash()const {
            std::size_t h=values.size();
            for(const auto& v:values)h=hash_combine(h,Hasher<T> {
            }
            (v));
            return h;
        }
    };
    template<class T> class HashSet {
        using Allocator=DestinationAllocator<T>;
        std::unordered_set<T,Hasher<T>,std::equal_to<T>,Allocator> values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public:
        HashSet():values(allocator()) {}
        HashSet(const HashSet& other):values(other.values,allocator()) {}
        HashSet(HashSet&& other):values(std::move(other.values),allocator()) {}
        HashSet& operator=(const HashSet&)=default;
        HashSet& operator=(HashSet&&)=default;
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count()const {
            return cast<std::int32_t>(values.size());
        }
        bool Add(const T& value) {
            return values.insert(value).second;
        }
        bool Remove(const T& value) {
            return values.erase(value)!=0;
        }
        bool Contains(const T& value)const {
            return values.contains(value);
        }
        void Clear() {
            values.clear();
        }
        bool operator==(const HashSet&)const=default;
        std::string BoltFormat()const {
            return collection_text(values,'{','}',true,[](const auto& value){return text(value);});
        }
        std::size_t BoltHash()const {
            std::size_t h=values.size();
            for(const auto& v:values)h^=Hasher<T> {
            }
            (v);
            return h;
        }
    };
    template<class K,class V> class Dictionary {
        using Allocator=DestinationAllocator<std::pair<const K,V>>;
        std::unordered_map<K,V,Hasher<K>,std::equal_to<K>,Allocator> values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public:
        Dictionary():values(allocator()) {}
        Dictionary(const Dictionary& other):values(other.values,allocator()) {}
        Dictionary(Dictionary&& other):values(std::move(other.values),allocator()) {}
        Dictionary& operator=(const Dictionary&)=default;
        Dictionary& operator=(Dictionary&&)=default;
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count()const {
            return cast<std::int32_t>(values.size());
        }
        void Add(const K& key,const V& value) {
            if(!values.emplace(key,value).second)throw std::runtime_error("duplicate dictionary key");
        }
        void Set(const K& key,const V& value) {
            values.insert_or_assign(key,value);
        }
        bool ContainsKey(const K& key)const {
            return values.contains(key);
        }
        bool Remove(const K& key) {
            return values.erase(key)!=0;
        }
        void Clear() {
            values.clear();
        }
        V& At(const K& key) {
            auto i=values.find(key);
            if(i==values.end())throw std::runtime_error("dictionary key not found");
            return i->second;
        }
        const V& At(const K& key)const {
            auto i=values.find(key);
            if(i==values.end())throw std::runtime_error("dictionary key not found");
            return i->second;
        }
        V& GetOrAdd(const K& key) {
            return values[key];
        }
        bool TryGetValue(const K& key,V& output)const {
            auto i=values.find(key);
            if(i==values.end())return false;
            output=i->second;
            return true;
        }
        bool operator==(const Dictionary&)const=default;
        std::string BoltFormat()const {
            return collection_text(values,'{','}',true,[](const auto& entry){
                const auto key=text(entry.first);
                return key+": "+text(entry.second);
            });
        }
        std::size_t BoltHash()const {
            std::size_t h=values.size();
            for(const auto& [k,v]:values)h^=hash_combine(Hasher<K> {
            }
            (k),Hasher<V> {
            }
            (v));
            return h;
        }
    };
    template<class T> class Queue {
        using Allocator=DestinationAllocator<T>;
        std::deque<T,Allocator> values;
        Allocator allocator()const {return Allocator(memory::owner_of(this));}
        public:
        Queue():values(allocator()) {}
        Queue(const Queue& other):values(other.values,allocator()) {}
        Queue(Queue&& other):values(std::move(other.values),allocator()) {}
        Queue& operator=(const Queue&)=default;
        Queue& operator=(Queue&&)=default;
        auto begin(){return values.begin();}
        auto end(){return values.end();}
        auto begin()const{return values.begin();}
        auto end()const{return values.end();}
        std::int32_t Count()const {
            return cast<std::int32_t>(values.size());
        }
        void Enqueue(const T& value) {
            values.push_back(value);
        }
        T Dequeue() {
            if(values.empty())throw std::runtime_error("queue is empty");
            T v=values.front();
            values.pop_front();
            return v;
        }
        T Peek()const {
            if(values.empty())throw std::runtime_error("queue is empty");
            return values.front();
        }
        bool TryDequeue(T& output) {
            if(values.empty())return false;
            output=values.front();
            values.pop_front();
            return true;
        }
        bool Contains(const T& value)const {
            return std::find(values.begin(),values.end(),value)!=values.end();
        }
        void Clear() {
            values.clear();
        }
        bool operator==(const Queue&)const=default;
        std::string BoltFormat()const {
            return collection_text(values,'[',']',false,[](const auto& value){return text(value);});
        }
        std::size_t BoltHash()const {
            std::size_t h=values.size();
            for(const auto& v:values)h=hash_combine(h,Hasher<T> {
            }
            (v));
            return h;
        }
    };
    inline constexpr double Pi=3.141592653589793238462643383279502884;
    inline constexpr double E=2.718281828459045235360287471352662498;
    inline constexpr double Tau=2.0*Pi;

}
// namespace bolt::native

#include "Buffer.hpp"
