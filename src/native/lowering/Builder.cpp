#include "Lowering.hpp"
#include <algorithm>

namespace bolt::ir::lowering {

Reg Builder::instruction(Op op,Type ty,std::vector<Reg> args,std::uint64_t immediate,std::string text) {
    if ((op==Op::Load || op==Op::Store) && text.empty() && !element_tags.empty()) {
        if (auto it=element_tags.find(args.front());it!=element_tags.end()) text=it->second.storage;
    }
    Instruction i;i.op=op;i.args=std::move(args);i.immediate=immediate;i.text=std::move(text);if(ty!=Type::Void)i.result=reg(ty);auto result=i.result;emit(std::move(i));return result;
}

Reg Builder::alloc(std::size_t bytes, bool zero) {
    const auto slot = reg(Type::Ptr);
    Instruction reserve;
    reserve.op = Op::Alloca;
    reserve.result = slot;
    reserve.immediate = std::max<std::size_t>(1,bytes);
    reserve.at = at;
    prologue.push_back(reserve);
    if (zero) {
        Instruction clear;
        clear.op = Op::Zero;
        clear.args = {slot};
        clear.immediate = reserve.immediate;
        clear.at = at;
        prologue.push_back(clear);
    }
    return slot;
}

void Builder::branch(Reg value, Label yes, Label no, std::size_t reduction_element_bytes) {
    Instruction i;
    i.op = Op::Branch;
    i.args = {value};
    i.yes = yes;
    i.no = no;
    i.immediate = reduction_element_bytes;
    emit(std::move(i));
}

Builder::Builder(Lower& l,std::string name,Type result,std::vector<Type> params,Location loc):lower(l),at(std::move(loc)){
    fn.name=std::move(name);fn.result=result;fn.parameters=std::move(params);fn.at=at;for(auto p:fn.parameters)fn.registers.push_back(p);block();exit=block();error=block();failure=error;
}
Reg Builder::temp(const std::string& t, bool owned) {
    const auto& layout = lower.layout(t);
    const auto slot = alloc(layout.size);
    if (owned && layout.aggregate) {
        Reg live = 0;
        if ((layout.kind == native_abi::Class || layout.kind == native_abi::Struct) &&
            !layout.plain_struct && !layout.padded_field_bytes && layout.class_headers.empty()) {
            // The shared exit also sees slots from untaken branches. Arm cleanup
            // before initialization so a partially constructed value still unwinds.
            live = alloc(1);
            store(live, integer(Type::I8, 1));
        }
        cleanup.push_back({t, slot, live});
    }
    return slot;
}
Reg Builder::project_symbol(const std::string& key){
    return call("br_project_lookup",Type::Ptr,{context,string(key)},false);
}
Reg Builder::dispatch_slot(const std::string& id){
    const auto index=lower.dispatch_slot(id);
    if(lower.program.library_mode&&!lower.options.source_core)return load(Type::I32,project_symbol("virtual:"+id));
    return integer(Type::I32,index);
}
Reg Builder::type(const std::string& t){
    const auto& layout=lower.layout(t);
    auto r=lower.program.library_mode&&!lower.options.source_core?project_symbol("type:"+layout.name):load(Type::Ptr,address(layout.symbol));
    if(lower.core() || primitive_formatter(t).owner)core_types.emplace(r,t);
    return r;
}
Reg Builder::string(const std::string& s){
    auto r=address(lower.literal(s));if(lower.core())core_strings.emplace(r,s);return r;
}
Reg Builder::site(){
    auto& symbol = lower.site_symbols[at.file][{at.line,at.column,at.length}];
    if (!symbol.empty()) return address(symbol);
    const auto line=lower.source_line(at.file,at.line);
    auto header=std::to_string(at.line)+":"+std::to_string(at.column)+":"+std::to_string(at.length)+":"+std::to_string(at.file.size())+":"+at.file;
    auto* edits=lower.options.edit_data;
    if(edits && edits->sites.contains(at.file) && edits->sites.at(at.file).contains(at.line)) {
        symbol="b_edit_site_"+std::to_string(lower.module.globals.size());
        header+=line;
        std::vector<std::uint8_t> bytes(header.begin(),header.end());
        bytes.resize(bytes.size()+65);
        edits->sites.at(at.file).at(at.line).push_back({symbol,at.column,at.length,bytes.size()});
        lower.module.globals.push_back({symbol,std::move(bytes),false,1,{}});
    } else {header+=line;symbol=lower.literal(header);}
    return address(symbol);
}
Reg Builder::call(const std::string& name,Type result,std::vector<Reg> args,bool inspect,bool ext){
    if(ext && !kernel_body && name=="br_text") {
        auto t=core_types.find(args[3]);
        if(t!=core_types.end()){
            const auto formatter=primitive_formatter(t->second);
            const auto* cls=formatter.owner?lower.program.find_class(formatter.owner):nullptr;
            const auto* method=cls?find_method(*cls,formatter.method):nullptr;
            if(method && method->analyzed) {
                auto value=converted(from_memory(t->second,args[4]),method->parameters.front().type);
                return call(method_symbol(*cls,*method),Type::Void,{args[0],args[2],value.value},inspect,false);
            }
        }
    }
    if(ext && !kernel_body && lower.core()) {
        if(name=="br_box"){
            const auto& source=core_types.at(args[4]);
            if(const auto* c=lower.program.find_class(source);(!c || !c->is_interface) && stringable_type(lower.program,source))
                lower.boxed_formatters.insert(source);
        }
        if(name=="br_memory_move"){
            const auto* memory=lower.program.find_class("Bolt.Runtime.MemoryOps");
            return call(method_symbol(*memory,*find_method(*memory,"Move")),result,
                        {args[0],args[2],args[3],args[4]},inspect,false);
        }
        if(name=="br_collection") {
            const auto& operation=core_strings.at(args[4]);
            if(operation=="FindInteger")
                return lower.core_call(*this,"FindInteger",result,{args[2],args[3],args[5],args[6]},inspect);
            if(collection_kind(core_types.at(args[2]))=="List"){
                static const std::array<std::string_view,8> operations{
                    "Reserve","Add","Insert","RemoveAt","Truncate","Clear","Swap","ToArray"};
                const auto it=std::find(operations.begin(),operations.end(),operation);
                if(it!=operations.end()){
                    args[4]=integer(Type::I32,it-operations.begin());
                    return lower.core_call(*this,"BufferAction",result,std::move(args),inspect);
                }
            }
        }
        if(name=="br_fail") {
            const auto& message=core_strings.at(args[2]);
            const char* exception="Exception";
            if(message.find("by zero")!=message.npos)exception="DivideByZeroException";
            else if(message.find("overflow")!=message.npos || message.find("underflow")!=message.npos)exception="OverflowException";
            else if(message.find("index")!=message.npos || message.find("bounds")!=message.npos)exception="IndexOutOfRangeException";
            else if(message.find("null pointer")!=message.npos)exception="NullReferenceException";
            else if(message.find("optional value")!=message.npos)exception="InvalidOperationException";
            args.insert(args.begin()+2,type(std::string("System.")+exception));
            return lower.core_call(*this,"CheckedFailure",result,std::move(args),inspect);
        }
        if(name=="br_compare" && core_types[args[2]]=="string")
            return lower.core_call(*this,"Compare",result,std::move(args),inspect);
        if(name=="br_compare")
            return lower.core_call(*this,"CompareValue",result,{args[2],args[3],args[4],args[5]},inspect);
        if(name=="br_value_hash" && core_types[args[2]]=="string")
            return lower.core_call(*this,"HashText",result,{args[3]},inspect);
        if(name=="br_value_hash")
            return lower.core_call(*this,"ValueHash",result,{args[2],args[3]},inspect);
        if(name=="br_text") {
            const auto t=core_types.at(args[3]);
            if(is_interface(lower.program,t)){
                auto receiver=snapshot({t,args[4]});
                return call("bolt_format_box",Type::Void,{context,args[2],receiver.value},inspect,false);
            }
            if(is_optional(t)){
                const auto present=block(),absent=block(),done=block();
                branch(load(Type::I8,args[4]),present,absent);
                current=present;
                call("br_text",Type::Void,{context,args[1],args[2],type(element_type(t)),offset(args[4],8)},inspect);
                jump(done);current=absent;
                lower.core_call(*this,"br_string",Type::Void,{context,args[1],args[2],string("null"),integer(Type::I64,4)},inspect);
                jump(done);current=done;return 0;
            }
            // With an exact receiver the compiler knows the formatter. Preserve
            // the value snapshot when its method is allowed to mutate the receiver.
            if(const auto* cls=lower.program.find_class(t);cls && !cls->is_native && !cls->is_interface &&
               derives_from(lower.program,t,"System.IStringable") &&
               (cls->is_struct || std::none_of(lower.program.classes.begin(),lower.program.classes.end(),
                    [&](const Class& candidate){return candidate.base==t && emitted_class(candidate);}))) {
                const auto* iface=lower.program.find_class("System.IStringable");
                const auto* contract=find_method(*iface,"ToString");
                for(auto* owner=cls;contract && owner;owner=lower.program.find_class(owner->base)){
                    const auto* formatter=method_by_id(*owner,contract->id);
                    if(!formatter)continue;
                    if(concrete(*formatter) && !formatter->is_abstract){
                        auto receiver=args[4];
                        if(!formatter->readonly_receiver){
                            receiver=temp(t);assign(t,receiver,from_memory(t,args[4]));
                        }
                        return call(method_symbol(*owner,*formatter),Type::Void,{context,args[2],receiver},inspect,false);
                    }
                    break;
                }
            }
            // Copy the concrete receiver, including fields below the static base.
            // Formatting a collection element may call a mutating override.
            const auto* cls=lower.program.find_class(t);
            if(cls && ((!cls->is_struct && !cls->is_native) || derives_from(lower.program,t,"System.Exception"))){
                lower.boxed_formatters.insert(t);
                auto boxed=temp("System.IStringable");
                call("br_box",Type::Void,{context,args[1],type("System.IStringable"),boxed,args[3],args[4]});
                return call("bolt_format_box",Type::Void,{context,args[2],boxed},inspect,false);
            }
            if(is_array(t) || t=="Arena" || (cls && cls->is_struct && !cls->is_native)){
                const auto message=cls?"type '"+t+"' does not implement IStringable":"expected a numeric native type";
                return call("br_fail",Type::Void,{context,args[1],string(message)},inspect);
            }
            if(is_pointer(t))return lower.core_call(*this,"PointerText",result,{context,args[1],args[2],args[4]},inspect);
            if(t=="string" || t=="char")return lower.core_call(*this,"Text",result,std::move(args),inspect);
            if(is_floating(t)){
                const auto* strings=lower.program.find_class("Bolt.Runtime.StandaloneStrings");
                auto number=load(t=="float"?Type::F32:Type::F64,args[4]);
                if(t=="float")number=cast(number,Type::F64,"fpext");
                return call(method_symbol(*strings,*find_method(*strings,"FloatText")),Type::Void,
                    {context,args[2],number,integer(Type::I32,t=="float"?6:15)},inspect,false);
            }
            if(is_integer(t)||lower.program.find_enum(t))return lower.core_call(*this,"IntegerText",result,std::move(args),inspect);
            if(t=="bool") {
                const auto value=load(Type::I8,args[4]);
                const auto bytes=instruction(Op::Select,Type::Ptr,{value,string("true"),string("false")});
                const auto size=instruction(Op::Select,Type::I64,{value,integer(Type::I64,4),integer(Type::I64,5)});
                return lower.core_call(*this,"br_string",result,{args[0],args[1],args[2],bytes,size},inspect);
            }
        }
    }
    if(ext && !kernel_body && name.starts_with("br_") && lower.core() && find_method(*lower.core(),name))
        return lower.core_call(*this,name,result,std::move(args),inspect);
    if(ext){std::vector<Type> params;for(auto a:args)params.push_back(fn.registers[a]);lower.external(name,result,params);}auto value=instruction(Op::Call,result,std::move(args),0,name);if(inspect)check();return value;}
void Builder::check(){if(kernel_body)return;auto fail=load(Type::I32,context,"execution context");auto next=block();branch(fail,failure,next);current=next;}
void Builder::guard(Reg invalid,const std::string& message){
    if(kernel_body)return;
    if (guard_fallback) {
        const auto next=block();
        branch(invalid,*guard_fallback,next);
        current=next;
    } else {
        const auto bad=block(),next=block();
        branch(invalid,bad,next);current=bad;
        call("br_fail",Type::Void,{context,site(),string(message)},false);jump(failure);
        current=next;
    }
}
void Builder::tick(){if(lower.module.instrument_budget && !kernel_body)call("br_tick",Type::Void,{context,site()});}

void Builder::finish(){
    if(!ended())jump(exit);
    current=error;at=fn.at;if(frames)call("br_frame",Type::Void,{context,site(),string(fn.display_name.empty()?fn.name:fn.display_name)},false);jump(exit);
    current=exit;
    if(!completes_type.empty()){
        const auto& l=lower.layout(completes_type);
        if(l.marker!=static_cast<std::size_t>(-1)){
            const auto okay=block(),done=block();branch(load(Type::I32,context,"execution context"),done,okay);current=okay;
            store(offset(self,l.marker),type(completes_type));jump(done);current=done;
        }
    }
    drop_since(0);
    if(entered_slot)call("br_leave",Type::Void,{context,load(Type::I32,entered_slot)},false);
    Instruction ret;ret.op=Op::Return;if(fn.result!=Type::Void){if(!return_slot)throw std::runtime_error("missing native return slot for "+fn.name);ret.args={load(fn.result,return_slot)};}emit(ret);
    promote_private_swaps();
    auto& start=fn.blocks[0].code;start.insert(start.begin(),prologue.begin(),prologue.end());lower.module.functions.push_back(std::move(fn));
}

} // namespace bolt::ir::lowering
