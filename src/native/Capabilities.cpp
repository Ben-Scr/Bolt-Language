#include <bolt/native/Library.hpp>
#include <bolt/Types.hpp>
#include <functional>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
namespace bolt::native_abi {
namespace {
constexpr std::string_view operations[] = {
#define BOLT_LIBRARY_ENTRY(id, signature) signature,
#include "LibraryManifest.inc"
#undef BOLT_LIBRARY_ENTRY
};
}
bool library_pair(const bolt::Class& c){return c.is_native&&c.token.at.file.starts_with("<bolt-stdlib>")&&generic_base(c.name)=="System.Collections.Generic.KeyValuePair"&&generic_base(c.native_cpp)=="bolt::library::KeyValuePair";}
bool library_type_supported(const bolt::Class& c){
    if(library_pair(c))return true;
    if(!c.is_native||!c.token.at.file.starts_with("<bolt-stdlib>"))return false;
    if(c.name=="Bolt.Runtime.ExecutionMemory" && c.native_cpp=="bolt::platform::ExecutionMemory")return true;
    if(c.name=="Bolt.Runtime.AtomicOps" && c.native_cpp=="bolt::platform::AtomicOps")return true;
    if(c.name=="Bolt.Runtime.ThreadCallback"&&c.native_cpp=="bolt::platform::ThreadCallback")return true;
    if(c.name=="Bolt.Runtime.NativeString"&&c.native_cpp=="bolt::platform::NativeString")return true;
    static const std::unordered_map<std::string,std::string> types = {
#define BOLT_LIBRARY_LAYOUT(name, type) {name,#type},
#include "LibraryTypes.inc"
#undef BOLT_LIBRARY_LAYOUT
    };
    auto i=types.find(c.name);return i!=types.end()&&i->second==c.native_cpp;
}
bool library_primitive_method(const bolt::Class& c,const Method& m){
    if(!c.is_native||!c.token.at.file.starts_with("<bolt-stdlib>"))return false;
    if(c.name=="Bolt.Runtime.ExecutionMemory" && c.native_cpp=="bolt::platform::ExecutionMemory")return true;
    if(c.name=="Bolt.Runtime.AtomicOps" && c.native_cpp=="bolt::platform::AtomicOps")return m.name=="Add" || m.name=="Cas";
    if(c.name=="Bolt.Runtime.ThreadCallback"&&c.native_cpp=="bolt::platform::ThreadCallback")return m.name=="Capture"||m.name=="Entry"||m.name=="Cancel";
    if(c.name=="Bolt.Runtime.MemoryOps" && c.native_cpp=="bolt::system::MemoryOps" && m.is_static &&
       m.name=="Address" && m.type=="ulong" && m.parameters.size()==1 && m.parameters[0].type=="byte*")
        return true;
    if(c.name=="Bolt.Runtime.NativeString"&&c.native_cpp=="bolt::platform::NativeString"&&m.is_static&&m.parameters.size()==2){
        if(m.name=="Bytes"&&m.type=="byte*")return m.parameters[0].type=="string&"&&m.parameters[1].type=="ulong&";
        if(m.name=="FromUtf8"&&m.type=="string")return m.parameters[0].type=="byte*"&&m.parameters[1].type=="ulong";
    }
    if(c.name=="Bolt.Runtime.Values"&&c.native_cpp=="bolt::platform::Values"&&m.specialization_types.size()==1){
        const auto& t=m.specialization_types[0];if(t=="void"||is_reference(t))return false;
        if((m.name=="Hash"||m.name=="Format")&&m.parameters.size()==1&&m.parameters[0].type==t+"&")return m.type==(m.name=="Hash"?"ulong":"string");
        if(m.name=="Equal"&&m.type=="bool"&&m.parameters.size()==2)return m.parameters[0].type==t+"&"&&m.parameters[1].type==t+"&";
    }
    return c.name=="Bolt.Runtime.Bits"&&c.native_cpp=="bolt::platform::Bits"&&(m.name=="Add64"||m.name=="Multiply64")&&m.type=="ulong"&&m.parameters.size()==2&&m.parameters[0].type=="ulong"&&m.parameters[1].type=="ulong";
}
std::string library_method_key(const bolt::Class& c,const Method& m){
    std::string key=c.native_cpp+"::"+(m.is_constructor?"$new":m.native_name.empty()?m.name:m.native_name)+"(";
    bool first=true;for(const auto& p:m.parameters){if(!first)key+=",";first=false;key+=referred(p.type);}
    return key+")->"+(m.is_constructor?c.name:referred(m.type));
}
std::string library_property_key(const bolt::Class& c,const Field& f){return c.native_cpp+"::"+f.native_getter+"()->"+f.type;}
std::uint64_t library_schema() noexcept {
    // Standalone programs need the manifest fingerprint at startup. Keep the
    // compiler's operation names and lookup tables out of that runtime path.
    constexpr auto schema = [] {
        std::uint64_t hash = 14695981039346656037ull;
        for (const auto op : operations) {
            for (const unsigned char ch : op) {
                hash ^= ch;
                hash *= 1099511628211ull;
            }
            hash *= 1099511628211ull; // terminating zero separates adjacent operation names
        }
        return hash;
    }();
    return schema;
}
int library_operation(const std::string& key) {
    static const auto ids = [] {
        std::unordered_map<std::string_view, int> result;
        for (std::size_t i = 0; i < std::size(operations); ++i)
            result.emplace(operations[i], static_cast<int>(i));
        return result;
    }();
    const auto entry = ids.find(key);
    return entry == ids.end() ? -1 : entry->second;
}
std::string direct_extern_requirement(const Program&,const Method& m){
    const auto label="external function "+m.name;
    if(!m.is_extern||m.native_abi!="C")return label+" requires C linkage";
    if(!m.is_static||m.is_constructor||m.is_destructor)return label+" requires a static C function";
    if(!m.generic_parameters.empty()||!m.pack_parameter.empty()||!m.specialization_types.empty()||m.native_pack)
        return label+" uses generic or variadic native linkage";
    const auto& symbol=m.native_name.empty()?m.name:m.native_name;
    auto letter=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    if(symbol.empty()||!letter(symbol.front()))return label+" has an unsupported C symbol name";
    for(char c:symbol)if(!letter(c)&&(c<'0'||c>'9'))return label+" has an unsupported C symbol name";
    auto scalar=[](const std::string& t){return is_numeric(t)||t=="bool"||t=="char"||is_pointer(t);};
    if(m.type!="void"&&!scalar(m.type))return label+" returns unsupported C ABI type "+m.type;
    for(const auto& parameter:m.parameters){
        if(parameter.variadic||parameter.pack)return label+" uses variadic C parameters";
        if(!scalar(referred(parameter.type)))return label+" has unsupported C ABI parameter "+parameter.type;
    }
    return {};
}
std::string direct_requirement(const Program& p){
    for(const auto& feature:p.cpp_features)if(feature!="local separately linked project libraries"&&feature!="abstract classes"&&feature!="async/await tasks"&&feature!="opt-in reflection"&&feature!="native standard library / interop"&&feature!="native C/C++ interoperability"&&feature!="exception handling"&&feature!="destructors and RAII"&&feature!="struct value layout"&&
        feature!="command-line entry arguments"&&feature!="bitwise operators"&&feature!="enum bitwise operators"&&feature!="conditional operator"&&feature!="null coalescing"&&feature!="range-for"&&feature!="Unicode char"&&feature!="Unicode char conversions"&&feature!="Unicode string methods"&&feature!="Unicode string ByteLength"&&feature!="Unicode string indexing")return feature;
    std::string failure;std::unordered_set<std::string> visited;
    std::function<void(std::string)> type=[&](std::string t){
        t=referred(t);if(t.empty()||!visited.insert(t).second)return;
        // A raw pointer is ABI-opaque. The native backend does not need the
        // pointee layout merely to forward that address to an extern C call.
        // This is important for source-authored platform APIs that use pointers
        // to opaque OS structures such as timespec, tm, FILETIME or SYSTEMTIME.
        if(is_pointer(t))return;
        if(is_array(t)||is_optional(t)){type(element_type(t));return;}
        if(is_collection(t)){for(const auto& a:generic_args(t))type(a);return;}
        if(auto c=p.find_class(t);c&&c->is_native&&!library_type_supported(*c))failure="native type "+c->name;
    };
    std::function<void(const Expr&)> expression=[&](const Expr& e){
        type(e.type);
        if((e.kind==Expr::Kind::Call||e.kind==Expr::Kind::New||!e.resolved.empty())&&!e.owner.empty()) {
            if(auto c=p.find_class(e.owner)) {
                auto m=method_by_id(*c,e.resolved);
                if(c->is_native){
                    if(!library_type_supported(*c))failure="native type "+c->name;
                    else if(m&&!library_primitive_method(*c,*m)&&library_operation(library_method_key(*c,*m))<0)failure="native operation "+c->name+"."+m->name;
                } else if(m&&m->is_extern){
                    if(auto reason=direct_extern_requirement(p,*m);!reason.empty())failure=reason;
                }
            }
        }
        if(e.binding==Expr::Binding::Field)if(auto c=p.find_class(e.owner);c&&c->is_native&&!library_pair(*c)){
            auto f=find_field(*c,e.text);if(!library_type_supported(*c)||!f||library_operation(library_property_key(*c,*f))<0)failure="native property "+c->name+"."+e.text;
        }
        if(e.arena)expression(*e.arena);for(const auto& a:e.args)expression(*a);
    };
    std::function<void(const Stmt&)> statement=[&](const Stmt& s){type(s.type);
        if(s.kind==Stmt::Kind::ForEach&&s.expression){const auto& t=s.expression->type;if(!is_array(t)&&t!="string"&&collection_kind(t)!="List"&&collection_kind(t)!="HashSet"&&collection_kind(t)!="Dictionary"&&collection_kind(t)!="Queue")failure="range-for over "+t;}
if(s.expression)expression(*s.expression);if(s.condition)expression(*s.condition);if(s.increment)expression(*s.increment);if(s.init)statement(*s.init);if(s.body)statement(*s.body);if(s.alternative)statement(*s.alternative);for(const auto& v:s.statements)statement(*v);};
    for(const auto& c:p.classes)if(emitted_class(c)&&!c.is_native&&(c.generic_parameters.empty()&&c.pack_parameter.empty())){
        if(!c.base.empty())if(auto* base=p.find_class(c.base);base&&base->is_native&&base->token.at.file.find("Exceptions.bolt")==std::string::npos)
            failure="user inheritance from native library type "+base->name;
        for(const auto& f:c.fields){type(f.type);if(f.initializer)expression(*f.initializer);}
        for(const auto& m:c.methods)if(m.generic_parameters.empty()&&m.pack_parameter.empty()&&m.analyzed){
            if(m.is_extern)if(auto reason=direct_extern_requirement(p,m);!reason.empty())failure=reason;
            type(m.type);for(const auto& a:m.parameters)type(a.type);for(const auto& a:m.base_arguments)expression(*a);for(const auto& a:m.field_initializers)expression(*a.second);
            if(m.body)statement(*m.body);
        }
    }
    return failure;
}
}
