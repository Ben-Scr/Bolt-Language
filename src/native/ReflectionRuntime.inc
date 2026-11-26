// Runtime side of the uniform member-call ABI emitted by the native lowering pass.
namespace {
using Tree=bolt::platform::ValueTree;
using JsonError=bolt::library::JsonException;
using Serial=bolt::native_abi::SerialMethod;

void invoke_member(BrContext* context,MemberCall call,void* result,void* object,void** args=nullptr){
    bolt::native_backend::detail::call_generated(call,context,result,object,args);
    // Preserve the generated method's diagnostic and dynamic exception payload.
    if(context->failed)throw std::runtime_error("reflected method failed");
}
Box read_member(BrContext* c,BrType* owner,const ReflectedField& f,void* object){
    if(!f.get)return Box(f.type,static_cast<std::byte*>(object)+f.offset);
    Box value(f.type);
    if(f.flags&SnapshotGetter){
        Box copy(owner,object);invoke_member(c,f.get,value.data(),copy.data());
    }else invoke_member(c,f.get,value.data(),object);
    return value;
}
void write_member(BrContext* c,const ReflectedField& f,void* object,void* value){
    if(f.set)invoke_member(c,f.set,nullptr,object,&value);
    else assign(f.type,static_cast<std::byte*>(object)+f.offset,value);
}

struct ObjectCodec {
    BrContext* c;
    Tree& tree;
    void construct(BrType* type,void* out){
        initialize(c,type,out);
        if(c->failed)throw std::runtime_error("object initialization failed");
    }
    template<class F> static decltype(auto) scalar(BrType* type,F&& f){
        if(type->name=="char")return f.template operator()<char32_t>();
        switch(type->kind){
        case Bool:return f.template operator()<bool>();
        case Signed:
            if(type->size==8)return f.template operator()<std::int64_t>();
            if(type->size==4)return f.template operator()<std::int32_t>();
            return f.template operator()<std::int16_t>();
        case Unsigned:
            if(type->size==8)return f.template operator()<std::uint64_t>();
            if(type->size==4)return f.template operator()<std::uint32_t>();
            if(type->size==2)return f.template operator()<std::uint16_t>();
            return f.template operator()<std::uint8_t>();
        case Float:
            if(type->size==8)return f.template operator()<double>();
            return f.template operator()<float>();
        case String:return f.template operator()<bolt::native::String>();
        default:throw JsonError("Serialization requires a supported value/collection type or [Reflect] type.");
        }
    }
    int encode(BrType* type,void* object,int depth=0){
        bolt::library::json_detail::depth_check(depth);
        if(type->kind==Array && type->rank!=1)throw JsonError("Serialization requires a rank-one array.");
        if(type->kind==Optional)return read<std::uint8_t>(object)?encode(type->key,static_cast<std::byte*>(object)+8,depth+1):tree.Null();
        if(type->kind==Array || type->kind==List){
            const auto id=tree.Array();
            if(auto* seq=read_sequence(object))
                for(std::size_t i=0;i<seq->count;++i)tree.Add(id,encode(type->key,seq->at(i),depth+1));
            return id;
        }
        if(auto* meta=type->metadata.get()){
            if(auto get=meta->method(Serial::Export);get.call){
                Box payload(get.type);invoke_member(c,get.call,payload.data(),object);
                return encode(get.type,payload.data(),depth+1);
            }
            if(auto count=meta->method(Serial::Count);count.call){
                const auto at=meta->method(Serial::At),key=meta->method(Serial::Key),value=meta->method(Serial::Value);
                const bool string_map=key.call && key.type->kind==String;
                const auto id=string_map?tree.Object():tree.Array();
                std::int32_t n=0;invoke_member(c,count.call,&n,object);
                for(std::int32_t i=0;i<n;++i){
                    Box item(at.type);void* index=&i;invoke_member(c,at.call,item.data(),object,&index);
                    if(!key.call){tree.Add(id,encode(at.type,item.data(),depth+1));continue;}
                    Box k(key.type),v(value.type);
                    invoke_member(c,key.call,k.data(),item.data());invoke_member(c,value.call,v.data(),item.data());
                    if(string_map)tree.Put(id,bolt::native::String(text(k.data())),encode(value.type,v.data(),depth+1));
                    else{
                        const auto row=tree.Object();tree.Put(row,"Key",encode(key.type,k.data(),depth+1));
                        tree.Put(row,"Value",encode(value.type,v.data(),depth+1));tree.Add(id,row);
                    }
                }
                return id;
            }
            if(meta->reflected){
                const auto id=tree.Object();
                for(const auto& f:meta->fields)if(!(f.flags&IgnoreJson)){
                    auto v=read_member(c,type,f,object);tree.Put(id,f.json_name,encode(f.type,v.data(),depth+1));
                }
                return id;
            }
        }
        return scalar(type,[&]<class T>(){return tree.AppendValue(BridgeCodec<T>::get(type,object));});
    }
    void decode(BrType* type,void* out,int node,int depth=0){
        bolt::library::json_detail::depth_check(depth);
        const auto kind=tree.Kind(node);
        if(type->kind==Array && type->rank!=1)throw JsonError("Serialization requires a rank-one array.");
        if(type->kind==Optional){
            if(kind!=0){
                write(out,std::uint8_t{1});
                decode(type->key,static_cast<std::byte*>(out)+8,node,depth+1);
            }
            return;
        }
        if(type->kind==Array || type->kind==List){
            if(kind!=6)throw JsonError("Expected a JSON array.");
            const auto n=tree.Count(node);
            auto& seq=sequence(type,out);seq.reserve(n);seq.count=n;
            for(int i=0;i<n;++i)decode(type->key,seq.at(i),tree.Child(node,i),depth+1);
            return;
        }
        if(auto* meta=type->metadata.get()){
            if(auto make=meta->method(Serial::Import);make.call){
                Box payload(meta->method(Serial::Export).type);decode(payload.type,payload.data(),node,depth+1);
                void* arg=payload.data();invoke_member(c,make.call,out,nullptr,&arg);return;
            }
            if(auto add=meta->method(Serial::Add);add.call){
                auto key=meta->method(Serial::Key),value=meta->method(Serial::Value);
                const bool string_map=key.call && key.type->kind==String;
                if(kind!=(string_map?7:6))throw JsonError(string_map?"Expected a JSON object.":"Expected a JSON array.");
                construct(type,out);
                auto keys=string_map?tree.Keys(node):bolt::native::Array<bolt::native::String>{};
                for(int i=0;i<tree.Count(node);++i){
                    Box item(key.call?value.type:meta->method(Serial::At).type),k(key.type),ignored(add.type);
                    void* args[2]={item.data(),nullptr};
                    auto child=string_map?tree.Member(node,keys.At(i)):tree.Child(node,i);
                    if(key.call){
                        if(string_map)put_text(k.data(),keys.At(i));
                        else{
                            if(tree.Kind(child)!=7||!tree.HasMember(child,"Key")||!tree.HasMember(child,"Value"))
                                throw JsonError("Map entries require Key and Value.");
                            decode(key.type,k.data(),tree.Member(child,"Key"),depth+1);
                            child=tree.Member(child,"Value");
                        }
                        args[0]=k.data();args[1]=item.data();
                    }
                    decode(item.type,item.data(),child,depth+1);
                    invoke_member(c,add.call,ignored.data(),out,args);
                }
                return;
            }
            if(meta->reflected){
                if(kind!=7)throw JsonError("Expected an object for the reflected type.");
                construct(type,out);
                std::set<std::string_view> known;
                for(const auto& f:meta->fields){
                    if(!known.insert(f.json_name).second)throw JsonError("Duplicate reflected JSON name.");
                    if((f.flags&IgnoreJson)||!tree.HasMember(node,f.json_name))continue;
                    if(f.flags&ReadOnlyField){
                        if(!(f.flags&PropertyField))throw JsonError("Cannot deserialize into a read-only field: "+f.name);
                        continue;
                    }
                    Box v(f.type);decode(f.type,v.data(),tree.Member(node,f.json_name),depth+1);
                    write_member(c,f,out,v.data());
                }
                return;
            }
        }
        scalar(type,[&]<class T>(){BridgeCodec<T>::put(type,out,tree.ReadValue<T>(node));});
    }
};
}

extern "C" {
void br_tree_object(BrContext* c,const char* site,BrType* value,void* object,BrType* tree_type,void* slot,std::int32_t encode)noexcept{
    checked(c,site,[&]{
        if(encode){
            Tree tree;tree.SetRoot(ObjectCodec{c,tree}.encode(value,object));
            BridgeCodec<Tree>::put(tree_type,slot,tree);
        }else{
            auto& tree=library_object<Tree>(slot);
            Box decoded(value);ObjectCodec{c,tree}.decode(value,decoded.data(),tree.Root());
            if(!c->failed)assign(value,object,decoded.data());
        }
    });
}
void br_reflected_type(BrContext* c,BrType* t,const char* space,const char* attrs)noexcept{
    checked(c,nullptr,[&]{
        auto& meta=t->get_metadata();meta.reflected=true;
        meta.info.Name=t->name;meta.info.Namespace=space;meta.info.Attributes=attrs;meta.info.Struct=t->kind==Struct;
    });
}
void br_reflected_field(BrContext* c,BrType* t,const char* name,const char* json_name,const char* attrs,
                        BrType* value,std::uint64_t offset,std::uint32_t flags,void* get,void* set)noexcept{
    checked(c,nullptr,[&]{
        auto& meta=*t->metadata;
        meta.fields.push_back({name,json_name,value,offset,flags,reinterpret_cast<MemberCall>(get),reinterpret_cast<MemberCall>(set)});
        meta.info.Fields.emplace_back(name,value->name,(flags&ReadOnlyField)!=0,attrs,(flags&PropertyField)!=0);
    });
}
void br_reflected_method(BrContext* c,BrType* t,const char* name,const char* result,const char* params,const char* signature,
                         const char* attrs,std::uint32_t is_static,void* method)noexcept{
    checked(c,nullptr,[&]{
        auto& meta=*t->metadata;
        meta.methods.push_back({name,signature,reinterpret_cast<MemberCall>(method)});
        meta.info.Methods.emplace_back(name,result,params,is_static!=0,attrs);
    });
}
void br_serialization_method(BrContext* c,BrType* t,std::uint32_t slot,BrType* result,void* method)noexcept{
    checked(c,nullptr,[&]{t->get_metadata().serial.at(slot)={result,reinterpret_cast<MemberCall>(method)};});
}
void br_reflection(BrContext* c,const char* site,const char* operation,BrType* owner,BrType* result,void* out,
                   void* object,const void* name,BrType* value,const char* signature,void** args)noexcept{
    checked(c,site,[&]{
        const auto* meta=owner->metadata.get();
        if(!meta||!meta->reflected)throw bolt::library::ReflectionException("The type is not marked [Reflect].");
        const std::string_view op(operation);
        if(op=="GetType"){
            BridgeCodec<bolt::library::TypeInfo>::put(result,out,bolt::library::TypeInfo(meta->info));return;
        }
        auto member=text(name);
        if(op=="Invoke"){
            MemberCall match=nullptr;
            for(const auto& m:meta->methods)if(m.name==member && m.signature==signature){
                if(match)throw bolt::library::ReflectionException("Ambiguous reflected overload.");
                match=m.call;
            }
            if(!match)throw bolt::library::ReflectionException("No public method matches the name, result type and argument types.");
            invoke_member(c,match,out,object,args);return;
        }
        bool found=false;
        for(const auto& f:meta->fields)if(f.name==member){
            found=true;
            if(op=="Set" && (f.flags&ReadOnlyField))throw bolt::library::InvalidOperationException("The reflected field is read-only.");
            if(f.type!=value)throw bolt::library::InvalidCastException("The requested field type does not match its declaration.");
            if(op=="Set")write_member(c,f,object,args[0]);
            else{auto v=read_member(c,owner,f,object);assign(value,out,v.data());}
        }
        if(!found)throw bolt::library::ReflectionException("Field not found: "+std::string(member));
    });
}
}
