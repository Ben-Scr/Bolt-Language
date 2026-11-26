#pragma once
#include <bolt/Arena.hpp>
#include <bolt/native/ABI.hpp>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <string>

namespace bolt::tests {
inline void verify_native_arena_ownership() {
    auto require=[](bool good,const char* message){if(!good)throw std::runtime_error(message);};
    struct State {
        BrContext* context=br_create(0,100);
        BrType* arena_type=nullptr;
        void* owner=nullptr;
        ~State(){if(arena_type)br_drop(arena_type,&owner);br_destroy(context);}
    } state;
    auto* c=state.context;require(c!=nullptr,"native context allocation failed");
    state.arena_type=br_type(c,"Arena",native_abi::Arena,8);
    auto* string_type=br_type(c,"string",native_abi::String,8);
    auto* list_type=br_type(c,"List<string>",native_abi::List,8);br_elements(c,list_type,string_type,nullptr);
    auto* interface_type=br_type(c,"IStringable",native_abi::Interface,16);
    auto* root_type=br_type(c,"ArenaProbe",native_abi::Class,40);
    br_field(c,root_type,8,string_type);br_field(c,root_type,16,list_type);br_field(c,root_type,24,interface_type);
    br_arena_create(c,nullptr,&state.owner,65536,65536,0);
    auto* owner=static_cast<memory::Arena*>(state.owner);
    auto* root=static_cast<std::byte*>(br_arena_new(c,nullptr,&state.owner,root_type));
    native_abi::rethrow(c);require(owner->owns(root),"native root must belong to arena");
    std::memcpy(root,&root_type,sizeof(root_type));
    const std::string payload(120,'x');
    br_string(c,nullptr,root+8,payload.data(),payload.size());
    br_box(c,nullptr,interface_type,root+24,string_type,root+8);
    for(int i=0;i<20;++i)br_collection(c,nullptr,list_type,root+16,"Add",nullptr,root+8,nullptr);
    native_abi::rethrow(c);
    auto get_pointer=[](const void* slot){void* p=nullptr;std::memcpy(&p,slot,sizeof(p));return p;};
    require(owner->owns(get_pointer(root+8)),"native string wrapper escaped arena");
    auto string_bytes=[](const void* slot) {
        std::uint64_t length=0;
        const auto* bytes=br_string_data(slot,&length);
        return std::string_view(bytes,static_cast<std::size_t>(length));
    };
    const auto text=string_bytes(root+8);
    require(owner->owns(text.data()),"native string bytes escaped arena");
    require(text==payload,"native arena string bytes differ");
    require(owner->owns(get_pointer(root+16)),"native sequence wrapper escaped arena");
    auto* element=br_index(c,nullptr,list_type,root+16,19);native_abi::rethrow(c);
    require(owner->owns(element),"native sequence buffer escaped arena");
    require(owner->owns(get_pointer(element)),"native nested string escaped arena");
    auto* boxed=br_unbox(c,nullptr,root+24);native_abi::rethrow(c);
    require(owner->owns(boxed)&&owner->owns(get_pointer(boxed)),"native interface payload escaped arena");
    struct Copy {
        BrType* type;
        alignas(8) std::byte data[40]{};
        ~Copy(){br_drop(type,data);}
    } copy{root_type};
    br_copy(c,nullptr,root_type,copy.data,root);native_abi::rethrow(c);
    require(memory::owner_of(get_pointer(copy.data+8))==nullptr,"native copy-out retained arena string");
    br_arena_action(c,nullptr,&state.owner,0);native_abi::rethrow(c);
    require(owner->used()==0&&owner->capacity()==65536&&owner->peak()>0,"native reset statistics incorrect");
    const auto copied=string_bytes(copy.data+8);
    require(memory::owner_of(copied.data())==nullptr,"native copy-out retained arena string bytes");
    require(copied==payload,"native copied string lost after reset");
    require(br_count(c,nullptr,list_type,copy.data+16)==20,"native copied collection lost after reset");
    // Copy is dropped before its type descriptors and context are destroyed.
}
}
