#include "lowering/Lowering.hpp"
#include <algorithm>

namespace bolt::ir {
struct MethodEnvironment {
    std::map<std::string,lowering::Layout> layouts;
    std::map<std::string,int> virtual_slots;
    std::unordered_map<std::string,External> signatures;
    std::unordered_set<std::string> globals;
    LowerOptions options;
};
}

namespace bolt::ir::lowering {

// StandaloneCore reads these type pointers from its execution context.
static constexpr std::pair<std::size_t,const char*> core_errors[]{
    {56,"System.OutOfMemoryException"}, {64,"System.IndexOutOfRangeException"},
    {72,"System.OverflowException"}, {80,"System.InvalidOperationException"},
    {88,"System.ArgumentNullException"}, {168,"System.FormatException"}
};

std::string mangle(const std::string& text){static constexpr char hex[]="0123456789abcdef";std::string s="b_";for(unsigned char c:text){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))s+=static_cast<char>(c);else{s+='_';s+=hex[c>>4];s+=hex[c&15];}}return s;}
std::string method_symbol(const Class& c,const Method& m){return mangle(c.name+"::"+m.id);}
bool concrete(const Method& m){return m.generic_parameters.empty()&&m.pack_parameter.empty()&&(m.analyzed||m.is_abstract||m.is_extern);}

Reg Lower::core_call(Builder& b,const std::string& name,Type result,std::vector<Reg> args,bool inspect) {
    const auto* c=core();
    const auto* m=c?find_method(*c,name):nullptr;
    if(!m)throw std::runtime_error("missing Bolt core operation: "+name);
    args.insert(args.begin(),b.context); // Lifetime callbacks execute on the calling Bolt context.
    const auto expected=parameters(*m);
    if(result_type(*m)!=result || expected.size()!=args.size())
        throw std::runtime_error("Bolt core ABI mismatch in "+name);
    for(std::size_t i=0;i<args.size();++i)if(b.fn.registers.at(args[i])!=expected[i])
        throw std::runtime_error("Bolt core ABI argument mismatch in "+name);
    return b.call(method_symbol(*c,*m),result,std::move(args),inspect,false);
}

void Lower::core_descriptors() {
    using namespace native_abi;
    for(const auto& c:program.classes)
        if(c.token.at.file=="<bolt-stdlib>/System/Exceptions.bolt") (void)layout(c.name);
    // The loader resolves these immutable pointers before the application's entry.
    // Unhandled layouts retain their ordinary writable slots and fail the proof.
    std::vector<Global> descriptors;
    for(const auto& [name,l]:layouts) {
        const auto* c=program.find_class(name);
        const bool exception=c && c->token.at.file=="<bolt-stdlib>/System/Exceptions.bolt";
        const bool record=c && !c->is_native && !c->is_static &&
            (l.kind==Struct || l.kind==native_abi::Class) &&
            std::none_of(c->fields.begin(),c->fields.end(),[](const Field& f){return !f.is_static && is_reference(f.type);});
        if(l.kind!=Interface && !scalar_storage_kind(l.kind) && l.kind!=String && l.kind!=Array && l.kind!=List && l.kind!=Optional &&
           l.kind!=Range && l.kind!=ErrorState && !exception && !record && l.kind!=native_abi::Arena && l.kind!=SharedResource)continue;
        Global d{l.symbol+"_core",std::vector<std::uint8_t>(48),false,8,{}};
        auto word=[&](std::size_t offset,std::uint64_t value){
            for(unsigned i=0;i<8;++i)d.bytes[offset+i]=static_cast<std::uint8_t>(value>>(i*8));
        };
        word(0,l.standalone_kind(c));word(8,l.size);
        if(l.kind==Interface)word(40,capture_cell_type(name));
        if(l.kind==Array || l.kind==Optional){
            d.relocations.push_back({16,layouts.at(element_type(name)).symbol+"_core",0});
            if(l.kind==Array)word(40,array_rank(name));
        }
        if(l.kind==List){
            d.relocations.push_back({16,layouts.at(generic_args(name).front()).symbol+"_core",0});
            word(40,1);
        }
        if(record){
            // Record lifetime metadata follows the field map. Function relocations
            // come exclusively from compiler-resolved, typed Bolt methods.
            const auto tail=48+16*l.fields.size();
            d.bytes.resize(tail+40);
            word(tail,l.marker);
            auto lifetime=[&](const bolt::Class& declaring,const Method& method,std::size_t offset){
                const auto symbol=method_symbol(declaring,method);
                lifetime_entries.insert(symbol);
                d.relocations.push_back({offset,symbol,0});
            };
            for(const auto& method:c->methods)
                if(method.is_destructor && concrete(method))lifetime(*c,method,tail+8);
            // Copying a derived value must also clone an Owner base subobject.
            for(auto* base=c;base;base=program.find_class(base->base)){
                if(base->token.at.file!="<bolt-stdlib>/System/Ownership.bolt" ||
                   generic_base(base->name)!="System.Memory.Owner")continue;
                if(const auto* copy=find_method(*base,"CopyValue");copy && concrete(*copy))
                    lifetime(*base,*copy,tail+16);
                break;
            }
            auto contract=[&](std::size_t offset,const std::string& target,const char* invoker){
                value_contract_invokers.emplace(target,method_symbol(*core(),*find_method(*core(),invoker)));
                d.relocations.push_back({offset,target,0});
            };
            const auto* equal=equality_contract(*c);
            if(equal && concrete(*equal)){
                auto symbol=method_symbol(*c,*equal);
                if(!is_reference(equal->parameters[0].type) || !is_reference(equal->parameters[1].type)){
                    symbol+="_boxed";
                    Builder adapter(*this,symbol,Type::I8,{Type::Ptr,Type::Ptr,Type::Ptr},equal->token.at);
                    adapter.return_slot=adapter.alloc(1);
                    const auto value=adapter.invoke_operator(*c,*equal,{{name,2},{name,3}});
                    adapter.store(adapter.return_slot,value.value);
                    adapter.finish();
                }
                contract(tail+24,symbol,"InvokeEquality");
            }
            if(const auto* hash=hash_contract(*c);hash && concrete(*hash) && !hash->is_abstract)
                contract(tail+32,method_symbol(*c,*hash),"InvokeHash");
            else if(equal)contract(tail+32,method_symbol(*core(),*find_method(*core(),"ZeroHash")),"InvokeHash");
            word(40,l.fields.size());
            for(std::size_t i=0;i<l.fields.size();++i) {
                const auto& f=l.fields[i];word(48+16*i,f.offset);
                d.relocations.push_back({56+16*i,layouts.at(f.type).symbol+"_core",0});
            }
        }
        if((exception||record)&&!c->base.empty())d.relocations.push_back({24,layouts.at(c->base).symbol+"_core",0});
        if(exception){
            d.bytes.resize(56);
            const char* message=name=="System.OutOfMemoryException"?"Out of memory.":
                name=="System.InvalidOperationException"?"collection was structurally modified during iteration":
                name=="System.ArgumentNullException"?"UTF-8 bytes cannot be null when length is nonzero.":
                name=="System.FormatException"?"Invalid UTF-8 sequence.":
                name=="System.OverflowException"?"array size overflow":"index out of range";
            d.relocations.push_back({48,literal(message),0});
        }
        d.relocations.push_back({32,literal(name),0});
        std::sort(d.relocations.begin(),d.relocations.end(),[](const Relocation& a,const Relocation& b){return a.offset<b.offset;});
        auto slot=std::find_if(module.globals.begin(),module.globals.end(),[&](const Global& g){return g.name==l.symbol;});
        if(slot!=module.globals.end()) {slot->writable=false;slot->relocations={{0,d.name,0}};}
        descriptors.push_back(std::move(d));
    }
    for(auto& d:descriptors)module.globals.push_back(std::move(d));
}

std::string Lower::text_literal(const std::string& value) {
    const auto bytes = literal(value);
    const auto name = bytes + "_text";
    if (text_literals.insert(name).second) {
        std::vector<std::uint8_t> header(sizeof(BrTextHeader));
        const auto length = static_cast<std::uint64_t>(value.size());
        // Supported native ABIs use little-endian 64-bit descriptors.
        for (std::size_t i = 0; i < sizeof(length); ++i)
            header[offsetof(BrTextHeader,size) + i] = static_cast<std::uint8_t>(length >> (8*i));
        module.globals.push_back({name,std::move(header),false,alignof(BrTextHeader),
                                  {{offsetof(BrTextHeader,data),bytes,0}}});
    }
    return name;
}

std::string Lower::literal(const std::string& s) {auto i=strings.find(s);if(i!=strings.end())return i->second;auto name="b_string_"+std::to_string(strings.size());strings[s]=name;std::vector<std::uint8_t> bytes(s.begin(),s.end());bytes.push_back(0);module.globals.push_back({name,std::move(bytes),false,1,{}});return name;}

void Lower::external(const std::string& name,Type result,const std::vector<Type>& parameters) {
    const auto [index,added]=external_indices.try_emplace(name,module.externals.size());
    if(added) {
        module.externals.push_back({name,result,parameters});
        return;
    }
    const auto& previous=module.externals[index->second];
    if(previous.result!=result || previous.parameters!=parameters)
        throw std::runtime_error("inconsistent native ABI declaration for "+name);
}

std::string_view Lower::source_line(const std::string& file,int line) {
    auto source=program.sources.find(file);
    if(source==program.sources.end() || line<1) return {};
    const std::string_view bytes=source->second;
    auto [it,added]=line_starts.try_emplace(file);
    auto& starts=it->second;
    if(added) {
        starts.push_back(0);
        for(auto pos=bytes.find('\n');pos!=bytes.npos;pos=bytes.find('\n',pos+1)) starts.push_back(pos+1);
    }
    const auto i=static_cast<std::size_t>(line-1);
    if(i>=starts.size()) return {};
    auto end=i+1<starts.size()?starts[i+1]-1:bytes.size();
    if(end>starts[i] && bytes[end-1]=='\r') --end;
    return bytes.substr(starts[i],end-starts[i]);
}

Module Lower::run() {
    for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native){
        (void)layout(c.name);
        for(const auto& f:c.fields)if(f.is_static){statics.emplace_back(&c,&f);auto l=layout(f.type);module.globals.push_back({static_name(c.name,f.name),std::vector<std::uint8_t>(l.size),true,8,{}});module.globals.push_back({static_name(c.name,f.name)+"_state",std::vector<std::uint8_t>(4),true,4,{}});}
        for(const auto& m:c.methods)if(concrete(m)&&m.is_virtual&&!virtual_slots.contains(m.id))virtual_slots[m.id]=static_cast<int>(virtual_slots.size());
    }
    for(const auto& c:program.classes)if(c.is_native&&layouts.contains(c.name))for(const auto& m:c.methods)if(m.is_virtual&&concrete(m)&&!virtual_slots.contains(m.id))virtual_slots[m.id]=static_cast<int>(virtual_slots.size());
    if(options.edit_data) for(const auto& [expr,slot]:options.edit_data->slots) {
        std::vector<std::uint8_t> bytes(8);
        for(unsigned i=0;i<8;++i) bytes[i]=static_cast<std::uint8_t>(slot.bits>>(8*i));
        module.globals.push_back({slot.symbol,std::move(bytes),false,8,{}});
    }
    methods();static_initializers();initializers();
    // A catch materializes native failures as typed objects. A finally
    // alone just preserves and restores the existing error state.
    if (std::any_of(module.externals.begin(),module.externals.end(),
        [](const External& e){return e.name=="br_catch_begin";}))
        for(const auto& c:program.classes)if(c.is_native&&c.token.at.file.find("Exceptions.bolt")!=std::string::npos)(void)layout(c.name);
    native_initializers();native_virtual_adapters();stringable_adapters();
    if(!program.library_mode)entry();
    else if(options.source_core)module.runtime_free_library=prove_runtime_free(nullptr,nullptr);
    if(!options.source_core && !module.runtime_free() && (program.library_mode || !program.external_sources.empty()))project_functions();
    if(!options.source_core || module.runtime_free())verify(module);
    if(options.edit_data) {
        auto env=std::make_shared<MethodEnvironment>();
        env->layouts=layouts;
        env->virtual_slots=virtual_slots;
        env->options=options;
        env->options.edit_data=nullptr;
        for(const auto& f:module.functions) env->signatures.emplace(f.name,External{f.name,f.result,f.parameters});
        for(const auto& e:module.externals) env->signatures.emplace(e.name,e);
        for(const auto& g:module.globals) env->globals.insert(g.name);
        options.edit_data->methods=std::move(env);
    }
    return std::move(module);
}

// ThreadKernel takes explicit execution contexts and otherwise uses unmanaged
// storage. The OS entry is shared by async bodies and captured Run methods.
Reg Lower::thread_call(Builder& b,const std::string& name,Type result,std::vector<Reg> args,bool inspect){
    const auto* kernel=program.find_class("Bolt.Runtime.ThreadKernel");
    const auto* method=kernel?find_method(*kernel,name):nullptr;
    if(!method)throw std::runtime_error("missing Bolt thread operation: "+name);
    args.insert(args.begin(),core()?b.context:b.null());
    return b.call(method_symbol(*kernel,*method),result,std::move(args),inspect,false);
}
std::string Lower::job_adapter(const Class& owner,const Method& method){
    const auto target=method_symbol(owner,method), adapter=target+"_job";
    if(thread_entries.insert(adapter).second){
        worker_callbacks.insert(target);
        Builder b(*this,adapter,Type::Void,{Type::Ptr,Type::Ptr,Type::Ptr},method.token.at);
        b.frames=false;
        b.call(target,Type::Void,{b.context,b.load(Type::Ptr,3)},false,false);
        b.finish();
    }
    return adapter;
}
std::string Lower::thread_entry(){
    const std::string name="bolt_thread_entry";
    if(!thread_entries.insert(name).second)return name;
    auto result=program.preprocessor.platform=="Windows"?Type::I32:Type::Ptr;
    Builder b(*this,name,result,{Type::Ptr},{"<bolt-stdlib>/System/ThreadKernel.bolt",1,1});
    b.frames=false;b.return_slot=b.alloc(width(result));
    auto slot=[&](const std::string& member){return field(b,"Bolt.Runtime.ThreadResource",member,1);};
    auto get=[&](const std::string& member){return b.load(Type::Ptr,slot(member));};
    b.context=get("Context");
    auto activate=[&](Reg ctx){return core()?core_call(b,"br_context_activate",Type::Ptr,{ctx},false):
        b.call("br_context_activate",Type::Ptr,{ctx},false);};
    const auto previous=activate(b.context);
    thread_call(b,"Lock",Type::Void,{1});
    b.instruction(Op::AtomicAdd,Type::I64,{slot("Word"),thread_call(b,"Identity",Type::I64,{})});
    thread_call(b,"Wake",Type::Void,{1});thread_call(b,"Unlock",Type::Void,{1});
    b.instruction(Op::CallIndirect,Type::Void,{get("Body"),b.context,get("Result"),get("Arguments")});
    const auto fault=b.block(),finish=b.block();
    b.branch(b.load(Type::I32,b.context),fault,finish);b.current=fault;
    b.store(slot("Failed"),b.integer(Type::I8,1));
    auto canceled=b.call("br_exception_match",Type::I32,{b.context,b.type("System.OperationCanceledException")},false);
    b.store(slot("Canceled"),b.boolean(canceled));
    auto error=b.call("br_catch_begin",Type::Ptr,{b.context,b.site()},false);
    const auto capture=b.block();
    b.branch(b.cmp("ne",error,b.null()),capture,finish);b.current=capture;
    auto actual=b.load(Type::Ptr,error);
    b.store(slot("FaultType"),actual);
    b.store(slot("Fault"),thread_call(b,"CopyValue",Type::Ptr,{b.context,actual,error}));
    b.call("br_catch_end",Type::Void,{b.context},false);
    b.jump(finish);b.current=finish;
    thread_call(b,"Finish",Type::Void,{1});thread_call(b,"Release",Type::Void,{1});
    activate(previous);b.finish();return name;
}

// The public method snapshots all operands before scheduling. The worker sees only
// owned value slots, with one result address for scalar and aggregate payloads alike.
void Lower::async_method(const Class& c,const Method& m){
    const auto symbol=method_symbol(c,m);
    const auto payload=generic_args(m.type).front();
    const auto& task=*program.find_class(m.type);
    const auto ctor=std::find_if(task.methods.begin(),task.methods.end(),[](const Method& v){
        return v.is_constructor && v.parameters.size()==1;
    });
    if(ctor==task.methods.end())throw Error(m.token.at,"Task has no completion-state constructor","BLT4001");
    const auto state=ctor->parameters[0].type;
    Builder start(*this,symbol,Type::Void,parameters(m),m.token.at);
    start.fn.display_name=c.name+"."+m.name;
    Builder body(*this,symbol+"_body",Type::Void,{Type::Ptr,Type::Ptr,Type::Ptr},m.token.at);
    body.owner=c.name;body.fn.display_name=start.fn.display_name;body.return_type=payload;body.structure_return=2;
    const auto count=m.parameters.size()+!m.is_static;
    const auto types=count?start.alloc(count*8,false):start.null();
    const auto values=count?start.alloc(count*8,false):start.null();
    Reg next=3;
    auto capture=[&](const std::string& t){
        const auto index=next-3;
        const auto source=start.memory({t,next++});
        start.store(start.offset(types,index*8),start.type(t));
        start.store(start.offset(values,index*8),source);
        return body.load(Type::Ptr,body.offset(3,index*8));
    };
    if(!m.is_static)body.self=capture(c.name);
    for(const auto& p:m.parameters)body.locals[p.local_id]=capture(p.type);
    if(module.instrument_budget){
        body.entered_slot=body.alloc(4);
        body.store(body.entered_slot,body.call("br_enter",Type::I32,{body.context,body.site()},false));
        body.check();
    }
    body.tick();body.statement(*m.body);body.finish();
    const auto completion=start.temp(state);
    thread_entries.insert(symbol+"_body");
    auto job=thread_call(start,"Prepare",Type::Ptr,{start.context,payload=="void"?start.null():start.type(payload),
        start.address(symbol+"_body"),types,values,start.integer(Type::I64,count)},true);
    start.store(field(start,state,"Handle",completion),job);
    auto launched=thread_call(start,"Launch",Type::I8,{start.context,job,start.address(thread_entry())});
    start.guard(start.cmp("eq",launched,start.integer(Type::I8,0)),"Operating system could not create a thread.");
    const std::vector<Reg> args{completion};
    start.invoke(task,*ctor,2,{},0,false,&args);
    start.finish();
}

// Lower dependency bodies to discover the layouts and dispatch entries used throughout
// the graph. At the artifact boundary replace each imported definition with a local
// thunk, so taking a method address also works with DLL imports and PIC archives.
void Lower::project_functions(){
    std::map<std::string,bool> symbols;
    auto record=[&](const Class& c,const std::string& file,const std::string& symbol){
        if(file.starts_with("<bolt-stdlib>") || file.starts_with("<bolt-generated>") || c.name.find('<')!=std::string::npos)return;
        const bool imported=std::find(program.external_sources.begin(),program.external_sources.end(),file)!=program.external_sources.end();
        if(imported || program.library_mode)symbols.emplace(symbol,imported);
    };
    for(const auto& c:program.classes){
        if(c.is_native)continue;
        record(c,c.token.at.file,init_name(c.name));
        for(const auto& f:c.fields)if(f.is_static)record(c,f.token.at.file,getter(c.name,f.name));
        for(const auto& m:c.methods)if(!m.is_specialization&&!m.name.starts_with("@lambda"))
            record(c,c.is_global?m.token.at.file:c.token.at.file,method_symbol(c,m));
    }
    const auto definitions=module.functions.size();
    for(std::size_t i=0;i<definitions;++i){
        const auto& fn=module.functions[i];
        auto symbol=symbols.find(fn.name);
        if(symbol==symbols.end())continue;
        const bool imported=symbol->second;
        const auto exported="bolt_lib_"+fn.name;
        const auto callee=imported?exported:fn.name;
        Function thunk;
        thunk.name=imported?fn.name:exported;thunk.at=fn.at;thunk.result=fn.result;
        thunk.parameters=fn.parameters;thunk.exported=!imported;
        thunk.blocks.emplace_back();
        Instruction call;call.at=thunk.at;call.op=Op::Call;call.text=callee;
        for(auto type:thunk.parameters){
            call.args.push_back(static_cast<Reg>(thunk.registers.size()));
            thunk.registers.push_back(type);
        }
        Instruction ret;ret.op=Op::Return;ret.at=thunk.at;
        if(fn.result!=Type::Void){
            call.result=static_cast<Reg>(thunk.registers.size());
            thunk.registers.push_back(fn.result);ret.args={call.result};
        }
        thunk.blocks.front().code={std::move(call),std::move(ret)};
        if(imported){
            external(callee,fn.result,fn.parameters);
            module.functions[i]=std::move(thunk);
        }else module.functions.push_back(std::move(thunk));
    }
}

void Lower::methods(){
    for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native)for(const auto& m:c.methods)if(concrete(m)&&!m.is_abstract&&!m.is_extern){
        method(c,m);
    }
}
void Lower::method(const Class& c,const Method& m) {
    if(m.is_async){async_method(c,m);return;}
    Builder b(*this,method_symbol(c,m),result_type(m),parameters(m),m.token.at);b.fn.display_name=c.name+"."+m.name;b.return_type=m.type;b.owner=c.name;if(m.is_constructor)b.completes_type=c.name;
    if (&c==core() || c.name=="Bolt.Runtime.ThreadKernel" || c.name=="Bolt.Runtime.SystemLifetime") { b.kernel_body=true; b.frames=false; }
    // These source-runtime routines construct an exception or append an error
    // frame. Keep them out of the normal path even when a caller is inlined.
    if (&c==core())
        b.fn.cold = m.name=="CheckedFailure" || m.name=="Bounds" || m.name=="OutOfMemory" ||
                    m.name=="Error" || m.name=="br_frame";
    Reg next=2;if(!m.is_constructor&&m.type!="void"&&!is_reference(m.type)&&layout(m.type).aggregate)b.structure_return=next++;
    if(!m.is_static)b.self=next++;
    if(b.fn.result!=Type::Void)b.return_slot=b.alloc(width(b.fn.result));
    for(const auto& p:m.parameters){auto reg=next++;if(is_reference(p.type))b.locals[p.local_id]=reg;else if(layout(p.type).aggregate)b.locals[p.local_id]=reg;else{auto slot=b.temp(p.type,false);b.store(slot,reg);b.locals[p.local_id]=slot;}}
    if(module.instrument_budget && !b.kernel_body){b.entered_slot=b.alloc(4);auto entered=b.call("br_enter",Type::I32,{b.context,b.site()},false);b.store(b.entered_slot,entered);b.check();}
    // Generic value cleanup is reachable even in a program that never opens a
    // socket. Only the owning socket type can acquire a Winsock reference.
    if(c.name=="Bolt.Runtime.SystemLifetime" && m.name=="CloseSystemSocket" && !layouts.contains("Bolt.Runtime.Net.SocketHandle")){
        b.finish();return;
    }
    if(c.name=="Bolt.Runtime.TypedMemory" && c.token.at.file=="<bolt-stdlib>/System/TypedMemory.bolt" && m.name=="Address"){
        b.store(b.return_slot,2);b.finish();return;
    }
    if (!module.instrument_budget && c.name=="Bolt.Runtime.ScalarMath" &&
        c.token.at.file=="<bolt-stdlib>/System/ScalarMath.bolt" && m.name=="PositiveRoot") {
        // Root has already checked the domain and handled signed zero.
        b.store(b.return_slot,b.instruction(Op::FSqrt,Type::F64,{2}));b.finish();return;
    }
    if(&c==core() && m.name=="InvokeLifetime"){
        b.instruction(Op::CallIndirect,Type::Void,{2,b.context,3});b.finish();return;
    }
    if(&c==core() && (m.name=="InvokeEquality" || m.name=="InvokeHash")){
        std::vector<Reg> args{2,b.context,3};
        if(m.name=="InvokeEquality")args.push_back(4);
        b.store(b.return_slot,b.instruction(Op::CallIndirect,b.fn.result,std::move(args)));
        b.finish();return;
    }
    b.tick();if(m.is_constructor)b.initialize_class(c,&m);b.statement(*m.body);b.finish();
}

void Lower::initializers(){
    // Only classes need executable default initializers. Arrays, strings and
    // collections default to an empty owning value (an internal null handle).
    for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native&&!c.is_static&&!c.is_interface){
        Builder b(*this,init_name(c.name),Type::Void,{Type::Ptr,Type::Ptr},c.token.at);b.self=2;b.owner=c.name;b.frames=false;b.completes_type=c.name;
        if(auto* ctor=default_constructor(c)){static const std::vector<ExprPtr> noargs;b.invoke(c,*ctor,b.self,noargs,0,false);}
        else {bool any=false;for(const auto& m:c.methods)any|=m.is_constructor;if(any){b.call("br_fail",Type::Void,{b.context,b.site(),b.string("type '"+c.name+"' has no default constructor")},false);b.jump(b.error);}else b.initialize_class(c,nullptr);}
        b.finish();
    }
}

void Lower::native_virtual_adapters(){
    for(const auto& c:program.classes)if(c.is_native&&layouts.contains(c.name))for(const auto& m:c.methods)if(concrete(m)&&m.is_virtual&&!m.is_abstract){
        // Direct native calls already use the library bridge. Only registered
        // slots, inherited formatters and worker callbacks need an adapter.
        if (!dispatched.contains(m.id) && m.name != "ToString" &&
            !worker_callbacks.contains(method_symbol(c,m))) continue;
        Builder b(*this,method_symbol(c,m),result_type(m),parameters(m),m.token.at);b.frames=false;b.owner=c.name;b.return_type=m.type;
        Reg next=2;if(m.type!="void"&&!is_reference(m.type)&&layout(m.type).aggregate)b.structure_return=next++;
        b.self=next++;if(b.fn.result!=Type::Void)b.return_slot=b.alloc(width(b.fn.result));
        std::vector<Reg> params;for(const auto& p:m.parameters){(void)p;params.push_back(next++);}
        static const std::vector<ExprPtr> none;auto value=b.invoke(c,m,b.self,none,0,false,&params);
        if (b.structure_return && options.optimize_loops && (is_array(m.type) || m.type == "string"))
            b.take_handle(b.structure_return,value);
        else if(m.type!="void")b.assign(m.type,b.structure_return?b.structure_return:b.return_slot,value);
        b.finish();
    }
}
void Lower::native_initializers(){
    std::set<std::string> done;
    for(;;){bool changed=false;
        for(const auto& c:program.classes)if(c.is_native&&!c.is_static&&!c.is_abstract&&layouts.contains(c.name)&&!done.contains(c.name)){
            done.insert(c.name);changed=true;
            Builder b(*this,init_name(c.name),Type::Void,{Type::Ptr,Type::Ptr},c.token.at);b.self=2;b.owner=c.name;b.frames=false;b.completes_type=c.name;
            if(auto* ctor=default_constructor(c)){static const std::vector<ExprPtr> none;b.invoke(c,*ctor,b.self,none,0,false);}
            else if(native_abi::library_pair(c))b.initialize_class(c,nullptr);
            else {b.call("br_fail",Type::Void,{b.context,b.site(),b.string("type '"+c.name+"' has no default constructor")},false);b.jump(b.error);}
            b.finish();
        }
        if(!changed)break;
    }
}

void Lower::static_initializers(){
    for(auto [c,f]:statics){
        Builder b(*this,getter(c->name,f->name),Type::Ptr,{Type::Ptr},f->token.at);b.fn.display_name=c->name+"."+f->name+" initializer";b.owner=c->name;b.return_slot=b.alloc(8);
        auto storage=static_name(c->name,f->name);
        auto address=[&](const std::string& key){return program.library_mode&&!options.source_core?b.project_symbol(key):b.address(key);};
        auto data=address(storage);b.store(b.return_slot,data);auto state=address(storage+"_state");auto loaded=b.load(Type::I32,state);auto init=b.block();b.branch(b.cmp("eq",loaded,b.integer(Type::I32,2)),b.exit,init);b.current=init;b.guard(b.cmp("eq",loaded,b.integer(Type::I32,1)),"recursive static initialization");b.store(state,b.integer(Type::I32,1));
        if(f->initializer)b.assign(f->type,data,b.expression(*f->initializer));else b.initialize(f->type,data);b.store(state,b.integer(Type::I32,2));b.finish();
    }
}
void Lower::stringable_adapters(){
    const auto* iface=program.find_class("System.IStringable");
    if(core() && iface){
        const auto* text_class=program.find_class("Bolt.Runtime.StandaloneStrings");
        const auto missing=method_symbol(*text_class,*find_method(*text_class,"MissingFormatter"));
        // A borrowed base receiver can denote any concrete derived value.
        const auto sources=boxed_formatters;
        for(const auto& c:program.classes){
            const bool builtin_exception=c.token.at.file=="<bolt-stdlib>/System/Exceptions.bolt";
            if((!builtin_exception && (!emitted_class(c) || c.is_native)) || c.is_abstract || c.is_interface ||
               !c.generic_parameters.empty() || !c.pack_parameter.empty())continue;
            for(auto* base=program.find_class(c.base);base;base=program.find_class(base->base)){
                if(sources.contains(base->name)){boxed_formatters.insert(c.name);break;}
            }
        }
        for(bool conversion:{false,true}){
            Builder b(*this,conversion?"bolt_convert_box":"bolt_format_box",Type::Void,{Type::Ptr,Type::Ptr,Type::Ptr},iface->token.at);
            const auto value=b.call("br_unbox",Type::Ptr,{b.context,b.site(),3});
            const auto actual=core_call(b,"ReceiverType",Type::Ptr,{3},false);
            const auto* contract=find_method(*iface,conversion?"@convert.string":"ToString");
            for(const auto& name:boxed_formatters){
                const auto* cls=program.find_class(name);
                const bool record=cls && !cls->is_native;
                if(record && !stringable_type(program,name))continue;
                auto match=b.block(),next=b.block();
                b.branch(b.cmp("eq",actual,b.type(name)),match,next);b.current=match;
                const Method* method=nullptr;
                while(cls && !(method=method_by_id(*cls,contract->id)))cls=program.find_class(cls->base);
                if(cls && method && concrete(*method) && !cls->is_native && !method->is_abstract){
                    b.call(method_symbol(*cls,*method),Type::Void,{b.context,2,value},true,false);
                }else if(conversion && record){
                    b.call("bolt_format_box",Type::Void,{b.context,2,3},true,false);
                }else if(derives_from(program,name,"System.Exception")){
                    b.call(method_symbol(*text_class,*find_method(*text_class,"DescribeException")),Type::Void,{b.context,2,value},true,false);
                }else if(record){
                    b.call(missing,Type::Void,{b.context,actual},true,false);
                }else b.call("br_text",Type::Void,{b.context,b.site(),2,b.type(name),value});
                b.jump(b.exit);b.current=next;
            }
            b.call(missing,Type::Void,{b.context,actual},true,false);
            b.finish();
        }
        return;
    }
    if(!iface || std::none_of(iface->methods.begin(),iface->methods.end(),
        [&](const Method& m){return dispatched.contains(m.id);})) return;
    for(const auto& [name,l]:layouts)if(is_primitive(name)||is_pointer(name)||program.find_enum(name)||((is_optional(name)||is_collection(name)||(program.find_class(name)&&program.find_class(name)->is_native))&&stringable_type(program,name))){
        (void)l;Builder b(*this,mangle("builtin-stringable::"+name),Type::Void,{Type::Ptr,Type::Ptr,Type::Ptr},{"<bolt-stdlib>/IStringable.bolt",6,5});
        b.frames=false;b.call("br_text",Type::Void,{b.context,b.site(),2,b.type(name),3});b.finish();
    }
}
// For a closed standalone program, prove that its entire reachable call graph
// can start from ordinary program data without invoking runtime services.
bool Lower::prove_runtime_free(const Class* owner, const Method* main) {
    if (!(options.standalone_executable || options.try_runtime_free_entry)) return false;
    auto requires_runtime = [&](std::string reason) {
        module.runtime_requirement = std::move(reason);
        return false;
    };
    if (module.instrument_budget) return requires_runtime("execution budget accounting");
    if (program.entry_accepts_arguments && !core()) return requires_runtime("the string[] argument to Main");
    if (core()) core_descriptors();
    std::map<std::string, std::string> static_getters;
    std::set<std::string> static_slots;
    for (const auto& [type, field] : statics) {
        if(core()){
            const auto slot=static_name(type->name,field->name);
            static_slots.insert(slot);static_slots.insert(slot+"_state");continue;
        }
        // Literal scalar initializers have no observable effects on other fields.
        // An arbitrary initializer must still run even if Main never reads it.
        const auto* init = field->initializer.get();
        if (is_reference(field->type) || layout(field->type).aggregate ||
            (init && (init->kind != K::Literal || init->type != field->type)))
            return requires_runtime("initialization of " + type->name + "." + field->name);
        static_getters.emplace(getter(type->name, field->name), static_name(type->name, field->name));
    }
    std::map<std::string,const Function*> definitions;
    for (const auto& f : module.functions) definitions.emplace(f.name, &f);
    std::map<std::string,const Global*> constants;
    for (const auto& g : module.globals)
        if (!g.writable) constants.emplace(g.name, &g);

    // The process already waits for Main. Run its async body on the root
    // context so awaits observe workers and the root owns their shutdown.
    const bool async_entry=main && core() && main->is_async;
    const auto root = main ? method_symbol(*owner, *main)+(async_entry?"_body":"") : std::string{};
    std::set<std::string> foreign, imports;
    for (const auto& e : module.externals) {
        if (e.foreign && e.name == "br_frame") return requires_runtime("an explicit import of br_frame");
        if (e.foreign && !e.name.starts_with("br_")) foreign.insert(e.name);
    }
    std::vector<std::string> pending;
    if(main)pending.push_back(root);
    else {
        // A library built on its own has no Main to select its exported methods.
        const std::set<std::string> dependencies(program.external_sources.begin(),program.external_sources.end());
        for(const auto& f:module.functions)
            if(!f.at.file.starts_with("<") && !dependencies.contains(f.at.file))pending.push_back(f.name);
    }
    if(core() && !thread_entries.empty()){
        const auto* kernel=program.find_class("Bolt.Runtime.ThreadKernel");
        pending.push_back(method_symbol(*kernel,*find_method(*kernel,"Sweep")));
    }
    if(const auto* c=core()){
        pending.push_back(method_symbol(*c,*find_method(*c,"Finish")));
        pending.push_back(method_symbol(*c,*find_method(*c,"ReclaimAllocation")));
    }
    std::set<std::string> live;
    std::set<std::string> live_data;
    std::vector<std::string> referenced_constants;
    std::string read_arguments;
    if(program.entry_accepts_arguments){
        const auto* text_class=program.find_class("Bolt.Runtime.StandaloneStrings");
        read_arguments=method_symbol(*text_class,*find_method(*text_class,"Arguments"));
        pending.push_back(read_arguments);
        referenced_constants.push_back(layout("string[]").symbol);
        pending.push_back(method_symbol(*core(),*find_method(*core(),"br_drop")));
    }
    if(core())for(const auto& error:core_errors)referenced_constants.push_back(layout(error.second).symbol);
    if(core())for(const auto& [type,field]:statics) {
        pending.push_back(getter(type->name,field->name));
        if(layout(field->type).aggregate){
            pending.push_back(method_symbol(*core(),*find_method(*core(),"br_drop")));
            referenced_constants.push_back(layout(field->type).symbol);
        }
    }
    const auto lifetime_call=core()?method_symbol(*core(),*find_method(*core(),"InvokeLifetime")):"";
    std::map<std::string,std::set<std::string>> contract_targets;
    if(core())for(const auto* name:{"InvokeEquality","InvokeHash"})
        contract_targets[method_symbol(*core(),*find_method(*core(),name))];
    while (!pending.empty() || !referenced_constants.empty()) {
        if(pending.empty()){
            auto name=std::move(referenced_constants.back());referenced_constants.pop_back();
            if(!live_data.insert(name).second)continue;
            if(!constants.contains(name))return requires_runtime("storage for "+name);
            for(const auto& relocation:constants.at(name)->relocations){
                if(auto it=value_contract_invokers.find(relocation.symbol);it!=value_contract_invokers.end()){
                    contract_targets.at(it->second).insert(relocation.symbol);
                    if(live.contains(it->second))pending.push_back(relocation.symbol);
                } else if(lifetime_entries.contains(relocation.symbol))pending.push_back(relocation.symbol);
                else referenced_constants.push_back(relocation.symbol);
            }
            continue;
        }
        auto name = std::move(pending.back());
        pending.pop_back();
        if (!live.insert(name).second) continue;
        if(auto it=contract_targets.find(name);it!=contract_targets.end())
            pending.insert(pending.end(),it->second.begin(),it->second.end());
        auto found = definitions.find(name);
        if (found == definitions.end()) return requires_runtime(name);
        const auto& function = *found->second;
        const auto& label = function.display_name.empty() ? name : function.display_name;
        for (const auto& block : function.blocks) for (const auto& op : block.code) {
            if (op.op == Op::CallIndirect && function.name!="bolt_thread_entry" && function.name!=lifetime_call &&
                !contract_targets.contains(function.name)) return requires_runtime("indirect dispatch in " + label);
            if (op.op == Op::Address) {
                if(thread_entries.contains(op.text))pending.push_back(op.text);
                else if(static_slots.contains(op.text))live_data.insert(op.text);
                else if(constants.contains(op.text))referenced_constants.push_back(op.text);
                else {
                    for(const auto& [type,layout]:layouts)if(layout.symbol==op.text)
                        return requires_runtime("storage for "+type+" in "+label);
                    return requires_runtime("runtime data "+op.text+" in "+label);
                }
            }
            if (op.op != Op::Call || op.text == "br_frame") continue;
            if (auto slot = static_getters.find(op.text); slot != static_getters.end())
                live_data.insert(slot->second);
            else if (foreign.contains(op.text)) imports.insert(op.text);
            else if (definitions.contains(op.text)) pending.push_back(op.text);
            else return requires_runtime(label + " calls " + op.text);
        }
    }
    // With no operation that can set the error flag, frame capture is dead.
    std::erase_if(module.functions, [&](const auto& f){ return !live.contains(f.name); });
    for (auto& f : module.functions) for (auto& b : f.blocks) {
        for (auto& op : b.code) if (op.op == Op::Call) {
            if (auto slot = static_getters.find(op.text); slot != static_getters.end()) {
                op.op = Op::Address;
                op.text = slot->second;
                op.args.clear();
            }
        }
        std::erase_if(b.code, [](const auto& op){return op.op == Op::Call && op.text == "br_frame";});
    }
    std::erase_if(module.externals, [&](const auto& e){ return !imports.contains(e.name); });
    // The entry shim can add or reuse an OS import after pruning.
    external_indices.clear();
    for(std::size_t i=0;i<module.externals.size();++i)external_indices.emplace(module.externals[i].name,i);
    std::erase_if(module.globals, [&](const auto& g){return !live_data.contains(g.name);});
    for(auto& g:module.globals)
        std::erase_if(g.relocations,[&](const auto& r){return value_contract_invokers.contains(r.symbol) && !live.contains(r.symbol);});
    if(!main)return true;
    Builder start(*this, "bolt_program_entry", Type::I32, {Type::I32, Type::Ptr}, main->token.at);
    start.fn.exported = true;
    start.frames = false;
    start.return_slot = start.alloc(4);
    auto ctx = start.alloc(core()?2296:sizeof(BrContext));
    start.context=ctx;
    const auto cleanup=start.block();
    start.failure=cleanup;
    if(core()){
        for(const auto& [offset,type]:core_errors)start.store(start.offset(ctx,offset),start.type(type));
        // The source runtime shares the sequence and active-range prefixes.
        // Populate its SIMD flags only when a reachable storage shift needs them.
        const bool shifts=std::any_of(module.functions.begin(),module.functions.end(),[](const Function& f) {
            return std::any_of(f.blocks.begin(),f.blocks.end(),[](const Block& b) {
                return std::any_of(b.code.begin(),b.code.end(),[](const Instruction& i){return i.storage_move_stride!=0;});
            });
        });
        if (shifts && program.preprocessor.platform=="Windows" && program.preprocessor.architecture=="X64") {
            auto flag=[&](unsigned feature,unsigned mask) {
                const auto found=start.call("IsProcessorFeaturePresent",Type::I32,{start.integer(Type::I32,feature)},false);
                return start.instruction(Op::Mul,Type::I32,{start.cmp("ne",found,start.integer(Type::I32,0)),start.integer(Type::I32,mask)});
            };
            const auto avx2=flag(40,BrContext::Avx2), avx512=flag(41,BrContext::Avx512F);
            module.externals[external_indices.at("IsProcessorFeaturePresent")].foreign=true;
            start.store(start.offset(ctx,offsetof(BrContext,flags)),start.instruction(Op::Or,Type::I32,{avx2,avx512}));
        }
    }
    for (const auto& [type, field] : statics) {
        const auto slot = static_name(type->name, field->name);
        if(core())start.call(getter(type->name,field->name),Type::Ptr,{ctx},true,false);
        else if (field->initializer && live_data.contains(slot))
            start.store(start.address(slot), start.expression(*field->initializer).value);
    }
    std::vector<Reg> inputs{ctx};
    if(!read_arguments.empty()){
        const auto arguments=start.temp("string[]");
        start.call(read_arguments,Type::Void,{ctx,arguments,1,2},true,false);
        inputs.push_back(arguments);
    }
    if(async_entry){
        auto args=start.null();
        if(inputs.size()>1){args=start.alloc(8);start.store(args,inputs.back());}
        start.call(root,Type::Void,{ctx,start.return_slot,args},false,false);
    } else {
        auto value = start.call(root, main->type == "void" ? Type::Void : Type::I32, inputs, false, false);
        if (main->type == "void") value=start.integer(Type::I32,0);
        start.store(start.return_slot, value);
    }
    start.jump(cleanup);
    start.current=cleanup;
    if(core()){
        if(std::any_of(thread_entries.begin(),thread_entries.end(),[&](const auto& entry){return live.contains(entry);}))
            thread_call(start,"Sweep",Type::Void,{ctx,start.integer(Type::I8,1)});
        for(auto it=statics.rbegin();it!=statics.rend();++it){
            const auto& [type,field]=*it;
            if(layout(field->type).aggregate)
                core_call(start,"br_drop",Type::Void,{start.type(field->type),start.address(static_name(type->name,field->name))},false);
        }
        // Destructors may delete another allocation, allocate again, or start a
        // worker. Re-read the ledger after each callback, joining workers first.
        const auto drain=start.block(),drained=start.block();
        start.jump(drain);start.current=drain;
        if(std::any_of(thread_entries.begin(),thread_entries.end(),[&](const auto& entry){return live.contains(entry);}))
            thread_call(start,"Sweep",Type::Void,{ctx,start.integer(Type::I8,1)});
        start.branch(core_call(start,"ReclaimAllocation",Type::I8,{ctx},false),drain,drained);
        start.current=drained;
        auto status=core_call(start,"Finish",Type::I32,{ctx,start.load(Type::I32,start.return_slot)},false);
        start.store(start.return_slot,status);
    }
    start.finish();
    return true;
}

#if defined(__APPLE__) || defined(__FreeBSD__)
void Lower::libc_main(const Location& at) {
    Builder os(*this,"main",Type::I32,{Type::I32,Type::Ptr},at);
    os.fn.exported=true;os.frames=false;os.return_slot=os.alloc(4);
    os.store(os.return_slot,os.call("bolt_program_entry",Type::I32,{1,2},false,false));
    os.finish();
}
#endif

void Lower::entry(){
    const auto* entry=program.find_class(program.entry_class.empty()?"Program":program.entry_class);const auto* main=entry?find_method(*entry,"Main"):nullptr;if(!main)throw std::runtime_error("native compilation requires Program.Main");
    if ((program.external_sources.empty() || options.source_core) && prove_runtime_free(entry, main)) {
        module.runtime_free_entry=true;
#if defined(_WIN32)
        Builder os(*this,"bolt_pure_start",Type::Void,{},main->token.at);
        os.fn.exported=true;os.frames=false;
        auto status=os.call("bolt_program_entry",Type::I32,{os.integer(Type::I32,0),os.null()},false,false);
        os.call("ExitProcess",Type::Void,{status},false);
        for(auto& e:module.externals)if(e.name=="ExitProcess")e.foreign=true;
        os.jump(os.exit);os.finish();
#elif defined(__APPLE__) || defined(__FreeBSD__)
        libc_main(main->token.at);
#endif
        return;
    }
    if(options.source_core)return; // The CLI reports the first unsupported reachable operation.
    if(program.entry_accepts_arguments)(void)layout("string[]");
    // Register all native layouts before executing any user initializers.
    Builder init(*this,"bolt_module_initialize",Type::Void,{Type::Ptr},entry->token.at);init.fn.exported=true;init.frames=false;
    auto abi=init.call("br_abi_version",Type::I32,{},false);
    init.guard(init.cmp("ne",abi,init.integer(Type::I32,native_abi::Version)),"Bolt runtime ABI version mismatch; rebuild compiler and runtime together");
    auto schema=init.call("br_library_schema",Type::I64,{},false);
    init.guard(init.cmp("ne",schema,init.integer(Type::I64,native_abi::library_schema())),"Bolt native library schema mismatch; install the matching runtime");
    for(const auto& [name,l]:layouts){(void)name;init.store(init.address(l.symbol),init.null());}
    for(auto [c,f]:statics){auto slot=init.address(static_name(c->name,f->name));init.instruction(Op::Zero,Type::Void,{slot},layout(f->type).size);init.store(init.address(static_name(c->name,f->name)+"_state"),init.integer(Type::I32,0));}
    for(const auto& [name,l]:layouts){auto t=init.call("br_type",Type::Ptr,{init.context,init.string(name),init.integer(Type::I32,static_cast<std::uint64_t>(l.kind)),init.integer(Type::I64,l.size)});init.store(init.address(l.symbol),t);}
    for(const auto& [name,l]:layouts){auto t=init.type(name);if(l.kind==native_abi::SharedResource){
        const auto* kernel=program.find_class("Bolt.Runtime.ThreadKernel");
        init.call("br_shared_hooks",Type::Void,{init.context,t,init.address(method_symbol(*kernel,*find_method(*kernel,"Retain"))),
            init.address(method_symbol(*kernel,*find_method(*kernel,"Release")))});
    }
    for(const auto& f:l.fields)init.call("br_field",Type::Void,{init.context,t,init.integer(Type::I64,f.offset),init.type(f.type)});
        if(is_optional(name))init.call("br_elements",Type::Void,{init.context,t,init.type(element_type(name)),init.null()});
        if(is_array(name))init.call("br_elements",Type::Void,{init.context,t,init.type(element_type(name)),init.null()});
        if(is_collection(name)){auto args=generic_args(name);init.call("br_elements",Type::Void,{init.context,t,init.type(args[0]),args.size()>1?init.type(args[1]):init.null()});}
        auto* c=program.find_class(name);
        if(c&&!c->base.empty()&&layouts.contains(c->base))init.call("br_base",Type::Void,{init.context,t,init.type(c->base)});
        if(c&&!c->is_native){
            const Method* destructor=nullptr;for(const auto& m:c->methods)if(m.is_destructor&&concrete(m))destructor=&m;
            if(l.marker!=static_cast<std::size_t>(-1)||destructor)
                init.call("br_destructor",Type::Void,{init.context,t,destructor?init.address(method_symbol(*c,*destructor)):init.null(),init.integer(Type::I64,l.marker)});
        }
        if(c&&c->is_native){if(!c->is_static&&!c->is_abstract)init.call("br_initializer",Type::Void,{init.context,t,init.address(init_name(name))});}
        if(c && !c->is_static && !c->is_interface){
            if(!c->is_native && !(l.zero_default && options.optimize_loops && !module.instrument_budget))
                init.call("br_initializer",Type::Void,{init.context,t,init.address(init_name(name))});
            std::set<std::string> seen;
            for(const auto* owner=c;owner;owner=program.find_class(owner->base)){
                for(const auto& method:owner->methods){
                    if(!method.is_virtual || !concrete(method) || !dispatched.contains(method.id) ||
                       !seen.insert(method.id).second || method.is_abstract)continue;
                    init.call("br_virtual",Type::Void,{init.context,t,
                        init.integer(Type::I32,static_cast<std::uint64_t>(virtual_slot(method.id))),
                        init.address(method_symbol(*owner,method))});
                }
            }
        }
    }
    for(const auto& [name,l]:layouts)if(const auto* c=program.find_class(name);c&&!c->is_native&&!c->is_static&&!c->is_interface){
        auto* eq=equality_contract(*c);auto* h=hash_contract(*c);
        if(h && h->is_abstract)h=nullptr;
        if(eq||h)init.call("br_value_contract",Type::Void,{init.context,init.type(name),eq?init.address(method_symbol(*c,*eq)):init.null(),h?init.address(method_symbol(*c,*h)):init.null()});
    }
    const auto* stringable=program.find_class("System.IStringable");const auto* stringify=stringable?find_method(*stringable,"ToString"):nullptr;
    if(stringify)for(const auto& [name,l]:layouts){
        if(is_primitive(name)||is_pointer(name)||program.find_enum(name)||((is_optional(name)||is_collection(name)||(program.find_class(name)&&program.find_class(name)->is_native))&&stringable_type(program,name))) {
            if(dispatched.contains(stringify->id))init.call("br_virtual",Type::Void,{init.context,init.type(name),init.integer(Type::I32,static_cast<std::uint64_t>(virtual_slot(stringify->id))),init.address(mangle("builtin-stringable::"+name))});
            if(const auto* conversion=find_method(*stringable,"@convert.string");conversion&&dispatched.contains(conversion->id))init.call("br_virtual",Type::Void,{init.context,init.type(name),init.integer(Type::I32,static_cast<std::uint64_t>(virtual_slot(conversion->id))),init.address(mangle("builtin-stringable::"+name))});
        }
        else if(const auto* c=program.find_class(name);c&&!c->is_interface&&stringable_type(program,name)){
            const Class* owner=c;const Method* implementation=nullptr;
            while(owner&&!implementation){implementation=method_by_id(*owner,stringify->id);if(!implementation)owner=owner->base.empty()?nullptr:program.find_class(owner->base);}
            if(implementation && !implementation->is_abstract)init.call("br_formatter",Type::Void,{init.context,init.type(name),init.address(method_symbol(*owner,*implementation))});
        }
    }
    if(!thread_entries.empty()){
        Builder drain(*this,"bolt_drain_jobs",Type::Void,{Type::Ptr},entry->token.at);
        drain.frames=false;
        thread_call(drain,"Sweep",Type::Void,{drain.context,drain.integer(Type::I8,1)});
        drain.finish();
        init.call("br_context_cleanup",Type::Void,{init.context,init.address("bolt_drain_jobs")},false);
    }
    if(!program.external_sources.empty()){
        auto bind=[&](const std::string& key,Reg address){
            init.call("br_project_bind",Type::Void,{init.context,init.string(key),address});
        };
        for(const auto& [name,l]:layouts)bind("type:"+name,init.type(name));
        for(const auto& [c,f]:statics){
            const auto storage=static_name(c->name,f->name);
            bind(storage,init.address(storage));bind(storage+"_state",init.address(storage+"_state"));
        }
        for(const auto& [id,index]:virtual_slots){
            const auto symbol=mangle("virtual:"+id);
            const auto slot=static_cast<std::uint32_t>(index);
            module.globals.push_back({symbol,{std::uint8_t(slot),std::uint8_t(slot>>8),std::uint8_t(slot>>16),std::uint8_t(slot>>24)},false,4,{}});
            bind("virtual:"+id,init.address(symbol));
        }
    }
    for(auto [c,f]:statics)init.call(getter(c->name,f->name),Type::Ptr,{init.context},true,false);
    init.finish();
    Builder fini(*this,"bolt_module_finalize",Type::Void,{Type::Ptr},entry->token.at);fini.fn.exported=true;fini.frames=false;if(!thread_entries.empty())fini.call("bolt_drain_jobs",Type::Void,{fini.context},false,false);for(auto i=statics.rbegin();i!=statics.rend();++i){auto [c,f]=*i;fini.call("br_drop",Type::Void,{fini.type(f->type),fini.address(static_name(c->name,f->name))},false);fini.store(fini.address(static_name(c->name,f->name)+"_state"),fini.integer(Type::I32,0));}fini.finish();
    Builder run(*this,"bolt_entry",Type::I32,{Type::Ptr},main->token.at);run.fn.exported=true;run.frames=false;run.return_slot=run.alloc(4);std::vector<Reg> arguments{run.context};
    const Reg task=main->is_async?run.temp(main->type):0;
    if(task)arguments.push_back(task);
    if(program.entry_accepts_arguments){auto array=run.temp("string[]");run.call("br_arguments",Type::Void,{run.context,run.site(),run.type("string[]"),array});arguments.push_back(array);}
    auto result=run.call(method_symbol(*entry,*main),result_type(*main),std::move(arguments),true,false);
    if(task){
        const auto& owner=*program.find_class(main->type);
        auto value=run.invoke(owner,*find_method(owner,"GetResult"),task,{},0,false);
        if(value.type!="void")run.store(run.return_slot,value.value);
    }else if(main->type!="void")run.store(run.return_slot,result);
    run.finish();
    // A standard main is retained for toolchain-independent direct linker drivers.
    Builder start(*this,"bolt_program_entry",Type::I32,{Type::I32,Type::Ptr},main->token.at);start.fn.exported=true;start.frames=false;start.return_slot=start.alloc(4);start.context=start.call("br_create",Type::Ptr,{start.integer(Type::I64,0),start.integer(Type::I32,0)},false);
    auto okay=start.block(),no_context=start.block();start.branch(start.cmp("ne",start.context,start.null()),okay,no_context);
    start.current=no_context;auto failure_status=start.call("br_finish",Type::I32,{start.context,start.integer(Type::I32,1)},false);Instruction failed_return;failed_return.op=Op::Return;failed_return.args={failure_status};start.emit(failed_return);start.current=okay;
    if(program.entry_accepts_arguments)start.call("br_process_arguments",Type::Void,{start.context,1,2},true);
    start.call("bolt_module_initialize",Type::Void,{start.context},true,false);auto rc=start.call("bolt_entry",Type::I32,{start.context},false,false);start.store(start.return_slot,rc);start.jump(start.exit);
    start.current=start.error;start.jump(start.exit);start.current=start.exit;start.call("bolt_module_finalize",Type::Void,{start.context},false,false);auto status=start.call("br_finish",Type::I32,{start.context,start.load(Type::I32,start.return_slot)},false);Instruction ret;ret.op=Op::Return;ret.args={status};start.emit(ret);auto& code=start.fn.blocks[0].code;code.insert(code.begin(),start.prologue.begin(),start.prologue.end());module.functions.push_back(std::move(start.fn));
#ifdef _WIN32
    Builder os(*this,"bolt_start",Type::Void,{},main->token.at);os.fn.exported=true;os.frames=false;auto s=os.call("bolt_program_entry",Type::I32,{os.integer(Type::I32,0xffffffff),os.null()},false,false);os.call("br_process_exit",Type::Void,{s},false);os.jump(os.exit);os.finish();
#elif defined(__APPLE__) || defined(__FreeBSD__)
    libc_main(main->token.at);
#endif
}

} // namespace bolt::ir::lowering

namespace bolt::ir {

// An edit borrows the original type descriptors and dispatch slots. Its
// literal strings and diagnostic sites live in the new object's data section.
Module lower_methods(const Program& p,const Class& owner,const std::vector<Method*>& methods,const MethodEnvironment& env) {
    lowering::Lower lower(p,env.options);
    lower.layouts=env.layouts;
    lower.virtual_slots=env.virtual_slots;
    for(auto* m:methods) lower.method(owner,*m);
    if(lower.layouts.size()!=env.layouts.size()) {
        // A new scalar can live entirely in registers. Drop its uninitialized
        // descriptor so any actual metadata reference fails import validation.
        std::unordered_set<std::string> descriptors;
        for(const auto& [name,layout]:lower.layouts) if(!env.layouts.contains(name)) {
            if(!is_numeric(name) && name!="bool")throw std::runtime_error("Method edit needs a new type layout");
            descriptors.insert(layout.symbol);
        }
        std::erase_if(lower.module.globals,[&](const Global& g){return descriptors.contains(g.name);});
    }
    if(lower.virtual_slots!=env.virtual_slots ||
       !lower.worker_callbacks.empty() || !lower.thread_entries.empty())
        throw std::runtime_error("Method edit needs additional runtime metadata");
    auto& module=lower.module;
    std::unordered_set<std::string> local;
    for(const auto& f:module.functions) local.insert(f.name);
    for(const auto& g:module.globals) local.insert(g.name);
    for(const auto& e:module.externals) local.insert(e.name);
    auto import=[&](const std::string& name) {
        if(!local.insert(name).second) return;
        if(auto i=env.signatures.find(name);i!=env.signatures.end()) module.externals.push_back(i->second);
        else if(env.globals.contains(name)) module.externals.push_back({name,Type::Ptr,{}});
        else throw std::runtime_error("Method edit requires an uncompiled symbol: "+name);
    };
    for(const auto& f:module.functions) for(const auto& b:f.blocks) for(const auto& i:b.code)
        if(i.op==Op::Call || i.op==Op::Address) import(i.text);
    for(const auto& g:module.globals) for(const auto& reloc:g.relocations) import(reloc.symbol);
    return std::move(module);
}
Module lower(const Program& p,LowerOptions o){
    if(auto reason=native_abi::direct_requirement(p);!reason.empty())throw std::runtime_error("The direct native/JIT backend does not yet lower '"+reason+"'; use the C++ native backend.");
    return lowering::Lower(p,o).run();
}

} // namespace bolt::ir
