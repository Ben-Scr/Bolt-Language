#include "Lowering.hpp"
#include <algorithm>
#include <limits>

namespace bolt::ir::lowering {

// These source-runtime descriptors retain and release the referenced state on copy.
int Layout::standalone_kind(const Class* cls) const {
    if(cls){
        const auto& file=cls->token.at.file;
        if(file=="<bolt-stdlib>/System/Shared.bolt" && generic_base(name)=="System.Memory.Shared")return 24;
        if(file=="<bolt-stdlib>/System/SystemLease.bolt" && name=="Bolt.Runtime.SystemLease")return 22;
        if(file=="<bolt-stdlib>/System/IO/StreamHandle.bolt" && name=="Bolt.Runtime.IO.StreamHandle")return 20;
    }
    return kind;
}

namespace {
bool collect_class_copy(const Lower& lower, const Layout& value, std::size_t offset,
                        std::uint64_t& bytes, std::vector<std::pair<std::size_t,std::string>>& headers) {
    if (native_abi::scalar_storage_kind(value.kind)) {
        bytes |= (std::numeric_limits<std::uint64_t>::max() >> (64 - value.size)) << offset;
        return true;
    }
    if (value.kind != native_abi::Class && value.kind != native_abi::Struct) return false;
    const auto* cls = lower.program.find_class(value.name);
    if (!cls || cls->is_native || value.marker != native_abi::no_marker) return false;
    if (lower.core() && value.standalone_kind(cls) != value.kind) return false;
    if (value.kind == native_abi::Class) {
        // The inherited fields are flattened, but a native base can own hidden storage.
        for (const auto* base = cls; !base->base.empty();) {
            base = lower.program.find_class(base->base);
            if (!base || base->is_native || lower.layouts.at(base->name).kind != native_abi::Class) return false;
        }
        headers.emplace_back(offset, value.name);
    } else if (std::any_of(cls->methods.begin(), cls->methods.end(),
                          [](const Method& method) { return method.is_destructor; })) return false;
    for (const auto& field : value.fields) {
        if (is_reference(field.type) || !collect_class_copy(lower, lower.layouts.at(referred(field.type)),
                offset + field.offset, bytes, headers)) return false;
    }
    return true;
}
}

void Lower::finish_layouts() {
    // Container element discovery may encounter a still-incomplete class.
    // Recompute only inline dependencies after the entire discovery chain;
    // handle/pointer edges do not contribute recursively to value size.
    for (const auto& name : pending_layouts) {
        auto& value = layouts.at(name);
        const auto& target=target_layouts.get(name);
        value.size=target.size;value.alignment=target.alignment;value.marker=target.marker;value.fields=target.fields;
    }
    for (const auto& name : pending_layouts) {
        auto& value = layouts.at(name);
        if (value.kind != native_abi::Class || value.size > 64) continue;
        if (!collect_class_copy(*this, value, 0, value.class_field_bytes, value.class_headers)) {
            value.class_field_bytes = 0;
            value.class_headers.clear();
        }
    }
    // Class copy plans must be complete before classifying optional payloads.
    for (const auto& name : pending_layouts) {
        auto& value = layouts.at(name);
        if (value.kind != native_abi::Optional || target_layouts.get("null").size != 8) continue;
        const auto& payload = layouts.at(referred(element_type(name)));
        value.inline_optional = payload.size <= 64 && (native_abi::scalar_storage_kind(payload.kind) ||
            payload.plain_struct || payload.padded_field_bytes || !payload.class_headers.empty());
    }
    pending_layouts.clear();
}

Layout& Lower::layout(std::string t) {
    ++layout_depth;
    Layout* result=nullptr;
    try{result=&layout_raw(std::move(t));}catch(...){--layout_depth;throw;}
    --layout_depth;if(!layout_depth&&!pending_layouts.empty())finish_layouts();return *result;
}

Layout& Lower::layout_raw(std::string t) {
    t=referred(t);
    if(auto i=layouts.find(t);i!=layouts.end())return i->second;
    if(t.empty()||t=="void")throw std::runtime_error("void does not have a native value layout");
    pending_layouts.push_back(t);
    Layout l;l.name=t;l.id=layouts.size();
    auto type_symbol=[&]{return options.source_core?mangle("type::"+t):"b_type_"+std::to_string(l.id);};
    l.symbol=type_symbol();
    using namespace native_abi;
    if(is_optional(t)) {
        l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=native_abi::Optional;l.aggregate=true;
        auto [it,inserted]=layouts.emplace(t,std::move(l));(void)inserted;
        module.globals.push_back({it->second.symbol,std::vector<std::uint8_t>(8),true,8,{}});
        const auto& inner=layout(element_type(t));it->second.size=8+(inner.size+7)/8*8;it->second.alignment=8;
        return it->second;
    }
    if(t=="#error-state"){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=native_abi::ErrorState;l.aggregate=true;}
    else if(t=="#native-range"){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=native_abi::Range;l.aggregate=true;}
    else if(is_pointer(t)||t=="null"){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=native_abi::Pointer;}
    else if(t=="char"){l.size=4;l.alignment=4;l.ir=Type::I32;l.kind=Unsigned;}
    else if(is_integer(t)){l.size=static_cast<std::size_t>(bits(t)/8);l.alignment=std::max<std::size_t>(1,std::min<std::size_t>(8,l.size));l.ir=l.size==1?Type::I8:l.size==2?Type::I16:l.size==4?Type::I32:Type::I64;l.kind=is_unsigned(t)?Unsigned:Signed;}
    else if(is_floating(t)){l.size=t=="float"?4:8;l.alignment=l.size;l.ir=t=="float"?Type::F32:Type::F64;l.kind=Float;}
    else if(t=="bool"){l.size=1;l.alignment=1;l.ir=Type::I8;l.kind=Bool;}
    else if(t=="string"){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=String;l.aggregate=true;}
    else if(t=="Arena"){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=native_abi::Arena;l.aggregate=true;}
    else if(is_array(t)){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.kind=Array;l.aggregate=true;}
    else if(is_collection(t)){l.size=8;l.alignment=8;l.ir=Type::Ptr;l.aggregate=true;auto k=collection_kind(t);l.kind=k=="List"?List:k=="HashSet"?HashSet:k=="Dictionary"?Dictionary:Queue;}
    else if(auto* e=program.find_enum(t)){auto underlying=layout(e->underlying);l.size=underlying.size;l.alignment=underlying.alignment;l.ir=underlying.ir;l.kind=underlying.kind;}
    else if(auto* c=program.find_class(t)){
        if(c->is_native&&!native_abi::library_pair(*c)){
            if(!native_abi::library_type_supported(*c))throw std::runtime_error("native library type is not in the shared ABI: "+t);
            l.size=native_abi::library_storage_size(c->native_cpp);l.alignment=8;l.kind=LibraryValue;l.ir=Type::Ptr;l.aggregate=true;
            auto [it,inserted]=layouts.emplace(t,std::move(l));(void)inserted;
            module.globals.push_back({it->second.symbol,std::vector<std::uint8_t>(8),true,8,{}});if(!c->base.empty())layout(c->base);return it->second;
        }
        if(c->name=="Bolt.Runtime.ThreadHandle" && c->token.at.file=="<bolt-stdlib>/System/ThreadKernel.bolt"){
            l.kind=SharedResource;l.ir=Type::Ptr;l.aggregate=true;l.size=8;l.alignment=8;
            l.fields.push_back({c->name,"Pointer","Bolt.Runtime.ThreadResource*",0});
            auto [it,inserted]=layouts.emplace(t,std::move(l));(void)inserted;
            module.globals.push_back({it->second.symbol,std::vector<std::uint8_t>(8),true,8,{}});
            layout("Bolt.Runtime.ThreadResource*");return it->second;
        }
        if(c->is_interface){l.kind=native_abi::Interface;l.ir=Type::Ptr;l.aggregate=true;l.size=16;l.alignment=8;auto [it,inserted]=layouts.emplace(t,std::move(l));(void)inserted;module.globals.push_back({it->second.symbol,std::vector<std::uint8_t>(8),true,8,{}});return it->second;}
        // Reserve the descriptor before discovering fields. Classes may own
        // List<Self>/Self[] without recursively embedding their own storage.
        // Structs reserve only the type metadata global; their value storage
        // starts at field offset zero and has no hidden runtime descriptor.
        l.kind=c->is_struct?native_abi::Struct:native_abi::Class;l.ir=Type::Ptr;l.aggregate=true;l.size=c->is_struct?0:8;l.alignment=c->is_struct?1:8;
        auto [it,inserted]=layouts.emplace(t,std::move(l));(void)inserted;
        auto& result=it->second;
        module.globals.push_back({result.symbol,std::vector<std::uint8_t>(8),true,8,{}});
        if(!c->is_struct&&!c->base.empty()){const auto base=layout(c->base);result.size=base.size;result.alignment=base.alignment;result.fields=base.fields;result.marker=base.marker;}
        for(const auto& f:c->fields)if(!f.is_static){const auto field=layout(f.type);result.size=(result.size+field.alignment-1)/field.alignment*field.alignment;result.fields.push_back({c->name,f.name,f.type,result.size});result.size+=field.size;result.alignment=std::max(result.alignment,field.alignment);}
        if(c->is_struct)result.size=(std::max<std::size_t>(1,result.size)+result.alignment-1)/result.alignment*result.alignment;
        else {
            result.alignment=8;result.size=(result.size+7)/8*8;
            if(result.marker==static_cast<std::size_t>(-1))for(const auto& m:c->methods)if(m.is_destructor){result.marker=result.size;result.size+=8;break;}
        }
        // An array of these records already has its complete default value
        // after zero allocation. A nested field initializer or user ctor
        // must still run for each element, including its observable effects.
        if (c->is_struct && !c->is_native && std::none_of(c->methods.begin(), c->methods.end(),
                [](const Method& m) { return m.is_constructor || m.is_destructor; })) {
            result.zero_default = std::all_of(c->fields.begin(), c->fields.end(), [&](const Field& field) {
                if (field.is_static) return true;
                if (field.initializer || is_reference(field.type)) return false;
                const auto& nested = layouts.at(referred(field.type));
                return !nested.aggregate || nested.zero_default;
            });
        }
        // Runtime copies preserve every declared field and zero padding on
        // assignment to distinct storage. A raw move is valid if the fields
        // cover the entire object; small padded layouts retain a field map.
        if (c->is_struct && !c->is_native &&
            std::none_of(c->methods.begin(),c->methods.end(),[](const Method& m){return m.is_destructor;})) {
            std::size_t end = 0;
            std::uint64_t field_bytes = 0;
            bool plain = true;
            bool scalar_fields = true;
            for (const auto& f : result.fields) {
                const auto& nested = layouts.at(referred(f.type));
                if (is_reference(f.type) || !nested.size ||
                    (nested.aggregate && !nested.plain_struct && !nested.padded_field_bytes)) {
                    scalar_fields = false;
                    break;
                }
                plain = plain && f.offset == end && !nested.padded_field_bytes;
                end = f.offset + nested.size;
                if (result.size <= 64) {
                    const auto mask = nested.padded_field_bytes ? nested.padded_field_bytes :
                        std::numeric_limits<std::uint64_t>::max() >> (64 - nested.size);
                    field_bytes |= mask << f.offset;
                }
            }
            result.plain_struct = scalar_fields && plain && end == result.size;
            if (scalar_fields && !result.plain_struct) result.padded_field_bytes = field_bytes;
        }
        return result;
    }else throw std::runtime_error("no native layout for type '"+t+"'");
    // Recursive element discovery happens only after insertion (pointer cycles
    // and nested containers do not recursively expand their storage).
    l.id=layouts.size();l.symbol=type_symbol();
    auto [it,inserted]=layouts.emplace(t,std::move(l));(void)inserted;
    module.globals.push_back({it->second.symbol,std::vector<std::uint8_t>(8),true,8,{}});
    if(is_array(t))layout(element_type(t));
    if(is_collection(t))for(const auto& a:generic_args(t))layout(a);
    return it->second;
}

Reg Lower::field(Builder& b,const std::string& c,const std::string& n,Reg self) {const auto& l=layout(c);for(auto i=l.fields.rbegin();i!=l.fields.rend();++i)if(i->name==n)return b.offset(self,i->offset);throw std::runtime_error("native field layout missing: "+c+"."+n);}

const Method* Lower::default_constructor(const Class& c) {
    const Method* selected = nullptr;
    std::size_t best = static_cast<std::size_t>(-1);
    for (const auto& m : c.methods) {
        if (!m.is_constructor) continue;
        bool eligible = true;
        for (const auto& p : m.parameters)
            if (!p.default_value && !p.variadic) eligible = false;
        if (!eligible) continue;
        const auto score = m.parameters.size() +
            ((!m.parameters.empty() && m.parameters.back().variadic) ? 4u : 0u);
        if (score < best) { best = score; selected = &m; }
    }
    return selected;
}

Type Lower::result_type(const Method& m) {if(m.is_constructor||m.type=="void")return Type::Void;if(is_reference(m.type))return Type::Ptr;auto l=layout(m.type);return l.aggregate?Type::Void:l.ir;}

std::vector<Type> Lower::parameters(const Method& m) {std::vector<Type> p{Type::Ptr};if(m.type!="void"&&!m.is_constructor&&!is_reference(m.type)&&layout(m.type).aggregate)p.push_back(Type::Ptr);if(!m.is_static)p.push_back(Type::Ptr);for(const auto& a:m.parameters)p.push_back(is_reference(a.type)?Type::Ptr:layout(a.type).ir);return p;}

} // namespace bolt::ir::lowering
