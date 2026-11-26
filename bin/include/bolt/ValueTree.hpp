#pragma once
// Typed heterogeneous storage and reflected-object mapping only.
// JSON grammar, escaping, traversal and BJS1 framing live in .bolt source.
#include "library/Json.hpp"
#include <memory>
#include <variant>
#include <map>
#include <vector>
#include <charconv>

namespace bolt::platform {
class ValueTree {
    using Json = library::json_detail::Json;
    using Children = std::vector<std::int32_t>;
    using Members = std::map<std::string, std::int32_t>;
    using Node = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t,
                              double, std::string, Children, Members>;
    struct Storage { std::vector<Node> nodes; std::int32_t root=-1; };
    std::shared_ptr<Storage> storage_ = std::make_shared<Storage>();
    const Node& at(std::int32_t id) const {
        if(id<0 || static_cast<std::size_t>(id)>=storage_->nodes.size())
            throw library::JsonException("Invalid value-tree node.");
        return storage_->nodes[static_cast<std::size_t>(id)];
    }
    Node& at(std::int32_t id) { return const_cast<Node&>(std::as_const(*this).at(id)); }
    template<class V> std::int32_t append(V value) {
        if(storage_->nodes.size()>=static_cast<std::size_t>(INT32_MAX))
            throw library::JsonException("Value tree exceeds the Bolt length limit.");
        const auto id=static_cast<std::int32_t>(storage_->nodes.size());
        storage_->nodes.emplace_back(std::move(value)); return id;
    }
    // Typed metadata visitation remains native, but writes directly to this arena.
    // No second JSON DOM and no recursive subtree copies are constructed.
    template<class T> std::int32_t encode(const T& value,int depth=0) {
        using V=std::remove_cvref_t<T>;
        using C=library::json_detail::Container<V>;
        using S=library::json_detail::SourceContainer<V>;
        library::json_detail::depth_check(depth);
        if constexpr(native::optional_traits<V>::value)return value.HasValue()?encode(value.Value(),depth+1):Null();
        else if constexpr(S::Kind==3)return encode(S::Export(value),depth+1);
        else if constexpr(std::is_same_v<V,bool>)return Bool(value);
        else if constexpr(std::is_same_v<V,char32_t>)return Text(unicode::encode(value));
        else if constexpr(std::is_integral_v<V> && std::is_signed_v<V>)return Signed(static_cast<std::int64_t>(value));
        else if constexpr(std::is_integral_v<V>)return Unsigned(static_cast<std::uint64_t>(value));
        else if constexpr(std::is_floating_point_v<V>)return Real(static_cast<double>(value));
        else if constexpr(std::is_enum_v<V>)return encode(static_cast<std::underlying_type_t<V>>(value),depth);
        else if constexpr(std::is_same_v<V,native::String>)return Text(value);
        else if constexpr(std::is_same_v<V,library::TimeSpan>)return Signed(value.Ticks());
        else if constexpr(std::is_same_v<V,library::DateTime>)return Text(value.ToUniversalTime().ToString());
        else if constexpr(S::Kind==1) {
            auto id=Array();const auto count=S::Count(value);
            std::get<Children>(at(id)).reserve(static_cast<std::size_t>(count));
            for(std::int32_t i=0;i<count;++i){const auto child=encode(S::At(value,i),depth+1);Add(id,child);}
            return id;
        }else if constexpr(S::Kind==2) {
            const auto count=S::Count(value);
            if constexpr(std::is_same_v<typename S::Key,native::String>) {
                auto id=Object();
                for(std::int32_t i=0;i<count;++i){auto pair=S::At(value,i);const auto child=encode(S::GetValue(pair),depth+1);Put(id,S::GetKey(pair),child);}
                return id;
            }else{
                auto id=Array();std::get<Children>(at(id)).reserve(static_cast<std::size_t>(count));
                for(std::int32_t i=0;i<count;++i){auto pair=S::At(value,i);auto row=Object();
                    const auto key=encode(S::GetKey(pair),depth+1);Put(row,"Key",key);
                    const auto child=encode(S::GetValue(pair),depth+1);Put(row,"Value",child);Add(id,row);}
                return id;
            }
        }else if constexpr(C::Kind>=1 && C::Kind<=4){
            auto id=Array();std::get<Children>(at(id)).reserve(static_cast<std::size_t>(value.Count()));
            for(const auto& v:value){const auto child=encode(v,depth+1);Add(id,child);}return id;
        }else if constexpr(C::Kind==5){
            if constexpr(std::is_same_v<typename C::Key,native::String>){
                auto id=Object();for(const auto& pair:value){const auto child=encode(pair.second,depth+1);Put(id,pair.first,child);}return id;
            }else{
                auto id=Array();for(const auto& pair:value){auto row=Object();
                    const auto key=encode(pair.first,depth+1);Put(row,"Key",key);
                    const auto child=encode(pair.second,depth+1);Put(row,"Value",child);Add(id,row);}return id;
            }
        }else if constexpr(library::Metadata<V>::Reflected){
            auto id=Object();std::apply([&](const auto&... field){([&]{if(!field.JsonIgnore){
                const auto child=encode(field.Get(value),depth+1);Put(id,field.JsonName,child);
            }}(),...);},library::Metadata<V>::Fields());return id;
        }else throw library::JsonException("Serialization requires a supported value/collection type or [Reflect] type. Raw pointers are not serialized.");
    }
    class View;
    struct ArrayView {
        const ValueTree* owner; const Children* children;
        struct Iterator {
            const ValueTree* owner; Children::const_iterator it;
            View operator*() const;
            Iterator& operator++(){++it;return *this;}
            bool operator!=(const Iterator& other)const{return it!=other.it;}
        };
        std::size_t size()const{return children->size();}
        Iterator begin()const{return {owner,children->begin()};}
        Iterator end()const{return {owner,children->end()};}
    };
    struct ObjectView {
        const ValueTree* owner; const Members* members;
        struct Iterator {
            const ValueTree* owner; Members::const_iterator it;
            std::pair<const std::string&,View> operator*() const;
            Iterator& operator++(){++it;return *this;}
            bool operator!=(const Iterator& other)const{return it!=other.it;}
        };
        Iterator begin()const{return {owner,members->begin()};}
        Iterator end()const{return {owner,members->end()};}
    };
    class View {
        const ValueTree* owner_; std::int32_t id_;
        const Node& node()const{return owner_->at(id_);}
    public:
        View(const ValueTree* owner,std::int32_t id):owner_(owner),id_(id){}
        bool is_null()const{return id_<0||node().index()==0;}
        bool is_bool()const{return id_>=0&&node().index()==1;}
        bool is_integer()const{return id_>=0&&node().index()==2;}
        bool is_unsigned()const{return id_>=0&&node().index()==3;}
        bool is_real()const{return id_>=0&&node().index()==4;}
        bool is_string()const{return id_>=0&&node().index()==5;}
        bool is_array()const{return id_>=0&&node().index()==6;}
        bool is_object()const{return id_>=0&&node().index()==7;}
        bool boolean()const{return std::get<bool>(node());}
        std::int64_t integer()const{return std::get<std::int64_t>(node());}
        std::uint64_t unsigned_integer()const{return std::get<std::uint64_t>(node());}
        double real()const{return std::get<double>(node());}
        long double number()const{
            if(is_integer())return static_cast<long double>(integer());
            if(is_unsigned())return static_cast<long double>(unsigned_integer());
            if(is_real())return real();throw library::JsonException("Expected a JSON number.");
        }
        const std::string& string()const{return std::get<std::string>(node());}
        bool contains(const std::string& key)const{return is_object()&&std::get<Members>(node()).contains(key);}
        View operator[](const std::string& key)const{
            if(!is_object())return {owner_,-1};
            const auto& members=std::get<Members>(node());auto it=members.find(key);
            return {owner_,it==members.end()?-1:it->second};
        }
        ArrayView array()const{return {owner_,&std::get<Children>(node())};}
        ObjectView object()const{return {owner_,&std::get<Members>(node())};}
    };
    template<class F> static decltype(auto) checked(F&& f) {
        try{return std::forward<F>(f)();}
        catch(const library::JsonException&){throw;}
        catch(const std::exception& e){throw library::JsonException(e.what());}
    }
public:
    ValueTree() = default;
    const void* Identity() const noexcept { return storage_.get(); }
    std::int32_t Null(){return append(nullptr);}
    std::int32_t Bool(bool v){return append(v);}
    std::int32_t Signed(std::int64_t v){return append(v);}
    std::int32_t Unsigned(std::uint64_t v){return append(v);}
    std::int32_t Real(double v){if(!std::isfinite(v))throw library::JsonException("Non-finite JSON number.");return append(v);}
    std::int32_t Text(const native::String& text){return checked([&]{return append(library::json_detail::validated_string(text));});}
    std::int32_t Array(){return append(Children{});}
    std::int32_t Object(){return append(Members{});}
    void Add(std::int32_t id,std::int32_t child){(void)at(child);std::get<Children>(at(id)).push_back(child);}
    void Put(std::int32_t id,const native::String& key,std::int32_t child){
        (void)at(child);
        const auto checkedKey=library::json_detail::validated_string(key);
        if(!std::get<Members>(at(id)).emplace(checkedKey,child).second)
            throw library::JsonException("Duplicate JSON object key: "+std::string(key));
    }
    std::int32_t Root() const { (void)at(storage_->root);return storage_->root; }
    void SetRoot(std::int32_t id){(void)at(id);storage_->root=id;}
    std::int32_t Kind(std::int32_t id) const {return static_cast<std::int32_t>(at(id).index());}
    bool Boolean(std::int32_t id) const{return std::get<bool>(at(id));}
    std::int64_t Integer(std::int32_t id) const{return std::get<std::int64_t>(at(id));}
    std::uint64_t Natural(std::int32_t id) const{return std::get<std::uint64_t>(at(id));}
    double Floating(std::int32_t id) const{return std::get<double>(at(id));}
    native::String String(std::int32_t id) const{return std::get<std::string>(at(id));}
    std::int32_t Count(std::int32_t id) const {
        const auto& n=at(id);const auto size=std::holds_alternative<Children>(n)?std::get<Children>(n).size():std::get<Members>(n).size();
        return native::cast<std::int32_t>(size);
    }
    std::int32_t Child(std::int32_t id,std::int32_t index) const {
        const auto& a=std::get<Children>(at(id));
        if(index<0||static_cast<std::size_t>(index)>=a.size())throw library::JsonException("Invalid JSON child index.");
        return a[static_cast<std::size_t>(index)];
    }
    native::Array<native::String> Keys(std::int32_t id) const {
        const auto& o=std::get<Members>(at(id));auto result=native::Array<native::String>::Create(o.size());std::size_t i=0;
        for(const auto& [key,value]:o){(void)value;result.At(i++)=key;}return result;
    }
    std::int32_t Member(std::int32_t id,const native::String& key) const {
        const auto& o=std::get<Members>(at(id));auto it=o.find(std::string(key));
        if(it==o.end())throw library::JsonException("Unknown JSON member.");return it->second;
    }
    // General numeric parse/format boundaries, not JSON grammar validation.
    std::int32_t Number(std::string_view text,bool real,bool negative) {
        return checked([&]{
            if(real){double v=0;auto r=native::from_decimal(text,v);
                if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size()||!std::isfinite(v))throw library::JsonException("Invalid or out-of-range JSON number.");return Real(v);}
            if(negative){std::int64_t v=0;auto r=std::from_chars(text.data(),text.data()+text.size(),v);
                if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size())throw library::JsonException("JSON integer is outside the signed 64-bit range.");return Signed(v);}
            std::uint64_t v=0;auto r=std::from_chars(text.data(),text.data()+text.size(),v);
            if(r.ec!=std::errc{}||r.ptr!=text.data()+text.size())throw library::JsonException("JSON integer is outside the unsigned 64-bit range.");return Unsigned(v);
        });
    }
    std::int32_t NumberBytes(const native::Array<std::uint8_t>& bytes,std::int32_t offset,std::int32_t count,bool real,bool negative) {
        if(offset<0||count<=0||offset>bytes.Count()||count>bytes.Count()-offset)throw library::JsonException("Invalid numeric byte span.");
        return Number(std::string_view(reinterpret_cast<const char*>(&bytes.At(offset)),static_cast<std::size_t>(count)),real,negative);
    }
    native::String NumberText(std::int32_t id) const {
        const auto& v=at(id);
        if(auto p=std::get_if<std::int64_t>(&v))return std::to_string(*p);
        if(auto p=std::get_if<std::uint64_t>(&v))return std::to_string(*p);
        if(auto p=std::get_if<double>(&v)){std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<*p;return out.str();}
        throw library::JsonException("Not a numeric tree value.");
    }
    bool HasMember(std::int32_t node,const std::string& key)const{return View(this,node).contains(key);}
    template<class T> std::int32_t AppendValue(const T& value){return checked([&]{return encode(value);});}
    template<class T> T ReadValue(std::int32_t node)const{return checked([&]{return library::json_detail::decode<T>(View(this,node));});}
    template<class T> static ValueTree FromObject(const T& value) {
        return checked([&]{ValueTree result;result.SetRoot(result.encode(value,0));return result;});
    }
    template<class T> T ToObject() const {
        return checked([&]{return library::json_detail::decode<T>(View(this,Root()));});
    }
};
inline ValueTree::View ValueTree::ArrayView::Iterator::operator*()const{return {owner,*it};}
inline std::pair<const std::string&,ValueTree::View> ValueTree::ObjectView::Iterator::operator*()const{return {it->first,View(owner,it->second)};}
}
