#pragma once
#include <bolt/native/Library.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace bolt::native_abi {
// Direct x86-64, ARM64 and RV64 backends share the 64-bit value ABI. Classes
// keep Bolt's runtime descriptor header, while structs use a plain sequential
// native layout with no hidden prefix or suffix. Field order is declaration
// order and padding follows each field's natural target alignment.
inline constexpr std::size_t pointer_bytes = 8;
inline constexpr std::size_t no_marker = static_cast<std::size_t>(-1);
struct ValueFieldLayout { std::string owner, name, type; std::size_t offset; };
struct ValueLayout {
    std::size_t size = 0, alignment = 1, marker = no_marker;
    std::vector<ValueFieldLayout> fields;
};
class ValueLayouts {
    const Program& program;
    const std::size_t address_bytes, max_scalar_alignment;
    std::map<std::string,ValueLayout> values;
    std::set<std::string> active;
    static std::size_t add(std::size_t a,std::size_t b) {
        if(b>std::numeric_limits<std::size_t>::max()-a)throw std::runtime_error("Bolt value layout exceeds the target address space");
        return a+b;
    }
    static std::size_t aligned(std::size_t size,std::size_t alignment) {
        if(!alignment)throw std::runtime_error("Bolt value layout has zero alignment");
        return add(size,alignment-1)/alignment*alignment;
    }
    std::size_t scalar_alignment(std::size_t size) const {
        return std::max<std::size_t>(1,std::min(max_scalar_alignment,size));
    }
public:
    explicit ValueLayouts(const Program& p):program(p),
        address_bytes(p.preprocessor.architecture=="X86" || p.preprocessor.architecture=="ARM32" ? 4 : pointer_bytes),
        max_scalar_alignment(p.preprocessor.architecture=="X86" ? 4 : 8) {}
    const ValueLayout& get(std::string type) {
        type=referred(type); // A reference aliases the referent's storage.
        if(auto found=values.find(type);found!=values.end())return found->second;
        if(!active.insert(type).second)throw std::runtime_error("recursive inline native layout: "+type);
        ValueLayout value;
        if(is_pointer(type)||is_array(type)||is_collection(type)||type=="string"||type=="Arena"||type=="null"||type=="#error-state"||type=="#native-range") {
            value.size=address_bytes;value.alignment=address_bytes;
        }
        else if(is_optional(type)) {
            const auto& element=get(element_type(type));
            value.alignment=std::max(address_bytes,element.alignment);
            value.size=add(aligned(address_bytes,value.alignment),aligned(element.size,value.alignment));
        }
        else if(type=="bool") {value.size=1;value.alignment=1;}
        else if(type=="char") {value.size=4;value.alignment=4;}
        else if(is_numeric(type)) {
            value.size=static_cast<std::size_t>(bits(type)/8);value.alignment=scalar_alignment(value.size);
        }
        else if(const auto* en=program.find_enum(type)) {
            const auto& underlying=get(en->underlying);value.size=underlying.size;value.alignment=underlying.alignment;
        }
        else if(const auto* c=program.find_class(type)) {
            if(c->is_native&&!library_pair(*c)) {
                if(!library_type_supported(*c))throw std::runtime_error("native library type has no registered Bolt ABI layout: "+type);
                value.size=library_storage_size(c->native_cpp,program.preprocessor);value.alignment=scalar_alignment(8);
            } else if(c->is_interface) {
                value.size=2*address_bytes;value.alignment=address_bytes;
            } else if(c->is_struct) {
                // Structs are native value types by default: no BrType* header,
                // no hidden construction marker and no field reordering.
                value.size=0;value.alignment=1;
                for(const auto& field:c->fields)if(!field.is_static) {
                    const auto& layout=get(field.type);
                    value.size=aligned(value.size,layout.alignment);
                    value.fields.push_back({c->name,field.name,field.type,value.size});
                    value.size=add(value.size,layout.size);
                    value.alignment=std::max(value.alignment,layout.alignment);
                }
                // Keep empty structs addressable just like a native C++ value.
                value.size=aligned(std::max<std::size_t>(1,value.size),value.alignment);
            } else {
                value.size=address_bytes;value.alignment=address_bytes;
                if(!c->base.empty())value=get(c->base);
                for(const auto& field:c->fields)if(!field.is_static) {
                    const auto& layout=get(field.type);
                    value.size=aligned(value.size,layout.alignment);
                    value.fields.push_back({c->name,field.name,field.type,value.size});
                    value.size=add(value.size,layout.size);
                    value.alignment=std::max(value.alignment,layout.alignment);
                }
                value.size=aligned(value.size,value.alignment);
                if(value.marker==no_marker)for(const auto& method:c->methods)if(method.is_destructor) {
                    value.marker=value.size;value.size=add(value.size,address_bytes);break;
                }
            }
        } else throw std::runtime_error("type '"+type+"' does not have a Bolt value layout");
        active.erase(type);
        return values.emplace(std::move(type),std::move(value)).first->second;
    }
};
inline std::size_t value_size(const Program& program,const std::string& type) {
    return ValueLayouts(program).get(type).size;
}
}
