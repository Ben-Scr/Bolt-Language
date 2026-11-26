#include "Lowering.hpp"
#include <bolt/Value.hpp>
#include <bolt/library/detail/UnicodeCase.hpp>

namespace bolt::ir::lowering {

static bool platform_console(const Class& c){
    if(!c.token.at.file.starts_with("<bolt-stdlib>"))return false;
    return c.name=="Bolt.Runtime.WindowsConsole"||c.name=="Bolt.Runtime.LinuxConsole"||c.name=="Bolt.Runtime.MacOSConsole";
}

static const Class* exception_root(const Program& program,const Class& type) {
    const auto* current=&type;
    while(current && !current->base.empty())current=program.find_class(current->base);
    return current && current->name=="System.Exception" &&
        current->token.at.file=="<bolt-stdlib>/System/Exceptions.bolt" ? current : nullptr;
}

Value Builder::invoke(const Class& c,const Method& m,Reg object,const std::vector<ExprPtr>& arguments,std::size_t start,bool dispatch,const std::vector<Reg>* prepared){
    // Interface default bodies are cloned onto concrete types. When such a
    // body runs on a native-layout struct, `self` is raw struct storage with
    // no hidden BrType* header. Resolve unqualified interface calls back to
    // the concrete struct implementation instead of passing raw storage to
    // br_unbox/br_dispatch. Real interface values are boxed and do not take
    // this path.
    if(c.is_interface&&object==self&&!owner.empty())
        if(const auto* receiver=lower.program.find_class(owner);receiver && (receiver->is_struct ||
                (lower.core() && !receiver->is_interface && !receiver->is_native)))
            if(const auto* implementation=method_by_id(*receiver,m.id);implementation&&concrete(*implementation))
                return invoke(*receiver,*implementation,object,arguments,start,!receiver->is_struct,prepared);
    if(lower.core() && c.name=="System.IStringable" && (m.name=="ToString" || m.name=="@convert.string") && object){
        auto output=temp("string");
        call(m.is_conversion?"bolt_convert_box":"bolt_format_box",Type::Void,{context,output,object},true,false);
        return {"string",output};
    }
    // A standalone executable sees every subtype. Direct calls are sufficient
    // when the receiver's method has no overriding implementation in the image.
    if(lower.core() && dispatch && m.is_virtual && !m.is_abstract && !c.is_interface && !c.is_native){
        dispatch=std::any_of(lower.program.classes.begin(),lower.program.classes.end(),[&](const Class& other){
            return other.name!=c.name && method_by_id(other,m.id) && derives_from(lower.program,other.name,c.name);
        });
    }
    if(lower.core() && m.is_virtual && m.name=="ToString" && m.parameters.empty() &&
       m.type=="string" && (dispatch || c.is_native) && exception_root(lower.program,c)){
        const auto output=temp("string"), actual=load(Type::Ptr,object);
        const auto done=block();
        if(dispatch)for(const auto& candidate:lower.program.classes){
            if(candidate.is_native || !candidate.generic_parameters.empty() || !exception_root(lower.program,candidate))continue;
            const auto* impl=&candidate;
            const Method* format=nullptr;
            while(impl && !impl->is_native && !format){
                format=method_by_id(*impl,m.id);
                if(!format)impl=lower.program.find_class(impl->base);
            }
            if(!format || !concrete(*format) || format->is_abstract)continue;
            const auto use=block(), next=block();
            branch(cmp("eq",actual,type(candidate.name)),use,next);current=use;
            call(method_symbol(*impl,*format),Type::Void,{context,output,object},true,false);
            jump(done);current=next;
        }
        const auto* strings=lower.program.find_class("Bolt.Runtime.StandaloneStrings");
        call(method_symbol(*strings,*find_method(*strings,"DescribeException")),Type::Void,{context,output,object},true,false);
        jump(done);current=done;return {"string",output};
    }
    if(lower.core() && c.name=="System.IO.BinaryCodec" && m.name=="Kind" &&
       c.token.at.file=="<bolt-stdlib>/System/IO/StandaloneReaders.bolt" && m.specialization_types.size()==1){
        const auto& t=m.specialization_types.front();
        const bool scalar=is_numeric(t)||t=="bool"||t=="char"||lower.program.find_enum(t);
        return {"int",integer(Type::I32,scalar?1:t=="string"?2:t=="byte[]"?3:0)};
    }
    if(!m.native_abi.empty()){
        if(auto reason=native_abi::direct_extern_requirement(lower.program,m);!reason.empty())throw Error(m.token.at,"native backend: "+reason,"BLT4001");
        std::vector<Reg> passed;
        if(prepared)passed=*prepared;
        else for(std::size_t n=0;n<m.parameters.size();++n){
            const auto& p=m.parameters[n];
            const auto* arg=start+n<arguments.size()?arguments[start+n].get():p.default_value.get();
            if(!arg)throw Error(p.token.at,"missing external function argument '"+p.name+"'","BLT2500");
            if(is_reference(p.type))passed.push_back(arg->lvalue?borrow(arg->type,referred(p.type),reference(*arg)):memory(converted(expression(*arg),referred(p.type))));
            else passed.push_back(converted(expression(*arg),p.type).value);
        }
        const auto symbol=m.native_name.empty()?m.name:m.native_name;
        if(lower.core() && symbol.starts_with("br_"))
            throw Error(m.token.at,"--no-runtime cannot import a Bolt runtime ABI symbol","BLT4001");
        auto result=call(symbol,m.type=="void"?Type::Void:lower.layout(m.type).ir,std::move(passed),false);
        for(auto& external:lower.module.externals)if(external.name==symbol){
            // Bolt's only signed integer narrower than 32 bits is short.
            std::vector<bool> signed_args;
            for(const auto& p:m.parameters)signed_args.push_back(p.type=="short");
            const bool signed_result=m.type=="short";
            if(external.foreign && (external.signed_arguments!=signed_args || external.signed_result!=signed_result))
                throw Error(m.token.at,"inconsistent C ABI declarations for "+symbol,"BLT4001");
            external.foreign=true;
            external.signed_arguments=std::move(signed_args);
            external.signed_result=signed_result;
            break;
        }
        return {m.type,result};
    }
    // Embedders may explicitly supply streams. Ordinary process execution runs
    // the platform Console Bolt bodies below, which call declared OS C symbols.
    // The bridge is used only for an explicitly injected host stream.
    if(platform_console(c)){
        if(m.name=="ReadLine"&&lower.options.custom_input){auto out=temp("string");call("br_read",Type::Void,{context,site(),out});return {"string",out};}
        if((m.name=="Write"||m.name=="WriteLine")&&lower.options.custom_output){
            auto text=prepared?prepared->at(0):expression(*arguments.at(start)).value;
            call("br_write",Type::Void,{context,site(),text,integer(Type::I32,m.name=="WriteLine")});return {"void",0};
        }
        if(m.name=="ClearDisplay"&&lower.options.custom_output)return {"void",0};
    }
    if(c.name == "Bolt.Runtime.MemoryOps" && native_abi::library_type_supported(c)) {
        const char* symbol = nullptr;
        if(m.name == "Copy") symbol = "br_memory_copy";
        else if(m.name == "Move") symbol = "br_memory_move";
        else if(m.name == "Set") symbol = "br_memory_set";
        else if(m.name == "Compare") symbol = "br_memory_compare";
        if(symbol || m.name == "Address") {
            std::vector<Reg> values;
            if(prepared) values = *prepared;
            else for(std::size_t i = 0; i < m.parameters.size(); ++i) {
                auto value = expression(*arguments.at(start + i));
                values.push_back(converted(std::move(value), m.parameters[i].type).value);
            }
            if(m.name == "Address") return {"ulong", cast(values.at(0), Type::I64, "ptrtoint")};
            std::vector<Reg> passed{context, site()};
            passed.insert(passed.end(), values.begin(), values.end());
            return {m.type, call(symbol, lower.result_type(m), std::move(passed))};
        }
    }
    if(c.is_native){
        std::vector<std::pair<std::string,Reg>> args;
        for(std::size_t n=0;n<m.parameters.size();++n){
            const auto& p=m.parameters[n];auto t=referred(p.type);Reg ptr=0;
            if(prepared){auto value=prepared->at(n);ptr=is_reference(p.type)||lower.layout(t).aggregate?value:memory({t,value});}
            else {
                Expr empty(Expr::Kind::Default,p.token);empty.type=t;
                const auto* arg=start+n<arguments.size()?arguments[start+n].get():p.default_value.get();
                if(!arg&&p.variadic)arg=&empty;
                if(!arg)throw Error(p.token.at,"missing native method argument '"+p.name+"'","BLT2500");
                if (!lower.core() && lower.options.optimize_loops && c.native_cpp == "bolt::platform::Failure" &&
                    p.type == "string" && arg->kind == K::Literal && arg->type == "string") {
                    // The failure bridge immediately copies these bytes into the
                    // exception. The caller never owns or mutates the literal.
                    ptr = alloc(sizeof(void*),false);
                    store(ptr,address(lower.text_literal(literal_value(*arg).as_string())));
                } else if(is_reference(p.type)&&arg->lvalue)ptr=borrow(arg->type,t,reference(*arg));
                else ptr=memory(snapshot(converted(expression(*arg),t)));
            }
            args.emplace_back(t,ptr);
        }
        if(native_abi::library_primitive_method(c,m)){
            if(c.native_cpp=="bolt::platform::ExecutionMemory"){
                if(m.name=="Context")return {m.type,context};
                if(m.name=="Descriptor")return {m.type,type(m.specialization_types.at(0))};
                std::vector<Reg> values;
                for(const auto& arg:args)values.push_back(load(Type::Ptr,arg.second));
                if(m.name=="RunEntry"){
                    auto result=lower.program.preprocessor.platform=="Windows"?Type::I32:Type::Ptr;
                    call(lower.thread_entry(),result,std::move(values),false,false);
                    return {"void",0};
                }
                const char* target=m.name=="Child"?"br_context_child":m.name=="Dispose"?"br_destroy":
                    m.name=="Activate"?"br_context_activate":m.name=="Registry"?"br_context_registry":
                    m.name=="Allocate"?"br_new":m.name=="Abandon"?"br_new_failed":
                    m.name=="Size"?"br_type_size":m.name=="Copy"?"br_copy":m.name=="Drop"?"br_drop":"";
                if(m.name=="OutOfMemory"){
                    if(lower.core())lower.core_call(*this,"OutOfMemory",Type::Void,{values[0],site()},false);
                    else call("br_fail",Type::Void,{values[0],site(),string("Out of memory.")},false);
                    return {"void",0};
                }
                if(m.name=="Copy" || m.name=="Allocate")values.insert(values.begin()+1,site());
                auto result=lower.result_type(m);
                return {m.type,lower.core()?lower.core_call(*this,target,result,std::move(values),false):
                    call(target,result,std::move(values),false)};
            }
            if(c.native_cpp=="bolt::platform::AtomicOps"){
                std::vector<Reg> operands{args[0].second};
                for(std::size_t n=1;n<args.size();++n)operands.push_back(load(Type::I64,args[n].second));
                return {"ulong",instruction(m.name=="Cas"?Op::AtomicCompareExchange:Op::AtomicAdd,Type::I64,std::move(operands))};
            }
            if(c.native_cpp=="bolt::platform::ThreadCallback"){
                if(m.name=="Entry")return {"void*",address(lower.thread_entry())};
                if(m.name=="Cancel"){lower.thread_call(*this,"Release",Type::Void,{load(Type::Ptr,args.at(0).second)});return {"void",0};}
            }
            if(c.native_cpp=="bolt::platform::NativeString"){
                if(m.name=="Bytes"){
                    if(lower.core() && owner!="Bolt.Runtime.StandaloneStrings"){
                        const auto bytes=lower.core_call(*this,"ExposeText",Type::Ptr,{context,site(),args[0].second,args[1].second},true);
                        return {"byte*",instruction(Op::Select,Type::Ptr,{cmp("ne",bytes,null()),bytes,string("")})};
                    }
                    return {"byte*",call("br_string_data",Type::Ptr,{args[0].second,args[1].second},false)};
                }
                auto out=temp("string");call("br_string",Type::Void,{context,site(),out,load(Type::Ptr,args[0].second),load(Type::I64,args[1].second)});return {"string",out};
            }
            if(c.native_cpp=="bolt::platform::Bits")return {"ulong",instruction(m.name=="Add64"?Op::Add:Op::Mul,Type::I64,{load(Type::I64,args[0].second),load(Type::I64,args[1].second)})};
            if(lower.core() && (m.name=="Equal" || m.name=="Hash")){
                const auto* value=lower.program.find_class(args[0].first);
                if(value && !value->is_native){
                    if(m.name=="Equal" && equality_contract(*value)){
                        std::vector<Value> inputs;
                        for(const auto& arg:args)inputs.push_back(from_memory(arg.first,arg.second));
                        return invoke_operator(*value,*equality_contract(*value),std::move(inputs));
                    }
                    if(m.name=="Hash"){
                        if(const auto* hash=hash_contract(*value))
                            return invoke(*value,*hash,args[0].second,{},0,true);
                    }
                    if(m.name=="Hash" && equality_contract(*value))return {"ulong",integer(Type::I64,0)};
                }
            }
            // Values.Equal uses language equality for scalar types. Leave the operands
            // in registers, including ordered IEEE equality for NaN and signed zero.
            if (m.name == "Equal") {
                if (lower.options.optimize_loops && args[0].first == "string")
                    return {"bool", string_equal(args[0].second,args[1].second)};
                const auto& element = lower.layout(args[0].first);
                if (native_abi::scalar_storage_kind(element.kind)) {
                    const auto lhs = load(element.ir, args[0].second);
                    const auto rhs = load(element.ir, args[1].second);
                    const auto pred = element.kind == native_abi::Float ? "oeq" : "eq";
                    return {"bool", boolean(cmp(pred, lhs, rhs))};
                }
            }
            auto t=type(args[0].first);
            if(m.name=="Hash")return {"ulong",call("br_value_hash",Type::I64,{context,site(),t,args[0].second})};
            if(m.name=="Equal")return {"bool",cast(call("br_compare",Type::I32,{context,site(),t,args[0].second,args[1].second,integer(Type::I32,0)}),Type::I8,"trunc")};
            auto out=temp("string");call("br_text",Type::Void,{context,site(),out,t,args[0].second});return {"string",out};
        }
        if(lower.core() && c.native_cpp=="bolt::platform::Failure") {
            static const std::map<std::string,std::string> types{
                {"Argument","ArgumentException"},{"Range","ArgumentOutOfRangeException"},
                {"Index","IndexOutOfRangeException"},{"MissingKey","KeyNotFoundException"},
                {"InvalidOperation","InvalidOperationException"},{"Overflow","OverflowException"},
                {"Format","FormatException"},{"IO","IOException"},{"DirectoryMissing","DirectoryNotFoundException"}};
            lower.core_call(*this,"Error",Type::Void,{context,site(),type("System."+types.at(m.name)),load(Type::Ptr,args.at(0).second)},true);
            return {"void",0};
        }
        if(lower.core() && c.token.at.file=="<bolt-stdlib>/System/Exceptions.bolt" && m.is_constructor) {
            store(object,type(c.name));
            lower.core_call(*this,"AssignText",Type::Void,{context,site(),offset(object,8),load(Type::Ptr,args.at(0).second)},true);
            return {c.name,object};
        }
        if(dispatch&&m.is_virtual&&!m.is_static&&!m.is_constructor){
            auto out=m.type!="void"&&lower.layout(m.type).aggregate?temp(m.type):0;
            auto fnptr=call("br_dispatch",Type::Ptr,{context,site(),object,dispatch_slot(m.id)});
            std::vector<Reg> passed{fnptr,context};if(out)passed.push_back(out);passed.push_back(object);
            for(std::size_t i=0;i<args.size();++i)passed.push_back(is_reference(m.parameters[i].type)||lower.layout(args[i].first).aggregate?args[i].second:load(lower.layout(args[i].first).ir,args[i].second));
            auto result=instruction(Op::CallIndirect,lower.result_type(m),std::move(passed));check();return {m.type,out?out:result};
        }
        return library_call(native_abi::library_method_key(c,m),m.is_constructor?c.name:m.type,object,args,m.is_constructor?object:0);
    }

    std::vector<Reg> passed{context};Reg output=0;
    if(m.type!="void"&&!m.is_constructor&&!is_reference(m.type)&&lower.layout(m.type).aggregate){output=temp(m.type);passed.push_back(output);}
    Reg dispatch_object=object;
    const auto receiver_index=passed.size();
    if(!m.is_static)passed.push_back(object);
    if(prepared)passed.insert(passed.end(),prepared->begin(),prepared->end());
    else for (std::size_t n=0; n<m.parameters.size(); ++n) {
        const auto& p=m.parameters[n];
        Expr empty(Expr::Kind::Default, p.token); empty.type=p.type;
        const Expr* value = start+n < arguments.size() ? arguments[start+n].get() : p.default_value.get();
        if (!value && p.variadic) value=&empty;
        if (!value) throw Error(p.token.at, "missing required constructor argument '"+p.name+"'", "BLT2500");
        const auto& e=*value;
        if (is_reference(p.type) && m.operator_symbol.empty())
            passed.push_back(borrow(e.type,referred(p.type),e.lvalue?reference(e):memory(expression(e))));
        else if(is_reference(p.type))
            passed.push_back(memory(snapshot(converted(expression(e),referred(p.type)))));
        else {
            auto value = converted(expression(e), p.type);
            const bool borrow = p.type == "string" && lower.options.optimize_loops &&
                n + 1 == m.parameters.size() && !is_reference(m.type) && lower.read_only.reads_only(m);
            const bool independent = lower.options.optimize_loops && e.type == p.type && fresh_handle(e);
            passed.push_back(borrow || independent ? value.value : snapshot(value).value);
        }
    }
    if(c.is_interface && !m.is_static)passed[receiver_index]=call("br_unbox",Type::Ptr,{context,site(),object});
    auto ty=lower.result_type(m);Reg result=0;
    // A standalone image knows the concrete implementations. Dispatch using the
    // descriptor in the receiver so reflected calls preserve ordinary virtual
    // overrides, including a concrete grandchild inheriting its parent's method.
    if(lower.core() && dispatch && m.is_virtual && !m.is_constructor &&
       !c.is_native && !c.is_struct) {
        const auto actual=c.is_interface ? lower.core_call(*this,"ReceiverType",Type::Ptr,{dispatch_object},false) :
                                           load(Type::Ptr,dispatch_object);
        const auto slot=ty==Type::Void?0:alloc(width(ty));
        const auto done=block();
        for(const auto& candidate:lower.program.classes){
            if(!emitted_class(candidate) || candidate.is_native || candidate.is_interface || candidate.is_abstract ||
               !candidate.generic_parameters.empty() || !candidate.pack_parameter.empty() ||
               !derives_from(lower.program,candidate.name,c.name))continue;
            const Class* owner=&candidate;
            const Method* impl=nullptr;
            while(owner && !(impl=method_by_id(*owner,m.id)))owner=lower.program.find_class(owner->base);
            if(!impl || !concrete(*impl) || impl->is_abstract)continue;
            const auto target=block(),next=block();
            branch(cmp("eq",actual,type(candidate.name)),target,next);current=target;
            const auto value=call(method_symbol(*owner,*impl),ty,passed,true,false);
            if(slot)store(slot,value);
            jump(done);current=next;
        }
        call("br_fail",Type::Void,{context,site(),string("Unbound virtual method.")},false);
        jump(failure);current=done;
        return {m.type,output?output:slot?load(ty,slot):0};
    }
    if(dispatch && m.is_virtual && !m.is_constructor && !c.is_struct) {
        const bool specialize=lower.options.optimize_loops && lower.module.instrument_budget &&
            !c.is_interface && !c.is_abstract && !m.is_abstract && m.body && m.analyzed;
        Label indirect=0, done=0;
        Reg saved=0;
        if(specialize) {
            const auto inspect=block(), direct=block();
            indirect=block(); done=block();
            if(ty!=Type::Void) saved=alloc(width(ty),false);
            branch(cmp("ne",dispatch_object,null()),inspect,indirect);
            current=inspect;
            branch(cmp("eq",load(Type::Ptr,dispatch_object),type(c.name)),direct,indirect);
            current=direct;
            const auto value=call(method_symbol(c,m),ty,passed,true,false);
            if(saved) store(saved,value);
            jump(done);
            current=indirect;
        }
        auto fnptr=call("br_dispatch",Type::Ptr,{context,site(),dispatch_object,dispatch_slot(m.id)});
        passed.insert(passed.begin(),fnptr);
        result=instruction(Op::CallIndirect,ty,std::move(passed));
        check();
        if(specialize) {
            if(saved) store(saved,result);
            jump(done);
            current=done;
            if(saved) result=load(ty,saved);
        }
    }
    else result=call(method_symbol(c,m),ty,std::move(passed),true,false);
    return {m.type,output?output:result};
}
Value Builder::invoke_operator(const Class& c,const Method& m,std::vector<Value> operands){
    if(c.is_native){std::vector<std::pair<std::string,Reg>> args;for(std::size_t i=0;i<operands.size();++i){auto t=referred(m.parameters[i].type);args.emplace_back(t,memory(snapshot(converted(std::move(operands[i]),t))));}return library_call(native_abi::library_method_key(c,m),m.type,0,args);}

    std::vector<Reg> args{context};Reg output=0;
    if(lower.layout(m.type).aggregate){output=temp(m.type);args.push_back(output);}
    for(std::size_t i=0;i<operands.size();++i){auto v=snapshot(converted(std::move(operands[i]),referred(m.parameters[i].type)));args.push_back(is_reference(m.parameters[i].type)?memory(std::move(v)):v.value);}
    auto result=call(method_symbol(c,m),lower.result_type(m),std::move(args),true,false);
    return {m.type,output?output:result};
}

Value Builder::library_call(const std::string& key,const std::string& result,Reg object,const std::vector<std::pair<std::string,Reg>>& values,Reg output){
    if(lower.core() && (key=="bolt::library::Exception::Message()->string" ||
                        key=="bolt::library::Exception::StackTrace()->string")) {
        if(!output)output=temp("string");
        auto slot=offset(object,key.find("StackTrace")!=key.npos?24:8);
        lower.core_call(*this,"AssignText",Type::Void,{context,site(),output,load(Type::Ptr,slot)},true);
        return {result,output};
    }
    if(lower.core() && key=="bolt::library::Exception::SourceFile()->string"){
        if(!output)output=temp("string");
        lower.core_call(*this,"SourceFile",Type::Void,{context,object,output},true);
        return {result,output};
    }
    if(lower.core() && (key=="bolt::library::Exception::SourceLine()->int" || key=="bolt::library::Exception::SourceColumn()->int")){
        auto column=integer(Type::I8,key.find("SourceColumn")!=key.npos);
        return {result,lower.core_call(*this,"SourceNumber",Type::I32,{object,column},false)};
    }
    const auto id=native_abi::library_operation(key);
    if(id<0)throw Error(at,"operation is not available in the shared native runtime: "+key,"BLT4001");
    Reg pointers=values.empty()?null():alloc(values.size()*8,false),types=values.empty()?null():alloc(values.size()*8,false);
    for(std::size_t i=0;i<values.size();++i){store(offset(pointers,i*8),values[i].second);store(offset(types,i*8),type(values[i].first));}
    if(!output)output=result=="void"?null():temp(result);
#if defined(_M_X64)
    if(lower.options.standalone_executable)
        call("br_static_library_"+std::to_string(id),Type::Void,{context,site(),result=="void"?null():type(result),output,object?object:null(),types,pointers,integer(Type::I64,values.size())});
    else
#endif
        call("br_library",Type::Void,{context,site(),integer(Type::I32,static_cast<std::uint64_t>(id)),result=="void"?null():type(result),output,object?object:null(),types,pointers,integer(Type::I64,values.size())});
    return result=="void"?Value{"void",0}:from_memory(result,output);
}
Value Builder::string_call(const std::string& method,const std::string& result,std::vector<Value> values,Reg output){
    if(lower.core()) {
        const auto* cls=lower.program.find_class("Bolt.Runtime.StandaloneStrings");
        if(cls && (method=="ToUpper" || method=="ToLower")) {
            const bool upper=method=="ToUpper";
            const std::string name=upper?"bolt_unicode_upper":"bolt_unicode_lower";
            auto emit=[&](const auto& table) {
                if(std::none_of(lower.module.globals.begin(),lower.module.globals.end(),[&](const Global& g){return g.name==name;})){
                    Global data{name,{},false,4,{}};
                    auto append=[&](std::uint32_t word){
                        for(unsigned i=0;i<4;++i)data.bytes.push_back(static_cast<std::uint8_t>(word>>(8*i)));
                    };
                    for(const auto& entry:table){append(entry.from);append(entry.count);for(auto ch:entry.to)append(ch);}
                    lower.module.globals.push_back(std::move(data));
                }
                if(!output)output=temp(result);
                call(method_symbol(*cls,*find_method(*cls,"Case")),Type::Void,
                     {context,output,values[0].value,address(name),integer(Type::I32,table.size())},true,false);
                return Value{result,output};
            };
            if(upper)return emit(bolt::library::unicode_case::Upper);
            return emit(bolt::library::unicode_case::Lower);
        }
        if(cls) for(const auto& m:cls->methods) {
            if(m.name!=method || m.parameters.size()!=values.size() || m.type!=result) continue;
            bool matches=true;
            for(std::size_t i=0;i<values.size();++i)matches &= referred(m.parameters[i].type)==values[i].type;
            if(!matches)continue;
            std::vector<Reg> args{context};
            if(lower.layout(result).aggregate){if(!output)output=temp(result);args.push_back(output);}
            for(auto& v:values)args.push_back(v.value);
            auto reg=call(method_symbol(*cls,m),lower.result_type(m),std::move(args),true,false);
            return {result,output?output:reg};
        }
    }
    // Callers snapshot in evaluation order before a later argument can mutate
    // the receiver. ByteLength borrows its sole argument for this synchronous read.
    std::string key="bolt::library::Strings::"+method+"(";std::vector<std::pair<std::string,Reg>> args;
    for(auto& value:values){if(!args.empty())key+=",";const auto t=value.type;key+=t;auto pointer=memory(std::move(value));args.emplace_back(t,pointer);}
    return library_call(key+")->"+result,result,0,args,output);
}

Reg Builder::buffer_data(Reg header) {
    // StandaloneCore's ReserveBuffer and rank-one ArrayInto put the elements
    // immediately after the sequence prefix and its single dimension.
    if (lower.core()) return offset(header,sizeof(BrSequenceHeader)+sizeof(std::uint64_t));
    return load_sequence_data(header);
}

// The input has already been evaluated, including any mutation of the receiver.
// Snapshotting the scalar register before a growth call also handles Add(buffer[i]).
void Builder::add_plain(const Expr& e, Reg slot, Value item) {
    const auto slow = block(), check_capacity = block(), append = block(), done = block();
    const auto header = load(Type::Ptr, slot);
    const auto observers = load(Type::I64, offset(context, offsetof(BrContext, active_ranges)),"execution context");
    const auto monitored = cmp("ne", observers, integer(Type::I64, 0));
    const auto empty = cmp("eq", header, null());
    branch(instruction(Op::Or, Type::I32, {monitored, empty}), slow, check_capacity);

    current = check_capacity;
    const auto count_address = offset(header, offsetof(BrSequenceHeader, count));
    const auto count = load_buffer_size(count_address);
    const auto capacity = load_buffer_size(offset(header, offsetof(BrSequenceHeader, capacity)));
    branch(cmp("ult", count, capacity), append, slow);

    current = append;
    const auto data = buffer_data(header);
    // reserve() has already checked that capacity * element_size fits size_t.
    const auto bytes = instruction(Op::Mul, Type::I64,
        {count, integer(Type::I64, lower.layout(item.type).size)});
    const auto dest = instruction(Op::Offset, Type::Ptr, {data, bytes});
    if (lower.layout(item.type).plain_struct) copy_value(item.type,dest,item.value);
    else store(dest, item.value,"sequence element");
    store(count_address, instruction(Op::Add, Type::I64, {count, integer(Type::I64, 1)}),"sequence header");
    jump(done);

    current = slow;
    call("br_collection", Type::Void, {context, site(), type(e.owner), slot,
        string("Add"), null(), memory(item), null()});
    jump(done);
    current = done;
}

void Builder::move_within_storage(Reg dest, Reg source, Reg bytes, std::size_t stride) {
    call("br_memory_move",Type::Void,{context,site(),dest,source,bytes},false);
    auto& move = fn.blocks[current].code.back();
    // StandaloneCore maps the same operation to its source-backed MemoryOps.Move.
    if (move.op == Op::Call && (move.text == "br_memory_move" || lower.core()))
        move.storage_move_stride = static_cast<std::uint8_t>(stride);
}

// For scalar elements the whole suffix can be shifted in place. Take the
// payload by value before reading the handle: an argument can change the
// buffer or refer to an element that the shift is about to overwrite.
void Builder::edit_scalar_buffer(const Expr& e, Reg object, const std::string& element, const Method* list_method) {
    const bool insert = e.args.size()==3;
    const auto index = converted(expression(*e.args[1]),"int");
    Value item;
    if (insert) item = converted(expression(*e.args[2]),element);
    at = e.token.at;
    const auto slot = list_method ? lower.field(*this,e.owner,"storage",object) : object;
    const auto slow = block(), present = block(), shift = block(), done = block();
    // The general List method is needed for growth, invalid indices or active
    // ranges. Keep its body out of loops that normally edit in place.
    fn.blocks[slow].cold = list_method != nullptr;
    const auto header = load(Type::Ptr,slot);
    const auto observers = loop_edit_ranges.first == &e ? loop_edit_ranges.second :
        load(Type::I64,offset(context,offsetof(BrContext,active_ranges)),"execution context");
    const auto monitored = cmp("ne",observers,integer(Type::I64,0));
    branch(instruction(Op::Or,Type::I32,{monitored,cmp("eq",header,null())}),slow,present);

    current = present;
    const auto count_slot = offset(header,offsetof(BrSequenceHeader,count));
    const auto count = load_buffer_size(count_slot);
    const auto pos = cast(index.value,Type::I64,"zext");
    auto valid = cmp(insert ? "ule" : "ult",pos,count);
    if (insert) {
        const auto capacity = load_buffer_size(offset(header,offsetof(BrSequenceHeader,capacity)));
        valid = instruction(Op::And,Type::I32,{valid,cmp("ult",count,capacity)});
    }
    branch(valid,shift,slow);
    current = shift;
    const auto size = lower.layout(element).size;
    const auto width = integer(Type::I64,size), one = integer(Type::I64,1);
    const auto data = buffer_data(header);
    const auto first = instruction(Op::Offset,Type::Ptr,{data,instruction(Op::Mul,Type::I64,{pos,width})});
    const auto next = offset(first,size);
    const auto new_count = instruction(insert ? Op::Add : Op::Sub,Type::I64,{count,one});
    const auto tail = instruction(Op::Sub,Type::I64,{insert ? count : new_count,pos});
    const auto bytes = instruction(Op::Mul,Type::I64,{tail,width});
    move_within_storage(insert ? next : first,insert ? first : next,bytes,size);
    if (insert) store(first,item.value,"sequence element");
    else {
        const auto end = instruction(Op::Offset,Type::Ptr,{data,instruction(Op::Mul,Type::I64,{new_count,width})});
        instruction(Op::Zero,Type::Void,{end},size);
    }
    store(count_slot,new_count,"sequence header");
    if (list_method) {
        const auto revision=lower.field(*this,e.owner,"revision",object);
        store(revision,instruction(Op::Add,Type::I64,{load(Type::I64,revision),one}));
    }
    jump(done);
    current = slow;
    if (list_method) {
        std::vector<Reg> args{index.value};
        if (insert) args.push_back(item.value);
        invoke(*lower.program.find_class(e.owner),*list_method,object,e.args,1,false,&args);
    } else {
        call("br_collection",Type::Void,{context,site(),type(e.owner),slot,string(e.text),null(),
            memory(index),insert ? memory(item) : null()});
    }
    jump(done);
    current = done;
}

// Shrinking a buffer of trivial elements ends their logical lifetime. Active
// iterators, empty handles and invalid lengths keep full runtime semantics.
void Builder::truncate_plain(const Expr& e, Reg slot, Value retained) {
    const auto slow = block(), present = block(), apply = block(), done = block();
    const auto header = load(Type::Ptr, slot);
    const auto observers = load(Type::I64, offset(context, offsetof(BrContext, active_ranges)),"execution context");
    const auto observed = cmp("ne", observers, integer(Type::I64, 0));
    branch(instruction(Op::Or, Type::I32, {observed, cmp("eq", header, null())}), slow, present);

    current = present;
    const auto count = offset(header, offsetof(BrSequenceHeader, count));
    // Both valid lengths fit int. An unsigned comparison also rejects
    // negatives and exposes count-1 <= count to the integer optimizer.
    const auto available = cast(load_buffer_size(count),Type::I32,"trunc");
    branch(cmp("ule",retained.value,available),apply,slow);
    current = apply;
    store(count,cast(retained.value,Type::I64,"zext"),"sequence header");
    jump(done);
    current = slow;
    const auto argument = e.text == "Clear" ? null() : memory(retained);
    call("br_collection",Type::Void,{context,site(),type(e.owner),slot,string(e.text),null(),argument,null()});
    jump(done); current = done;
}
// Append plain array elements directly into spare capacity. A nonempty valid
// source range is live, while the destination starts at the first unused slot.
// The runtime handles growth, ownership, observed ranges and diagnostics.
void Builder::append_plain_range(const Expr& e, Reg slot, Reg array, Reg index, Reg amount, std::size_t stride) {
    const auto inspect = block(), copy = block(), slow = block(), done = block();
    const auto dst = load(Type::Ptr,slot), src = load(Type::Ptr,array);
    const auto observed = load(Type::I64,offset(context,offsetof(BrContext,active_ranges)),"execution context");
    const auto missing = instruction(Op::Or,Type::I32,{cmp("eq",dst,null()),cmp("eq",src,null())});
    branch(instruction(Op::Or,Type::I32,{missing,cmp("ne",observed,integer(Type::I64,0))}),slow,inspect);
    current = inspect;
    const auto offset64 = cast(index,Type::I64,"zext"), n = cast(amount,Type::I64,"zext");
    const auto end = instruction(Op::Add,Type::I64,{offset64,n});
    const auto count_slot = offset(dst,offsetof(BrSequenceHeader,count));
    const auto count = load_buffer_size(count_slot);
    const auto new_count = instruction(Op::Add,Type::I64,{count,n});
    const auto capacity = load_buffer_size(offset(dst,offsetof(BrSequenceHeader,capacity)));
    const auto length = load(Type::I64,offset(src,offsetof(BrSequenceHeader,count)),"sequence header");
    const auto fits = instruction(Op::And,Type::I32,{cmp("ule",end,length),cmp("ule",new_count,capacity)});
    const auto positive_range = instruction(Op::And,Type::I32,{cmp("sge",index,integer(Type::I32,0)),
                                                              cmp("sgt",amount,integer(Type::I32,0))});
    branch(instruction(Op::And,Type::I32,{fits,positive_range}),copy,slow);
    current = copy;
    const auto size = integer(Type::I64,stride);
    const auto tail = instruction(Op::Offset,Type::Ptr,{load_sequence_data(dst),instruction(Op::Mul,Type::I64,{count,size})});
    const auto from = instruction(Op::Offset,Type::Ptr,{load_sequence_data(src),instruction(Op::Mul,Type::I64,{offset64,size})});
    call("br_memory_move",Type::Void,{context,site(),tail,from,instruction(Op::Mul,Type::I64,{n,size})});
    store(count_slot,new_count,"sequence header"); jump(done);
    current = slow;
    call("br_buffer_append",Type::Void,{context,site(),type(e.owner),slot,array,index,amount});
    jump(done); current = done;
}

Value Builder::call_expr(const Expr& e){
    at=e.token.at;const auto& target=*e.args[0];
    if(e.binding==B::Cast){auto v=expression(*e.args[1]);at=e.token.at;if(e.type=="string"){auto p=temp("string");call("br_text",Type::Void,{context,site(),p,type(v.type),memory(v)});return {e.type,p};}return converted(std::move(v),e.type);}
    if(e.binding==B::Construct){auto p=temp(e.type);auto* c=lower.program.find_class(e.type);if(c&&!e.resolved.empty()){clear_value(e.type,p,"br_reset");invoke(*c,*method_by_id(*c,e.resolved),p,e.args,1,false);}else initialize(e.type,p);return {e.type,p};}
    if(e.binding==B::Builtin){
        if(e.owner=="String"){
            if (lower.options.optimize_loops && !lower.module.instrument_budget && !e.is_static &&
                (e.text=="StartsWith" || e.text=="EndsWith") && e.args.size()==2 &&
                e.args[1]->kind==K::Literal && e.args[1]->type=="string") {
                const auto pattern=literal_value(*e.args[1]).as_string();
                if (pattern.size()<=16) return {"bool",literal_affix(receiver(target),pattern,e.text=="EndsWith")};
            }
            std::vector<Value> values;
            std::size_t last_effect = 0;
            for (std::size_t i = 1; i < e.args.size(); ++i)
                if (!cannot_change_string(*e.args[i])) last_effect = i;
            if (!e.is_static) {
                Value input{"string", receiver(target)};
                values.push_back(last_effect ? snapshot(input) : input);
            }
            for (std::size_t i = 1; i < e.args.size(); ++i) {
                auto arg = converted(expression(*e.args[i]), e.type_args.at(i - 1));
                const bool readonly = arg.type == "string" || arg.type == "byte[]" || arg.type == "string[]";
                values.push_back(readonly && i >= last_effect ? arg : snapshot(arg));
            }
            at=e.token.at;
            if (lower.options.optimize_loops && e.text == "Equals" && values.size() == 2 &&
                values[0].type == "string" && values[1].type == "string")
                return {"bool",string_equal(values[0].value,values[1].value)};
            return string_call(e.text,e.type,std::move(values));
        }
        if(e.owner=="Array"&&e.text=="GetLength"){
            auto object=receiver(target);auto dimension=converted(expression(*e.args[1]),"long");at=e.token.at;
            auto array_type=target.arrow?element_type(target.args[0]->type):target.args[0]->type;
            if(array_rank(array_type)==1) {
                guard(cmp("ne",dimension.value,integer(Type::I64,0)),"index out of range");
                return {"int",sequence_size(object,offsetof(BrSequenceHeader,count))};
            }
            return {"int",call("br_array_length",Type::I32,{context,site(),type(array_type),object,dimension.value})};
        }
        if(e.owner=="Arena"){
            if(e.text=="Fixed"||e.text=="Growable"){
                auto initial=converted(expression(*e.args[1]),"ulong");auto maximum=initial;
                if(e.text=="Growable")maximum=converted(expression(*e.args[2]),"ulong");
                at=e.token.at;auto p=temp("Arena");call("br_arena_create",Type::Void,{context,site(),p,initial.value,maximum.value,integer(Type::I32,e.text=="Growable"?1:0)});return {"Arena",p};
            }
            auto p=receiver(target);at=e.token.at;call("br_arena_action",Type::Void,{context,site(),p,integer(Type::I32,e.text=="Dispose"?1:0)});return {"void",0};
        }
        if(e.owner=="Intrinsic"&&e.text=="Reinterpret"){
            auto v=expression(*e.args[1]);auto to=lower.layout(e.type).ir;
            return {e.type,cast(v.value,to,"bitcast")};
        }
        if(e.owner=="Console"){
            if(e.text=="ReadLine"){auto p=temp("string");call("br_read",Type::Void,{context,site(),p});return {"string",p};}
            Value v{"string",temp("string")};if(e.args.size()>1){auto a=expression(*e.args[1]);at=e.token.at;if(a.type=="string")v=a;else call("br_text",Type::Void,{context,site(),v.value,type(a.type),memory(a)});}
            call("br_write",Type::Void,{context,site(),v.value,integer(Type::I32,e.text=="WriteLine"?1:0)});return {"void",0};
        }
        if(e.owner=="Runtime"){auto v=expression(*e.args[1]);at=e.token.at;int mode=e.text=="Execute"?0:e.text=="ExecuteProgram"?1:e.text=="ExecuteFile"?2:e.text=="ExecuteJit"?3:e.text=="ExecuteProgramJit"?4:5;return {"int",call("br_runtime",Type::I32,{context,site(),integer(Type::I32,static_cast<std::uint64_t>(mode)),v.value})};}
        if(e.owner=="Type"){auto p=temp("string");call("br_string",Type::Void,{context,site(),p,string(e.type_args[0]),integer(Type::I64,e.type_args[0].size())});return {"string",p};}
        throw Error(e.token.at,"unsupported builtin call: "+e.owner+"."+e.text);
    }
    if(e.binding==B::Collection){
        auto object=receiver(target);auto types=generic_args(e.owner);std::vector<std::string> params;
        if (consumed_snapshots.contains(&e)) return take_snapshot(e, object);
        if (lower.options.optimize_loops && !lower.module.instrument_budget &&
            collection_kind(e.owner) == "List" && (e.text == "Insert" || e.text == "RemoveAt") &&
            native_abi::scalar_storage_kind(lower.layout(types[0]).kind)) {
            edit_scalar_buffer(e,object,types[0]);
            return {"void",0};
        }
        if(e.text=="Add" && collection_kind(e.owner)=="List") {
            const auto& element=lower.layout(types[0]);
            if (native_abi::scalar_storage_kind(element.kind) ||
                (lower.options.optimize_loops && !lower.module.instrument_budget && element.plain_struct && element.size<=64)) {
                auto value = converted(expression(*e.args[1]), types[0]);
                // Growth may invalidate an argument taken from this buffer.
                if (element.plain_struct) value = snapshot(value);
                at = e.token.at;
                add_plain(e, object, value);
                return {"void", 0};
            }
        }
        if (lower.options.optimize_loops && !lower.module.instrument_budget && collection_kind(e.owner) == "List" &&
            (e.text == "Clear" || e.text == "Truncate") &&
            (native_abi::scalar_storage_kind(lower.layout(types[0]).kind) || lower.layout(types[0]).plain_struct)) {
            const auto retained = e.text == "Clear" ? Value{"int",integer(Type::I32,0)}
                                                    : converted(expression(*e.args[1]),"int");
            at = e.token.at;
            truncate_plain(e,object,retained);
            return {"void",0};
        }
        // Two direct fields of this receiver have the same owning arena. With
        // no live ranges, the runtime Swap exchanges only the sequence handles.
        const auto instance_field = [](const Expr& arg) {
            return arg.kind == K::Name && arg.binding == B::Field && !arg.is_static;
        };
        if (e.text == "Swap" && collection_kind(e.owner) == "List" &&
            !lower.module.instrument_budget && target.kind == K::Member &&
            target.args.size() == 1 && e.args.size() == 2 &&
            instance_field(*target.args[0]) && instance_field(*e.args[1])) {
            const auto other = reference(*e.args[1]);
            at = e.token.at;
            const auto fast = block(), slow = block(), done = block();
            const auto observers = load(Type::I64, offset(context, offsetof(BrContext, active_ranges)), "execution context");
            branch(cmp("eq", observers, integer(Type::I64, 0)), fast, slow);
            current = fast;
            const auto left = load(Type::Ptr, object);
            const auto right = load(Type::Ptr, other);
            store(object, right);
            store(other, left);
            jump(done);
            current = slow;
            call("br_collection", Type::Void, {context, site(), type(e.owner), object, string(e.text), null(), other, null()});
            jump(done);
            current = done;
            return {"void", 0};
        }
        if(e.text=="AppendBuffer") {
            const auto start=converted(expression(*e.args[1]),"int");
            const auto count=converted(expression(*e.args[2]),"int");
            const auto input=expression(*e.args[3]);
            at=e.token.at;
            call("br_buffer_append",Type::Void,{context,site(),type(e.owner),object,input.value,start.value,count.value});
            return {"void",0};
        }
        if(e.text=="AppendRange") {
            auto array=expression(*e.args[1]);auto offset=converted(expression(*e.args[2]),"int");auto count=converted(expression(*e.args[3]),"int");
            at=e.token.at;
            const auto& element = lower.layout(types[0]);
            if (lower.options.optimize_loops && !lower.module.instrument_budget &&
                (native_abi::scalar_storage_kind(element.kind) || element.plain_struct))
                append_plain_range(e,object,array.value,offset.value,count.value,element.size);
            else call("br_buffer_append",Type::Void,{context,site(),type(e.owner),object,array.value,offset.value,count.value});
            return {"void",0};
        }
        if(e.text=="Swap")params={e.owner+"&"};else if(e.text=="Insert")params={"int",types[0]};else if(e.text=="RemoveAt"||e.text=="Reserve"||e.text=="Truncate")params={"int"};else if(e.text=="TryDequeue")params={types[0]+"&"};else if(e.text=="TryGetValue")params={types[0],types[1]+"&"};else if(collection_kind(e.owner)=="Dictionary"&&(e.text=="Add"||e.text=="Set"))params={types[0],types[1]};else if(e.args.size()>1)params={types[0]};
        std::vector<Reg> args;for(std::size_t n=0;n<params.size();++n){if(is_reference(params[n]))args.push_back(reference(*e.args[n+1]));else args.push_back(memory(snapshot(converted(expression(*e.args[n+1]),params[n]))));}while(args.size()<2)args.push_back(null());at=e.token.at;auto p=e.type=="void"?null():temp(e.type);call("br_collection",Type::Void,{context,site(),type(e.owner),object,string(e.text),p,args[0],args[1]});return e.type=="void"?Value{"void",0}:from_memory(e.type,p);
    }
    if(e.owner=="Bolt.Runtime.ThreadCallback" && !e.callback_method.empty()) {
        auto value=snapshot(expression(*e.args[1]));
        const auto& owner=*lower.program.find_class(e.callback_owner);
        const auto& method=*method_by_id(owner,e.callback_method);
        const auto types=alloc(8,false), values=alloc(8,false);
        store(types,type(value.type));store(values,memory(value));at=e.token.at;
        return {"void*",lower.thread_call(*this,"Prepare",Type::Ptr,{context,null(),address(lower.job_adapter(owner,method)),
            types,values,integer(Type::I64,1)},true)};
    }
    const auto& c=*lower.program.find_class(e.owner);const auto& m=*method_by_id(c,e.resolved);Reg object=0;if(!m.is_static)object=target.kind==K::Member?receiver(target):self;
    if (object && lower.options.optimize_loops && !lower.module.instrument_budget &&
        (m.name=="Insert" || m.name=="RemoveAt") && generic_base(c.name)=="System.Collections.Generic.List" &&
        c.token.at.file=="<bolt-stdlib>/System/Collections/Generic/List.bolt") {
        const auto element=generic_args(c.name).at(0);
        if (native_abi::scalar_storage_kind(lower.layout(element).kind)) {
            edit_scalar_buffer(e,object,element,&m);
            return {"void",0};
        }
    }
    if (memory_region && (memory_region->fill == &e || memory_region->copy == &e ||
                          memory_region->compare == &e || memory_region->move == &e)) {
        std::vector<Reg> values;
        for (std::size_t n = 0; n < m.parameters.size(); ++n)
            values.push_back(converted(expression(*e.args[n + 1]), m.parameters[n].type).value);
        if (memory_region->fill == &e) memory_region->filled = values;
        if (memory_region->copy == &e) memory_region->copied = values;
        if (memory_region->compare == &e) narrow_comparison(values);
        if (memory_region->fill && (memory_region->copy == &e || memory_region->move == &e)) {
            transfer_filled(e,c,m,values,memory_region->move == &e);
            return {"void",0};
        }
        return invoke(c, m, object, e.args, 1, false, &values);
    }
    auto result=invoke(c,m,object,e.args,1);result.type=e.type;return result;
}

} // namespace bolt::ir::lowering
