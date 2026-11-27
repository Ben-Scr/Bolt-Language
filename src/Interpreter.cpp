#include <bolt/native/Jit.hpp>
#include <bolt/API.hpp>
#include "RuntimeInternal.hpp"
#include <bolt/Path.hpp>
#include <bolt/Frontend.hpp>
#include <bolt/Std.hpp>
#include <bolt/Platform.hpp>
#include <bolt/SecureRandom.hpp>
#include <array>
#include <bolt/library/Strings.hpp>
#include <unordered_set>
#include <bolt/Types.hpp>
#include <algorithm>
#include <deque>
#include <iostream>
#include <utility>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
namespace bolt {
    namespace {
        bool platform_console(const Class& c) {
            if(!c.token.at.file.starts_with("<bolt-stdlib>"))return false;
            return c.name=="Bolt.Runtime.WindowsConsole"||c.name=="Bolt.Runtime.LinuxConsole"||c.name=="Bolt.Runtime.MacOSConsole"||c.name=="Bolt.Runtime.HostConsole";
        }
        bool platform_datetime(const Class& c) {
            if(!c.token.at.file.starts_with("<bolt-stdlib>"))return false;
            return c.name=="Bolt.Runtime.WindowsDateTime"||c.name=="Bolt.Runtime.LinuxDateTime"||c.name=="Bolt.Runtime.MacOSDateTime"||c.name=="Bolt.Runtime.HostDateTime";
        }
        bool desktop_clock_or_thread_ops(const Class& c) {
            return (c.name=="Bolt.Runtime.Clock" && c.token.at.file=="<bolt-stdlib>/System/DesktopClock.bolt") ||
                   (c.name=="Bolt.Runtime.ThreadOps" && (c.token.at.file=="<bolt-stdlib>/System/DesktopThreadOps.bolt" || c.token.at.file=="<bolt-stdlib>/System/HostedThreadOps.bolt"));
        }
        bool windows_counter_frequency(const Class& c) {
            return c.name=="Bolt.Runtime.WindowsCounterFrequency" && c.token.at.file=="<bolt-stdlib>/System/DesktopClock.bolt";
        }
        bool crypto_random_source(const std::string& file) {
            return file=="<bolt-stdlib>/System/Security/Cryptography/RandomNumberGenerator.bolt";
        }
        bool random_word(const Class& c) {
            return c.name=="Bolt.Runtime.RandomWord" && c.token.at.file=="<bolt-stdlib>/System/RandomState.bolt";
        }
        bool platform_api_source(const std::string& file) {
            if(!file.starts_with("<bolt-stdlib>"))return false;
            return file.ends_with("/System/WindowsApi.bolt")||file.ends_with("/System/LinuxApi.bolt")||
                   file.ends_with("/System/MacOSApi.bolt")||file.ends_with("/System/PosixThreadApi.bolt")||
                   file.ends_with("/System/ThreadLocalApi.bolt")||file.ends_with("/System/PlatformRandomApi.bolt");
        }
        // The interpreter is itself a C++ execution engine and cannot execute the
        // raw platform FFI used by compiled Console. Keep only its host-stream
        // Clear compatibility here; this is not a Bolt runtime Console ABI.
        void write_interpreter_console(std::ostream& output, std::string_view text, bool newline) {
            output << text;
            if (newline) output.put('\n');
            // Native console calls write directly to the OS. Keep interpreted
            // console output in order when control moves between both modes.
            if (&output == &std::cout) output.flush();
        }
        void clear_interpreter_display(std::ostream& output) {
            if(&output!=&std::cout)return;
#ifdef _WIN32
            const auto handle=GetStdHandle(STD_OUTPUT_HANDLE);DWORD mode=0;
            if(!GetConsoleMode(handle,&mode))return;
            CONSOLE_SCREEN_BUFFER_INFO info{};
            if(!GetConsoleScreenBufferInfo(handle,&info))return;
            const COORD start{0,0};DWORD written=0;
            const auto cells=static_cast<DWORD>(info.dwSize.X)*static_cast<DWORD>(info.dwSize.Y);
            FillConsoleOutputCharacterW(handle,L' ',cells,start,&written);
            FillConsoleOutputAttribute(handle,info.wAttributes,cells,start,&written);
            SetConsoleCursorPosition(handle,start);
#else
            if(!isatty(STDOUT_FILENO))return;
            const auto* term=std::getenv("TERM");
            if(!term||std::string_view(term)=="dumb")return;
            output<<"\x1b[2J\x1b[H"<<std::flush;
#endif
        }
        struct State {
            std::size_t calls=0;
            detail::SharedExecutionBudget budget;
            explicit State(std::size_t max_steps):budget(std::make_shared<detail::ExecutionBudget>(max_steps)) {}
            explicit State(detail::SharedExecutionBudget shared):budget(std::move(shared)) {}
        };
        struct CounterGuard {
            std::size_t& count;
            explicit CounterGuard(std::size_t& c):count(c) {
                ++count;
            }
            ~CounterGuard() {
                --count;
            }
        };
        struct Slot {
            Value value;
            Value* alias=nullptr;
            std::weak_ptr<char> alias_lifetime;
        };
        bool native_exception(const Class& type) {
            return type.is_native && type.token.at.file=="<bolt-stdlib>/System/Exceptions.bolt";
        }
        void set_exception_fields(Value& value, const std::string& message) {
            auto& fields=value.object().fields;
            fields.insert_or_assign("Message",Value(message));
            fields.insert_or_assign("SourceFile",Value(""));
            fields.insert_or_assign("SourceLine",Value(0));
            fields.insert_or_assign("SourceColumn",Value(0));
            fields.insert_or_assign("StackTrace",Value(""));
        }
        // Language handlers catch only these exceptions. Compiler diagnostics,
        // step budgets and interpreter depth limits propagate to the host.
        class InterpretedException : public Error {
        public:
            Value payload;
            InterpretedException(Value value, const Location& at)
                :Error(runtime_diagnostic(at,value.object().fields.at("Message").as_string())),payload(std::move(value)) {}
            void update_source() {
                auto& fields=payload.object().fields;
                if (fields.at("SourceFile").as_string().empty()) {
                    fields.at("SourceFile")=Value(location.file);
                    fields.at("SourceLine")=Value(location.line);
                    fields.at("SourceColumn")=Value(location.column);
                }
                std::string trace;
                for (const auto& frame:diagnostic.frames)
                    trace+="  at "+frame.message+" at "+frame.location.file+":"+std::to_string(frame.location.line)+"\n";
                fields.at("StackTrace")=Value(std::move(trace));
            }
        };
        InterpretedException language_fault(const std::exception& error,const Location& at) {
            auto convert=[&](const library::Exception& original) {
                auto value=Value::object_value(std::string("System.")+original.TypeName());
                set_exception_fields(value,original.what());
                return InterpretedException(std::move(value),at);
            };
            if (auto* original=dynamic_cast<const library::Exception*>(&error)) return convert(*original);
            try { library::translate_error(error,at); }
            catch (const library::Exception& translated) { return convert(translated); }
        }

        struct Frame {
            const Class* owner=nullptr;
            const Method* method=nullptr;
            Value* self=nullptr;
            // Scope growth must not copy live slots: Arena locals uniquely own
            // their storage and borrowed aliases keep each slot's address.
            std::deque<std::unordered_map<std::string,Slot>> scopes;
        };
        struct ScopeGuard {
            Frame& f;
            explicit ScopeGuard(Frame& x):f(x) {
                f.scopes.emplace_back();
            }
            ~ScopeGuard() {
                f.scopes.pop_back();
            }
        };
        struct Flow {
            enum class Kind {
                Normal,Return,Break,Continue
            }
            kind=Kind::Normal;
            Value value;
        };
        std::string statement_program(std::string_view source) {
            return "import Math\nimport Random\nimport Type\nimport System.Collections.Generic\nstatic class Program {public: void Main(){\n"+std::string(source)+"\n}}";
        }
        template<class T> T scalar(const Value& value) {
            return std::visit([](const auto& v)->T {
                using U=std::decay_t<decltype(v)>;if constexpr(std::is_arithmetic_v<U>&&!std::is_same_v<U,bool>)return native::cast<T>(v);else throw std::runtime_error("numeric value required");
            },value.data);
        }
        class Engine {
            std::unordered_map<std::string,int> static_state;
            std::unordered_map<Value*,std::unique_ptr<Value>> heap;
            bool derives(const std::string& from,const std::string& to)const {
                return derives_from(program,from,to);
            }
            const Method* default_constructor(const Class& c)const {
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
            void copy_fields(const Class& c,Value& to,const Value& from) {
                if(!c.base.empty())copy_fields(*program.find_class(c.base),to,from);
                for(const auto& f:c.fields)if(!f.is_static)to.object().fields.emplace(f.name,from.object().fields.at(f.name));
            }
            void initialize_fields(const Class& c,Value& self,Frame& frame) {
                self.object().dispatch_type=c.name;
                for(const auto& f:c.fields)if(!f.is_static) {
                    const Expr* member=nullptr;
                    if(frame.method)for(const auto& [name,expr]:frame.method->field_initializers)if(name==f.name){member=expr.get();break;}
                    const auto* init=member?member:f.initializer.get();
                    auto value=init?convert(f.type,evaluate(*init,frame)):make(f.type);
                    self.object().fields.emplace(f.name,std::move(value));
                }
            }
            void initialize(const Class& c,Value& self,const Method* constructor,std::vector<Value> args) {
                if(constructor) {
                    (void)invoke(c,*constructor,&self,std::move(args));
                    return;
                }
                if(!c.base.empty()) {
                    const auto& base=*program.find_class(c.base);
                    initialize(base,self,default_constructor(base), {
                    });
                }
                Frame frame;
                frame.owner=&c;
                frame.self=&self;
                initialize_fields(c,self,frame);
            }
            std::vector<Value> arguments(const std::vector<ExprPtr>& source,std::size_t begin,const Method* method,Frame& frame) {
                std::vector<Value> args;
                args.reserve(source.size()-begin);
                for(std::size_t i=begin;i<source.size();++i) {
                    const auto* p=method?&method->parameters[i-begin]:nullptr;
                    if(p&&is_reference(p->type)&&method->operator_symbol.empty()&&source[i]->lvalue)args.push_back(Value::pointer_value(p->type,&reference(*source[i],frame)));
                    else {
                        auto v=evaluate(*source[i],frame);
                        args.push_back(p?convert(referred(p->type),v):std::move(v));
                    }
                }
                return args;
            }
            Value& local(Frame& f,const std::string& n) {
                for(auto it=f.scopes.rbegin();it!=f.scopes.rend();++it) {
                    auto found=it->find(n);
                    if(found!=it->end()){
                        auto& slot=found->second;
                        if(slot.alias){
                            if(slot.alias_lifetime.expired())throw std::runtime_error("dangling reference");
                            return *slot.alias;
                        }
                        return slot.value;
                    }
                }
                throw std::runtime_error("internal error: local not found: "+n);
            }
            Value& static_value(const std::string& owner,const std::string& name) {
                auto key=owner+"."+name;
                if(static_state[key]==2)return statics.at(key);
                if(static_state[key]==1)throw std::runtime_error("cyclic static initialization");
                static_state[key]=1;
                const auto* c=program.find_class(owner);
                const auto* f=find_field(*c,name);
                Frame frame;
                frame.owner=c;
                auto v=f->initializer?convert(f->type,evaluate(*f->initializer,frame)):make(f->type);
                auto [it,inserted]=statics.emplace(key,std::move(v));
                (void)inserted;
                static_state[key]=2;
                return it->second;
            }
            Value& receiver(const Expr& e,Frame& frame) {
                if(e.arrow)return evaluate(*e.args[0],frame).dereference();
                return reference(*e.args[0],frame);
            }
            std::size_t array_index(const Expr& e,Frame& frame,const Value& base) {
                if(e.args.size()==2) {
                    const auto argument=evaluate(*e.args[1],frame);
                    const auto length=base.collection().values.size();
                    return is_unsigned(argument.type)
                        ? native::checked_index(scalar<std::uint64_t>(argument),length)
                        : native::checked_index(argument.as_int(),length);
                }
                std::vector<Value> arguments;
                arguments.reserve(e.args.size()-1);
                for(std::size_t axis=1;axis<e.args.size();++axis)arguments.push_back(evaluate(*e.args[axis],frame));
                const auto& collection=base.collection();
                std::vector<std::size_t> indices;
                indices.reserve(arguments.size());
                for(const auto& argument:arguments)
                    indices.push_back(is_unsigned(argument.type)?native::checked_size(scalar<std::uint64_t>(argument)):native::checked_size(argument.as_int()));
                return native::array_offset(collection.dimensions,indices);
            }
            Value& index(const Expr& e,Frame& frame) {
                auto& base=reference(*e.args[0],frame);
                if(is_array(base.type)) {const auto offset=array_index(e,frame,base);return base.collection().values.at(offset);}
                auto key=evaluate(*e.args[1],frame);
                auto& c=base.collection();
                if(collection_kind(base.type)=="Dictionary") {
                    key=convert(generic_args(base.type)[0],key);
                    for(auto& [k,v]:c.entries)if(equal_values(k,key))return v;
                    if(!e.is_static)throw std::runtime_error("dictionary key not found");
                    c.entries.emplace_back(std::move(key),make(generic_args(base.type)[1]));
                    return c.entries.back().second;
                }
                auto i=native::checked_index(key.as_int(),c.values.size());
                return c.values[i];
            }
            std::pair<const Class*,const Method*> dispatch(const Class& owner,const Method& method,Value* self)const {
                if(!self||!method.is_virtual||method.is_constructor)return {
                    &owner,&method
                };
                self=&unbox(*self);
                if(!program.find_class(self->type))return {&owner,&method};
                auto t=self->object().dispatch_type;
                while(!t.empty()) {
                    auto* c=program.find_class(t);
                    if(auto* m=method_by_id(*c,method.id))return {
                        c,m
                    };
                    t=c->base;
                }
                return {
                    &owner,&method
                };
            }
            Value collection_call(const Expr& e,Frame& frame) {
                const auto& target=*e.args[0];
                Value temporary;
                Value* object=nullptr;
                if(target.arrow) {
                    temporary=evaluate(*target.args[0],frame);
                    object=&temporary.dereference();
                } else if(target.args[0]->lvalue)object=&reference(*target.args[0],frame);
                else {
                    temporary=evaluate(*target.args[0],frame);
                    object=&temporary;
                }
                const std::weak_ptr<char> receiver_lifetime=object->lifetime;
                const auto kind=collection_kind(e.owner);
                const auto types=generic_args(e.owner);
                const auto& name=e.text;
                std::vector<Value> args;
                for(std::size_t i=1;i<e.args.size();++i) {
                    bool ref=(name=="Swap"&&i==1)||(name=="TryGetValue"&&i==2)||(name=="TryDequeue"&&i==1);
                    args.push_back(ref?Value::pointer_value("@reference",&reference(*e.args[i],frame)):evaluate(*e.args[i],frame));
                }
                if(receiver_lifetime.expired())throw std::runtime_error("dangling collection receiver");
                auto& c=object->collection();
                if(kind=="List"&&(name=="AppendRange"||name=="AppendBuffer")){
                    const bool buffer=name=="AppendBuffer";
                    auto& input=args[buffer?2:0].collection().values;
                    auto start=args[buffer?0:1].as_int(),count=args[buffer?1:2].as_int();
                    if(start<0||count<0||static_cast<std::uint64_t>(start)>input.size()||static_cast<std::uint64_t>(count)>input.size()-start)throw std::runtime_error("invalid buffer source range");
                    if (buffer) {
                        const auto required = c.values.size() + static_cast<std::size_t>(count);
                        if (required > c.values.capacity())
                            c.values.reserve(memory::detail::buffer_growth_capacity(c.values.capacity(), required));
                    }
                    c.values.insert(c.values.end(),input.begin()+start,input.begin()+start+count);
                    if(count)++object->range_version;
                    return {};
                }
                if(kind=="List"&&name=="Swap"){
                    auto& other_value=args.at(0).dereference();
                    auto& other=other_value.collection();
                    ++object->range_version;
                    if(object==&other_value)return {};
                    ++other_value.range_version;
                    if(c.values.get_allocator()==other.values.get_allocator())c.values.swap(other.values);
                    else {auto first=c.values,second=other.values;c.values.assign(second.begin(),second.end());other.values.assign(first.begin(),first.end());}
                    return {};
                }
                if(kind=="List"&&name=="Reserve"){
                    const auto n=native::checked_size(args[0].as_int());
                    if(n>c.values.capacity()){c.values.reserve(n);++object->range_version;}
                    return {};
                }
                if(kind=="List"&&name=="Truncate"){
                    const auto n=native::checked_size(args[0].as_int());if(n>c.values.size())throw std::runtime_error("cannot grow live buffer length with Truncate");if(n!=c.values.size()){c.values.erase(c.values.begin()+n,c.values.end());++object->range_version;}
                    return {};
                }
                if(kind=="List"&&name=="ToArray"){
                    auto array=Value::collection_value(types[0]+"[]");array.collection().values=c.values;array.collection().dimensions={c.values.size()};return array;
                }
                if(name!="Contains"&&name!="ContainsKey"&&name!="IndexOf"&&name!="Peek"&&name!="TryGetValue")++object->range_version;
                if(name=="Clear") {
                    c.values.clear();
                    c.entries.clear();
                    return {
                    };
                }
                if(kind=="Dictionary") {
                    auto key=convert(types[0],args[0]);
                    auto found=std::find_if(c.entries.begin(),c.entries.end(),[&](const auto& p) {
                        return equal_values(p.first,key);
                    });
                    if(name=="ContainsKey")return Value(found!=c.entries.end());
                    if(name=="Remove") {
                        bool result=found!=c.entries.end();
                        if(result)c.entries.erase(found);
                        return Value(result);
                    }
                    if(name=="TryGetValue") {
                        if(found==c.entries.end())return Value(false);
                        assign_in_place(args[1].dereference(),found->second);
                        return Value(true);
                    }
                    if(name=="Add"||name=="Set") {
                        auto v=convert(types[1],args[1]);
                        if(found!=c.entries.end()) {
                            if(name=="Add")throw std::runtime_error("duplicate dictionary key");
                            assign_in_place(found->second,v);
                        }else c.entries.emplace_back(std::move(key),std::move(v));
                        return {
                        };
                    }
                }
                if(name=="Peek"||name=="Dequeue"||name=="TryDequeue") {
                    if(c.values.empty()) {
                        if(name=="TryDequeue")return Value(false);
                        throw std::runtime_error("queue is empty");
                    }
                    Value v=c.values.front();
                    if(name!="Peek")c.values.erase(c.values.begin());
                    if(name=="TryDequeue") {
                        assign_in_place(args[0].dereference(),v);
                        return Value(true);
                    }
                    return v;
                }
                if(name=="RemoveAt") {
                    c.values.erase(c.values.begin()+static_cast<std::ptrdiff_t>(native::checked_index(args[0].as_int(),c.values.size())));
                    return {
                    };
                }
                if(name=="Insert") {
                    auto i=native::checked_index(args[0].as_int(),c.values.size(),true);
                    auto v=convert(types[0],args[1]);
                    c.values.insert(c.values.begin()+static_cast<std::ptrdiff_t>(i),std::move(v));
                    return {
                    };
                }
                auto v=convert(types[0],args[0]);
                auto found=std::find_if(c.values.begin(),c.values.end(),[&](const auto& x) {
                    return equal_values(x,v);
                });
                if(name=="Contains")return Value(found!=c.values.end());
                if(name=="IndexOf")return Value(found==c.values.end()?std::int32_t {
                    -1
                }
                :native::cast<std::int32_t>(std::distance(c.values.begin(),found)));
                if(name=="Remove") {
                    bool result=found!=c.values.end();
                    if(result)c.values.erase(found);
                    return Value(result);
                }
                if(name=="Add"||name=="Enqueue") {
                    bool added=kind!="HashSet"||found==c.values.end();
                    if(added)c.values.push_back(std::move(v));
                    return kind=="HashSet"?Value(added):Value {
                    };
                }
                throw std::runtime_error("unknown collection operation");
            }
            Value builtin(const Expr& e,Frame& frame) {
                if(e.owner=="String") {
                    std::vector<Value> args;
                    if(!e.is_static)args.push_back(evaluate(*e.args[0]->args[0],frame));
                    auto params=arguments(e.args,1,nullptr,frame);for(auto& v:params)args.push_back(std::move(v));
                    using S=bolt::library::Strings;
                    auto text=[&](std::size_t i){return native::String(args.at(i).as_string());};
                    auto num=[&](std::size_t i){return scalar<std::int32_t>(args.at(i));};
                    auto chr=[&](std::size_t i){return scalar<char32_t>(args.at(i));};
                    auto string_array=[&](const auto& array){auto result=Value::collection_value("string[]");for(int i=0;i<array.Count();++i)result.collection().values.emplace_back(std::string(array.At(i)));return result;};
                    if(e.text=="Join"){auto parts=native::Array<native::String>::Create(static_cast<std::int32_t>(args.at(1).collection().values.size()));for(int i=0;i<parts.Count();++i)parts.At(i)=native::String(args.at(1).collection().values[i].as_string());return Value(std::string(S::Join(text(0),parts)));}
                    if(e.text=="IsNullOrEmpty")return Value(S::IsNullOrEmpty(text(0)));
                    if(e.text=="IsNullOrWhiteSpace")return Value(S::IsNullOrWhiteSpace(text(0)));
                    if(e.text=="Equals")return Value(S::Equals(text(0),text(1)));
                    if(e.text=="Compare")return Value(S::Compare(text(0),text(1)));
                    if(e.text=="Contains")return Value(args[1].type=="char"?S::Contains(text(0),chr(1)):S::Contains(text(0),text(1)));
                    if(e.text=="StartsWith")return Value(S::StartsWith(text(0),text(1)));
                    if(e.text=="EndsWith")return Value(S::EndsWith(text(0),text(1)));
                    if(e.text=="IndexOf")return Value(args[1].type=="char"?S::IndexOf(text(0),chr(1),args.size()>2?num(2):0):S::IndexOf(text(0),text(1),args.size()>2?num(2):0));
                    if(e.text=="LastIndexOf")return Value(args[1].type=="char"?S::LastIndexOf(text(0),chr(1)):S::LastIndexOf(text(0),text(1)));
                    if(e.text=="Substring")return Value(std::string(args.size()>2?S::Substring(text(0),num(1),num(2)):S::Substring(text(0),num(1))));
                    if(e.text=="Remove")return Value(std::string(args.size()>2?S::Remove(text(0),num(1),num(2)):S::Remove(text(0),num(1))));
                    if(e.text=="Insert")return Value(std::string(S::Insert(text(0),num(1),text(2))));
                    if(e.text=="Replace")return Value(std::string(args[1].type=="char"?S::Replace(text(0),chr(1),chr(2)):S::Replace(text(0),text(1),text(2))));
                    if(e.text=="Trim")return Value(std::string(S::Trim(text(0))));
                    if(e.text=="TrimStart")return Value(std::string(S::TrimStart(text(0))));
                    if(e.text=="TrimEnd")return Value(std::string(S::TrimEnd(text(0))));
                    if(e.text=="ToUpper")return Value(std::string(S::ToUpper(text(0))));
                    if(e.text=="ToLower")return Value(std::string(S::ToLower(text(0))));
                    if(e.text=="PadLeft")return Value(std::string(S::PadLeft(text(0),num(1),args.size()>2?chr(2):U' ')));
                    if(e.text=="PadRight")return Value(std::string(S::PadRight(text(0),num(1),args.size()>2?chr(2):U' ')));
                    if(e.text=="Split")return string_array(args[1].type=="char"?S::Split(text(0),chr(1),args.size()>2&&args[2].as_bool()):S::Split(text(0),text(1),args.size()>2&&args[2].as_bool()));
                    if(e.text=="ToUtf8"){auto data=S::ToUtf8(text(0));auto result=Value::collection_value("byte[]");for(int i=0;i<data.Count();++i)result.collection().values.emplace_back(data.At(i));return result;}
                    if(e.text=="ToCharArray"){auto data=S::ToCharArray(text(0));auto result=Value::collection_value("char[]");for(int i=0;i<data.Count();++i)result.collection().values.push_back(Value::signed_integer("char",data.At(i)));return result;}
                    if(e.text=="FromUtf8"){auto data=native::Array<std::uint8_t>::Create(static_cast<std::int32_t>(args.at(0).collection().values.size()));for(int i=0;i<data.Count();++i)data.At(i)=scalar<std::uint8_t>(args.at(0).collection().values[i]);return Value(std::string(S::FromUtf8(data)));}
                    throw std::runtime_error("unsupported interpreter string operation: "+e.text);
                }
                if(e.owner=="Array"&&e.text=="GetLength") {
                    const auto& target=*e.args[0];
                    Value temporary;
                    Value* object=nullptr;
                    if(target.arrow) {temporary=evaluate(*target.args[0],frame);object=&temporary.dereference();}
                    else if(target.args[0]->lvalue)object=&reference(*target.args[0],frame);
                    else {temporary=evaluate(*target.args[0],frame);object=&temporary;}
                    const auto dimension=evaluate(*e.args[1],frame).as_int();
                    const auto rank=array_rank(object->type);
                    const auto axis=native::checked_index(dimension,rank);
                    const auto& collection=object->collection();
                    return Value(native::cast<std::int32_t>(rank==1?collection.values.size():collection.dimensions[axis]));
                }
                auto args=arguments(e.args,1,nullptr,frame);
                if(e.owner=="Arena") {
                    if(e.text=="Fixed"||e.text=="Growable") {
                        const auto initial=scalar<std::size_t>(args.at(0));
                        const auto limit=e.text=="Fixed"?initial:scalar<std::size_t>(args.at(1));
                        return Value::arena_value(initial,limit);
                    }
                    auto& arena=reference(*e.args[0]->args[0],frame).arena();
                    if(e.text=="Reset")arena.reset();
                    else if(e.text=="Dispose")arena.dispose();
                    else throw std::runtime_error("unknown Arena operation");
                    return {};
                }
                if(e.owner=="Intrinsic"&&e.text=="Reinterpret") {
                    auto underlying=[&](const std::string& t) { const auto* en=program.find_enum(t);return en?en->underlying:t; };
                    auto result=reinterpret_value(underlying(e.type),underlying(args[0].type),args[0]);
                    result.type=e.type;
                    result.bit_pattern=true;
                    return result;
                }
                if(e.owner=="Console") {
                    if(e.text=="ReadLine") {
                        std::string s;
                        std::getline(input,s);
                        return Value(s);
                    }
                    write_interpreter_console(output, args.empty() ? std::string{} : format(args[0]), e.text=="WriteLine");
                    return {
                    };
                }
                if(e.owner=="Runtime")return Value(nested(e.text,args[0].as_string(),e.token.at));
                if(e.owner=="Type")return Value(e.type_args[0]);
                throw std::runtime_error("unsupported builtin call: "+e.owner+"."+e.text);
            }
            Value call_expression(const Expr& e,Frame& frame) {
                if(e.binding==Expr::Binding::Builtin)return builtin(e,frame);
                if(e.binding==Expr::Binding::Collection)return collection_call(e,frame);
                if(e.binding==Expr::Binding::Cast) {
                    auto v=evaluate(*e.args[1],frame);
                    return e.type=="string"?Value(format(std::move(v))):convert(e.type,v);
                }
                if(e.binding==Expr::Binding::Construct) {
                    const auto* c=program.find_class(e.type);
                    const auto* m=c&&!e.resolved.empty()?method_by_id(*c,e.resolved):nullptr;
                    return construct(e.type,arguments(e.args,1,m,frame),m);
                }
                const auto& target=*e.args[0];
                const auto& owner=*program.find_class(e.owner);
                const auto& method=*method_by_id(owner,e.resolved);
                Value temporary;
                Value* self=nullptr;
                if(!method.is_static) {
                    if(target.kind==Expr::Kind::Member) {
                        const auto& base=*target.args[0];
                        if(target.arrow)self=&evaluate(base,frame).dereference();
                        else if(base.lvalue)self=&reference(base,frame);
                        else {
                            temporary=evaluate(base,frame);
                            self=&temporary;
                        }
                    } else self=frame.self;
                }
                auto args=arguments(e.args,1,&method,frame);
                auto [actual_owner,actual_method]=dispatch(owner,method,self);
                return invoke(*actual_owner,*actual_method,self,std::move(args));
            }
            public: Program program;
            ExecutionOptions options;
            std::istream& input;
            std::ostream& output;
            std::shared_ptr<State> state;
            std::unordered_map<std::string,Value> statics;
            Engine(Program p,ExecutionOptions opts,std::istream& in,std::ostream& out,std::shared_ptr<State> shared= {
            }):program(std::move(p)),options(opts),input(in),output(out),state(shared?std::move(shared):std::make_shared<State>(opts.max_steps)) {
                if(auto requirement=interpreter_requirement(program);!requirement.empty())throw std::runtime_error("The interpreter does not support "+requirement+"; use bolt run --native or --jit.");
                for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native)for(const auto& f:c.fields)if(f.is_static)(void)static_value(c.name,f.name);
            }
            void reset_budget() {
                state->calls=0;
                state->budget->reset();
            }
            void tick(const Location& at) {
                if(!state->budget->consume_step())throw Error(at,"runtime instruction budget exceeded", "BLT3000");
            }
            Value convert(const std::string& t,const Value& v) {
                if(is_primitive(t))return convert_value(t,v);
                if(is_optional(t)) {
                    if(v.type=="null"||(is_optional(v.type)&&!v.has_value()))return Value::optional_value(t);
                    auto payload=convert(element_type(t),is_optional(v.type)?v.optional_payload():v);
                    return Value::optional_value(t,&payload);
                }
                if(is_interface(program,t)) {
                    if(t==v.type&&capture_cell_type(t))return v;
                    const auto& raw=unbox(v);
                    if(!derives(raw.type,t)&&!(t=="System.IStringable"&&stringable_type(program,raw.type)))throw std::runtime_error("value '"+raw.type+"' does not implement '"+t+"'");
                    auto result=Value::object_value(t);result.object().interface_box=true;
                    if(capture_cell_type(t)){result.object().capture_cell=std::make_shared<Value>(raw);return result;}
                    result.object().fields.emplace("@value",raw);return result;
                }
                if(const auto* en=program.find_enum(t)) {
                    auto value=convert_value(en->underlying,v);
                    value.type=t;
                    return value;
                }
                if(auto* c=program.find_class(t);c&&t!=v.type&&derives(v.type,t)) {
                    auto result=Value::object_value(t);
                    copy_fields(*c,result,v);
                    return result;
                }
                return convert_value(t,v);
            }
            Value make(const std::string& t) {
                if(is_optional(t))return Value::optional_value(t);
                if(is_primitive(t))return default_primitive(t);
                if(is_pointer(t))return Value::pointer_value(t);
                if(auto* en=program.find_enum(t)) {
                    auto v=default_primitive(en->underlying);
                    v.type=t;
                    return v;
                }
                if(is_array(t)||is_collection(t))return Value::collection_value(t);
                return construct(t, {
                },nullptr);
            }
            Value construct(const std::string& type,std::vector<Value> args,const Method* selected) {
                if(is_collection(type))return Value::collection_value(type);
                auto* c=program.find_class(type);
                if(!c||c->is_static||c->is_interface)throw std::runtime_error("cannot instantiate '"+type+"'");
                if(!selected)selected=default_constructor(*c);
                auto result=Value::object_value(type);
                initialize(*c,result,selected,std::move(args));
                return result;
            }
            Value& reference(const Expr& e,Frame& frame) {
                using B=Expr::Binding;
                using K=Expr::Kind;
                if(e.binding==B::Local)return local(frame,e.text);
                if(e.binding==B::This)return *frame.self;
                if(e.binding==B::Field) {
                    if(e.is_static)return static_value(e.owner,e.text);
                    auto& object=e.kind==K::Member?receiver(e,frame):*frame.self;
                    return object.object().fields.at(e.text);
                }
                if(e.kind==K::OptionalValue)return reference(*e.args[0],frame).optional_payload();
                if(e.kind==K::Unary&&e.text=="*")return evaluate(*e.args[0],frame).dereference();
                if(e.kind==K::Index)return index(e,frame);
                if(e.kind==K::Call&&is_reference(e.declared_type))return call_expression(e,frame).dereference();
                throw Error(e.token.at,"internal error: expression is not a reference", "BLT3000");
            }
            std::string format(Value value) {
                if (auto* owned = std::get_if<std::string>(&value.data); owned && value.type == "string")
                    return std::move(*owned);
                auto& raw=unbox(value);
                if(is_optional(raw.type))return raw.has_value()?format(raw.optional_payload()):"null";
                if(is_reference(raw.type))return format(raw.dereference());
                if(const auto formatter=primitive_formatter(raw.type);formatter.owner) {
                    const auto* cls=program.find_class(formatter.owner);
                    const auto* method=cls?find_method(*cls,formatter.method):nullptr;
                    if(method && method->analyzed)return invoke(*cls,*method,nullptr,{raw}).as_string();
                }
                if(is_primitive(raw.type)||is_pointer(raw.type)||program.find_enum(raw.type))return raw.to_string();
                if(is_collection(raw.type)&&stringable_type(program,raw.type)) {
                    const auto kind=collection_kind(raw.type);
                    const bool unordered=kind=="HashSet"||kind=="Dictionary";
                    std::vector<std::string> parts;
                    if(kind=="Dictionary") {
                        for(const auto& [key,entry]:raw.collection().entries) {
                            auto part=format(key);
                            part+=": "+format(entry);
                            parts.push_back(std::move(part));
                        }
                    } else {
                        for(const auto& entry:raw.collection().values)parts.push_back(format(entry));
                    }
                    if(unordered)std::sort(parts.begin(),parts.end());
                    std::string result=unordered?"{":"[";
                    for(std::size_t i=0;i<parts.size();++i) {
                        if(i)result+=", ";
                        result+=parts[i];
                    }
                    return result+(unordered?"}":"]");
                }
                const auto* iface=program.find_class("System.IStringable");
                if(!iface||!stringable_type(program,raw.type))throw std::runtime_error("type '"+raw.type+"' does not implement IStringable");
                const auto* m=find_method(*iface,"ToString");
                auto [owner,actual]=dispatch(*iface,*m,&raw);
                return invoke(*owner,*actual,&raw,{}).as_string();
            }
            Value arithmetic(const std::string& op,const Value& a,const Value& b) {
                if(op=="+"&&(a.type=="string"||b.type=="string"))return Value(format(a)+format(b));
                return numeric_binary(op,(op=="<<"||op==">>")?promoted(a.type):common_numeric(a.type,b.type),a,b);
            }
            bool logical_equal(const Value& a,const Value& b) {
                if(a.type!=b.type)return equal_values(a,b);
                if(a.type=="Arena")return std::get<std::unique_ptr<ValueArena>>(a.data)==std::get<std::unique_ptr<ValueArena>>(b.data);
                if(auto* c=program.find_class(a.type)) {
                    if(auto* method=equality_contract(*c))return invoke(*c,*method,nullptr,{a,b}).as_bool();
                    const auto& x=a.object().fields;const auto& y=b.object().fields;
                    if(x.size()!=y.size())return false;
                    for(const auto& [name,value]:x){auto found=y.find(name);if(found==y.end()||!logical_equal(value,found->second))return false;}return true;
                }
                if(std::holds_alternative<ValueOwned<Collection>>(a.data)) {
                    const auto& x=a.collection();const auto& y=b.collection();
                    if(x.dimensions!=y.dimensions||x.values.size()!=y.values.size())return false;
                    for(std::size_t i=0;i<x.values.size();++i)if(!logical_equal(x.values[i],y.values[i]))return false;return true;
                }
                return equal_values(a,b);
            }
            std::uint64_t logical_hash(const Value& v) {
                if(v.type=="Arena")return std::hash<ValueArena*>{}(std::get<std::unique_ptr<ValueArena>>(v.data).get());
                if(auto* c=program.find_class(v.type)) {
                    if(auto* method=hash_contract(*c))return scalar<std::uint64_t>(invoke(*c,*method,const_cast<Value*>(&v),{}));
                    if(equality_contract(*c))return 0;
                    std::uint64_t result=0;for(const auto& [name,value]:v.object().fields)result+=std::hash<std::string>{}(name)^logical_hash(value);return result;
                }
                if(std::holds_alternative<ValueOwned<Collection>>(v.data)) {
                    std::uint64_t result=0;for(const auto& value:v.collection().values)result=result*16777619ull+logical_hash(value);
                    for(const auto size:v.collection().dimensions)result=result*16777619ull+size;return result;
                }
                if(v.type=="string")return std::hash<std::string>{}(v.as_string());
                return std::visit([](const auto& x)->std::uint64_t {using T=std::decay_t<decltype(x)>;if constexpr(std::is_arithmetic_v<T>)return static_cast<std::uint64_t>(std::hash<T>{}(x));else if constexpr(std::is_same_v<T,Pointer>)return std::hash<void*>{}(x.address);else return 0;},v.data);
            }
            Value invoke(const Class& owner,const Method& method,Value* self,std::vector<Value> args) {
                try {
                // The interpreter retains its supplied streams. Native backends
                // compile the ordinary WindowsConsole Bolt bodies instead.
                if (native_exception(owner) && self) {
                    if (method.is_constructor) {
                        set_exception_fields(*self,args.empty()?std::string():args.front().as_string());
                        return {};
                    }
                    if (method.name=="ToString") {
                        const auto& fields=self->object().fields;
                        const auto* type=program.find_class(self->type);
                        std::string text=(type?type->short_name:self->type)+": "+fields.at("Message").as_string();
                        auto file=fields.at("SourceFile").as_string();
                        if (!file.empty()) text+="\n"+file+":"+std::to_string(fields.at("SourceLine").as_int())+":"+std::to_string(fields.at("SourceColumn").as_int());
                        auto trace=fields.at("StackTrace").as_string();
                        if (!trace.empty()) text+="\n"+trace;
                        return Value(std::move(text));
                    }
                }
                // An interpreter instance runs on one execution thread. Its static
                // word objects can hold the TLS values directly.
                if(random_word(owner) && self) {
                    if(method.is_constructor) {
                        Frame frame;
                        initialize_fields(owner,*self,frame);
                        return {};
                    }
                    if(method.is_destructor)return {};
                    auto& word=self->object().fields.at("key");
                    if(method.name=="Read")return Value(scalar<std::uint64_t>(word));
                    if(method.name=="Write"){word=Value(scalar<std::uint64_t>(args.at(0)));return {};}
                }
                if(crypto_random_source(method.token.at.file)) {
                    if(method.name=="GetUInt64") {
                        std::uint64_t value;
                        platform::secure_random_bytes(reinterpret_cast<std::uint8_t*>(&value),sizeof(value));
                        return Value(value);
                    }
                    if(method.name=="GetByte") {
                        std::uint8_t byte;
                        platform::secure_random_bytes(&byte,1);
                        return Value(byte);
                    }
                    if(method.name=="Fill") {
                        auto& arg=args.at(0);
                        auto& slots=(is_reference(arg.type)?arg.dereference():arg).collection().values;
                        // Interpreted array elements have Value storage, unlike the
                        // contiguous native byte array. Bound temporary memory.
                        std::array<std::uint8_t,256> bytes;
                        for(std::size_t at=0;at<slots.size();) {
                            const auto count=std::min(bytes.size(),slots.size()-at);
                            platform::secure_random_bytes(bytes.data(),count);
                            for(std::size_t j=0;j<count;++j)slots[at+j]=Value(bytes[j]);
                            at+=count;
                        }
                        return {};
                    }
                }
                if(platform_console(owner)) {
                    if(method.name=="ReadLine") { std::string text; std::getline(input,text); return Value(text); }
                    if(method.name=="ClearDisplay") { clear_interpreter_display(output); return {}; }
                    if(method.name=="Write"||method.name=="WriteLine") {
                        const auto& value=is_reference(method.parameters.at(0).type)&&args.at(0).type==method.parameters.at(0).type?args.at(0).dereference():args.at(0);
                        write_interpreter_console(output, value.as_string(), method.name=="WriteLine");
                        return {};
                    }
                }
                // Compiled DateTime executes the platform-specific Bolt bodies and
                // reaches the OS C ABI directly. --runtime is itself a C++
                // interpreter, so mirror those three observable platform results
                // here instead of pretending it can execute raw foreign pointers.
                if(platform_datetime(owner)) {
                    if(method.name=="UtcNowTicks")return Value(platform::Clock::UnixTicks());
                    if(method.name=="MonotonicTicks")return Value(platform::Clock::MonotonicTicks());
                    const auto ticks=scalar<std::int64_t>(args.at(0));
                    if(method.name=="OffsetSeconds")return Value(platform::TimeZone::OffsetSeconds(ticks));
                    if(method.name=="LocalMidnightTicks")return Value(platform::TimeZone::LocalMidnightTicks(ticks));
                    throw std::runtime_error("unsupported interpreter DateTime platform primitive "+owner.name+"."+method.name);
                }
                // The interpreter's clock already reports 100 ns ticks.
                if(windows_counter_frequency(owner) && method.name=="Query") return Value(std::int64_t{10000000});
                if(desktop_clock_or_thread_ops(owner)){
                    if(method.name=="MonotonicTicks")return Value(platform::Clock::MonotonicTicks());
                    const auto ms=method.name=="Yield"?0:scalar<std::int32_t>(args.at(0));
                    if(ms<0)throw library::ArgumentOutOfRangeException("Sleep duration must be non-negative.");
                    if(ms==0)std::this_thread::yield();else std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                    return {};
                }
                if(owner.is_native&&owner.name.starts_with("Bolt.Runtime.")) {
                    const auto value=[&](std::size_t i)->Value&{return is_reference(method.parameters.at(i).type)&&args.at(i).type==method.parameters.at(i).type?args.at(i).dereference():args.at(i);};
                    if(owner.name=="Bolt.Runtime.Values") {
                        if(method.name=="Equal")return Value(logical_equal(value(0),value(1)));
                        if(method.name=="Hash")return Value(logical_hash(value(0)));
                        if(method.name=="Format")return Value(format(value(0)));
                    }
                    if(owner.name=="Bolt.Runtime.Bits") {
                        auto a=scalar<std::uint64_t>(value(0)),b=scalar<std::uint64_t>(value(1));return Value(method.name=="Add64"?a+b:a*b);
                    }
                    if(owner.name=="Bolt.Runtime.Failure") {
                        const auto message=value(0).as_string();
                        if(method.name=="IO")platform::Failure::IO(message);
                        if(method.name=="DirectoryMissing")platform::Failure::DirectoryMissing(message);
                        if(method.name=="Argument")platform::Failure::Argument(message);
                        if(method.name=="Range")platform::Failure::Range(message);
                        if(method.name=="Index")platform::Failure::Index(message);
                        if(method.name=="MissingKey")platform::Failure::MissingKey(message);
                        if(method.name=="InvalidOperation")platform::Failure::InvalidOperation(message);
                        if(method.name=="Overflow")platform::Failure::Overflow(message);
                        if(method.name=="Format")platform::Failure::Format(message);
                        throw Error(method.token.at,"unrecognized failure primitive","BLT3000");
                    }
                    if(owner.name=="Bolt.Runtime.TimeZone")return Value(method.name=="OffsetSeconds"?platform::TimeZone::OffsetSeconds(scalar<std::int64_t>(value(0))):platform::TimeZone::LocalMidnightTicks(scalar<std::int64_t>(value(0))));
                    if(owner.name=="Bolt.Runtime.Clock")return Value(method.name=="UnixTicks"?platform::Clock::UnixTicks():platform::Clock::MonotonicTicks());
                    throw std::runtime_error("unsupported interpreter primitive "+owner.name+"."+method.name);
                }
                if(self)self=&unbox(*self);
                if(method.is_abstract) {
                    if(owner.name=="System.IStringable"&&(method.name=="ToString"||method.name=="@convert.string")&&self&&(is_primitive(self->type)||is_pointer(self->type)||program.find_enum(self->type)||is_collection(self->type)||is_optional(self->type)))return Value(format(*self));
                    auto [actual_owner,actual_method]=dispatch(owner,method,self);
                    if(actual_method==&method)throw Error(method.token.at,"interface method has no implementation", "BLT3000");
                    return invoke(*actual_owner,*actual_method,self,std::move(args));
                }
                if(args.size()>method.parameters.size())throw Error(method.token.at,"incorrect argument count", "BLT3000");
                if(state->calls>=options.max_call_depth)throw Error(method.token.at,"runtime call-depth limit exceeded", "BLT3000");
                CounterGuard counter(state->calls);
                Frame frame;
                frame.owner=&owner;
                frame.method=&method;
                frame.self=self;
                ScopeGuard parameters(frame);
                // Implicit construction (locals, fields, array elements and base
                // subobjects) has no call AST to carry normalized defaults.
                // Evaluate each missing default exactly once in the callee's
                // declaration context, just as an explicit normalized call does.
                while (args.size() < method.parameters.size()) {
                    const auto& p = method.parameters[args.size()];
                    if (p.default_value) args.push_back(evaluate(*p.default_value, frame));
                    else if (p.variadic) args.push_back(Value::collection_value(p.type));
                    else throw Error(p.token.at, "missing required constructor argument '"+p.name+"'", "BLT3000");
                }
                for(std::size_t i=0;i<args.size();++i) {
                    const auto& p=method.parameters[i];
                    Slot slot;
                    if(is_reference(p.type)) {
                        if(!method.operator_symbol.empty()||(p.constant&&args[i].type!=p.type))slot.alias=&args[i];
                        else {
                            if(!std::holds_alternative<Pointer>(args[i].data))throw Error(p.token.at,"reference parameter requires a reference value", "BLT3000");
                            slot.alias=&args[i].dereference();
                        }
                        slot.alias_lifetime=slot.alias->lifetime;
                    } else slot.value=convert(p.type,args[i]);
                    frame.scopes.back().emplace(p.name,std::move(slot));
                }
                if(method.is_constructor) {
                    if(!self)throw std::runtime_error("constructor needs an object");
                    if(!owner.base.empty()) {
                        const auto& base=*program.find_class(owner.base);
                        const auto* m=method.base_constructor.empty()?default_constructor(base):method_by_id(base,method.base_constructor);
                        auto base_args=arguments(method.base_arguments,0,m,frame);
                        initialize(base,*self,m,std::move(base_args));
                    }
                    initialize_fields(owner,*self,frame);
                }
                auto flow=execute(*method.body,frame);
                if(method.type=="void")return {
                };
                if(flow.kind!=Flow::Kind::Return)throw Error(method.token.at,"method did not return a value", "BLT3000");
                if(is_reference(method.type))return std::move(flow.value);
                return convert(method.type,flow.value);
                } catch(Error& error) {
                    if(error.diagnostic.source_line.empty()) {
                        if(auto it=program.sources.find(error.location.file);it!=program.sources.end())attach_source(error.diagnostic,it->second);
                    }
                    error.diagnostic.frames.push_back({method.token.at,owner.name+"."+method.name});
                    throw;
                } catch(const std::exception& exception) {
                    auto fault=language_fault(exception,method.token.at);
                    auto& d=fault.diagnostic;
                    if(auto it=program.sources.find(d.location.file);it!=program.sources.end())attach_source(d,it->second);
                    d.frames.push_back({method.token.at,owner.name+"."+method.name});
                    throw fault;
                }
            }
            Value evaluate(const Expr& e,Frame& frame) {
                tick(e.token.at);
                try {
                    using K=Expr::Kind;
                    using B=Expr::Binding;
                    if(e.kind==K::OptionalHas) {
                        if(e.args[0]->lvalue)return Value(reference(*e.args[0],frame).has_value());
                        return Value(evaluate(*e.args[0],frame).has_value());
                    }
                    if(e.kind==K::OptionalValue) {
                        if(e.lvalue)return reference(e,frame);
                        return evaluate(*e.args[0],frame).optional_payload();
                    }
                    if(e.kind==K::SizeOf)return Value::signed_integer("int",std::stoll(e.resolved));
                    if(e.kind==K::Literal)return literal_value(e);
                    if(e.kind==K::Default)return make(e.type);
                    if(e.kind==K::Conditional)return convert(e.type,evaluate(*e.args[evaluate(*e.args[0],frame).as_bool()?1:2],frame));
                    if(e.kind==K::Let){
                        Slot slot;
                        if(is_reference(e.declared_type)){slot.alias=&reference(*e.args[0],frame);slot.alias_lifetime=slot.alias->lifetime;}
                        else slot.value=convert(e.declared_type,evaluate(*e.args[0],frame));
                        ScopeGuard scope(frame);frame.scopes.back().emplace(e.text,std::move(slot));
                        return evaluate(*e.args[1],frame);
                    }
                    if(e.binding==B::EnumValue) {
                        Expr n(K::Literal,e.token,e.resolved);
                        n.token.kind=Token::Kind::Number;
                        return convert(e.type,literal_value(n));
                    }
                    if(e.binding==B::Property) {
                        if(e.owner=="String"&&e.text=="Empty")return Value("");
                        if(e.owner=="Math")return convert(e.type,Value(e.text=="Pi"?native::Pi:e.text=="Tau"?native::Tau:native::E));
                        if(is_numeric(e.owner))return numeric_limit_value(e.owner,e.text=="MaxValue");
                        if(e.owner=="Arena") {
                            auto& arena=reference(*e.args[0],frame).arena();
                            return Value(e.text=="UsedBytes"?arena.used():e.text=="CapacityBytes"?arena.capacity():e.text=="PeakBytes"?arena.peak():arena.limit());
                        }
                        Value temp;
                        Value* v=nullptr;
                        if(e.arrow) {
                            temp=evaluate(*e.args[0],frame);
                            v=&temp.dereference();
                        } else if(e.args[0]->lvalue)v=&reference(*e.args[0],frame);
                        else {
                            temp=evaluate(*e.args[0],frame);
                            v=&temp;
                        }
                        if(v->type=="string")return Value(native::cast<std::int32_t>(e.text=="ByteLength"?v->as_string().size():unicode::length(v->as_string())));
                        if(is_array(v->type)&&e.text=="Rank")return Value(native::cast<std::int32_t>(array_rank(v->type)));
                        auto& c=v->collection();
                        return Value(native::cast<std::int32_t>(e.text=="Capacity"?c.values.capacity():collection_kind(v->type)=="Dictionary"?c.entries.size():c.values.size()));
                    }
                    // Keep the dynamic receiver until the destination conversion is known.
                    // A concrete by-value destination still slices; interface boxing does not.
                    if(e.lvalue)return reference(e,frame);
                    if(e.kind==K::Name||e.kind==K::Member) {
                        if(e.binding==B::Field) {
                            auto v=evaluate(*e.args[0],frame);
                            if(e.arrow)return v.dereference().object().fields.at(e.text);
                            return v.object().fields.at(e.text);
                        }
                    }
                    if(e.kind==K::Call)return call_expression(e,frame);
                    if(e.kind==K::Pack) {
                        for(const auto& call:e.args)(void)evaluate(*call,frame);
                        return {
                        };
                    }
                    if(e.kind==K::Array) {
                        auto result=Value::collection_value(e.type);
                        auto& dimensions=result.collection().dimensions;
                        if(array_rank(e.type)>1)dimensions.assign(e.dimensions.begin(),e.dimensions.end());
                        else dimensions[0]=e.args.size();
                        for(const auto& a:e.args)result.collection().values.push_back(convert(element_type(e.type),evaluate(*a,frame)));
                        return result;
                    }
                    if(e.kind==K::Index) {
                        auto base=evaluate(*e.args[0],frame);
                        if(base.type=="string"){auto key=evaluate(*e.args[1],frame).as_int();if(key<0)throw std::runtime_error("string index is out of range");return Value::signed_integer("char",unicode::at(base.as_string(),static_cast<std::size_t>(key)));}
                        if(is_array(base.type)) {const auto offset=array_index(e,frame,base);return base.collection().values.at(offset);}
                        auto key=evaluate(*e.args[1],frame);
                        if(collection_kind(base.type)=="Dictionary") {
                            for(const auto& [k,v]:base.collection().entries)if(equal_values(k,key))return v;
                            throw std::runtime_error("dictionary key not found");
                        }
                        return base.collection().values.at(native::checked_index(key.as_int(),base.collection().values.size()));
                    }
                    if(e.kind==K::New) {
                        ValueArena* arena=e.arena?&reference(*e.arena,frame).arena():nullptr;
                        if(e.arrow) {
                            auto result=Value::collection_value(e.type);
                            auto& dimensions=result.collection().dimensions;
                            for(std::size_t axis=0;axis<e.args.size();++axis) {
                                auto dimension=evaluate(*e.args[axis],frame);
                                dimensions[axis]=is_unsigned(dimension.type)?native::checked_dimension(scalar<std::uint64_t>(dimension)):native::checked_dimension(dimension.as_int());
                            }
                            const auto size=native::array_count(dimensions);
                            if(size>result.collection().values.max_size())throw std::runtime_error("array size is too large");
                            result.collection().values.reserve(size);
                            for(std::size_t i=0;i<size;++i)result.collection().values.push_back(make(element_type(e.type)));
                            return result;
                        }
                        auto* c=program.find_class(e.text);
                        const auto* m=c&&!e.resolved.empty()?method_by_id(*c,e.resolved):nullptr;
                        auto args=arguments(e.args,0,m,frame);
                        if(arena) {
                            auto* storage=arena->allocate(sizeof(Value),alignof(Value));
                            auto* pointer=::new(storage) Value();
                            pointer->allocation_arena=arena;
                            try {
                                if(c) {
                                    *pointer=Value::object_value(e.text,arena);
                                    initialize(*c,*pointer,m,std::move(args));
                                } else *pointer=args.empty()?make(e.text):convert(e.text,args[0]);
                                arena->add_root(pointer);
                            } catch(...) {pointer->~Value();throw;}
                            return Value::pointer_value(e.type,pointer);
                        }
                        Value v;
                        if(c)v=construct(e.text,std::move(args),m);
                        else v=args.empty()?make(e.text):convert(e.text,args[0]);
                        auto owned=std::make_unique<Value>(std::move(v));
                        auto* pointer=owned.get();
                        heap.emplace(pointer,std::move(owned));
                        return Value::pointer_value(e.type,pointer);
                    }
                    if(e.kind==K::Delete) {
                        auto v=evaluate(*e.args[0],frame);
                        auto pointer=std::get<Pointer>(v.data);
                        if(!pointer.address)return {
                        };
                        if(!pointer.lifetime.expired()&&pointer.address->allocation_arena)throw std::runtime_error("delete cannot free an arena allocation");
                        auto found=heap.find(pointer.address);
                        if(found==heap.end())throw std::runtime_error("delete requires a live heap allocation (not a stack address)");
                        heap.erase(found);
                        return {
                        };
                    }
                    if(e.binding==B::Operator) {
                        auto& destination=reference(*e.args[0],frame);
                        Value old=destination;
                        std::vector<Value> args;args.push_back(old);
                        if(e.args.size()==2)args.push_back(evaluate(*e.args[1],frame));
                        const auto& owner=*program.find_class(e.owner);const auto& method=*method_by_id(owner,e.resolved);
                        for(std::size_t i=0;i<args.size();++i)args[i]=convert(referred(method.parameters[i].type),args[i]);
                        auto result=invoke(owner,method,nullptr,std::move(args));
                        assign_in_place(destination,convert(e.type,result));
                        return e.kind==K::Postfix?std::move(old):destination;
                    }
                    if(e.kind==K::Assign) {
                        const auto& lhs=*e.args[0];
                        if(e.text=="="&&lhs.kind==K::Index&&collection_kind(lhs.owner)=="Dictionary"){
                            auto& dictionary=reference(*lhs.args[0],frame);
                            auto key=convert(generic_args(lhs.owner)[0],evaluate(*lhs.args[1],frame));
                            auto right=convert(e.type,evaluate(*e.args[1],frame));
                            auto& entries=dictionary.collection().entries;
                            for(auto& item:entries)if(equal_values(item.first,key)){
                                assign_in_place(item.second,right);
                                return item.second;
                            }
                            entries.emplace_back(std::move(key),right);
                            return right;
                        }
                        auto& destination=reference(*e.args[0],frame);
                        Value old;
                        if(e.text!="=")old=destination;
                        if(e.text=="?" "?="&&(is_optional(old.type)?old.has_value():std::get<Pointer>(old.data).address!=nullptr))return destination;
                        auto right=evaluate(*e.args[1],frame);
                        auto updated=e.text=="="||e.text=="?" "?="?convert(e.type,right):convert(e.type,arithmetic(e.text.substr(0,e.text.size()-1),old,right));
                        assign_in_place(destination,updated);
                        return destination;
                    }
                    if(e.kind==K::Unary||e.kind==K::Postfix) {
                        if(e.text=="&")return Value::pointer_value(e.type,&reference(*e.args[0],frame));
                        if(e.text=="++"||e.text=="--") {
                            auto& v=reference(*e.args[0],frame);
                            Value old=v;
                            auto updated=convert(e.type,arithmetic(e.text=="++"?"+":"-",old,convert(old.type,Value(1))));
                            assign_in_place(v,updated);
                            return e.kind==K::Postfix?std::move(old):v;
                        }
                        auto v=evaluate(*e.args[0],frame);
                        if(e.text=="!")return Value(!v.as_bool());
                        if(e.text=="~") {const auto* en=program.find_enum(e.type);auto type=en?en->underlying:e.type;auto raw=v;raw.type=type;auto mask=numeric_limit_value(type,is_unsigned(type));if(!is_unsigned(type))mask=convert(type,Value(-1));auto result=numeric_binary("^",promoted(type),raw,mask);return convert(e.type,result);}
                        if(e.text=="+")return convert(e.type,v);
                        if(e.text=="-")return numeric_binary("-",e.type,default_primitive(e.type),v);
                    }
                    if(e.kind==K::Binary) {
                        if(e.text=="+"&&(e.args[0]->type=="string"||e.args[1]->type=="string")) {
                            auto left=format(evaluate(*e.args[0],frame));
                            left += format(evaluate(*e.args[1],frame));
                            return Value(std::move(left));
                        }
                        auto a=evaluate(*e.args[0],frame);
                        if(e.text=="??"&&is_optional(a.type))return a.has_value()?convert(e.type,is_optional(e.type)?a:a.optional_payload()):convert(e.type,evaluate(*e.args[1],frame));
                        if(e.text=="??")return std::get<Pointer>(a.data).address?a:convert(e.type,evaluate(*e.args[1],frame));
                        if(e.text=="&&"&&!a.as_bool())return Value(false);
                        if(e.text=="||"&&a.as_bool())return Value(true);
                        auto b=evaluate(*e.args[1],frame);
                        if(e.text==",")return b;
                        if(e.text=="&&"||e.text=="||")return Value(b.as_bool());
                        if(e.text=="=="||e.text=="!="||e.text=="<"||e.text==">"||e.text=="<="||e.text==">=")return compare_values(e.text,a,b);
                        return arithmetic(e.text,a,b);
                    }
                    throw Error(e.token.at,"internal error: unsupported expression", "BLT3000");
                }catch(const Error&) {
                    throw;
                }catch(const std::exception& ex) {
                    throw language_fault(ex,e.token.at);
                }
            }
            Flow scoped(const Stmt& s,Frame& f) {
                ScopeGuard scope(f);
                return execute(s,f);
            }
            Flow for_each(const Stmt& s,Frame& frame) {
                Value temporary;
                Value* source;
                if(s.expression->lvalue)source=&reference(*s.expression,frame);
                else {temporary=evaluate(*s.expression,frame);source=&temporary;}
                const std::weak_ptr<char> lifetime=source->lifetime;
                const auto version=source->range_version;
                const bool text=source->type=="string";
                const bool dictionary=collection_kind(source->type)=="Dictionary";
                const auto characters=text?source->as_string():std::string();
                std::size_t index=0;
                while(true) {
                    tick(s.token.at);
                    if(lifetime.expired()||source->range_version!=version)
                        throw language_fault(library::InvalidOperationException("collection was structurally modified during iteration"),s.token.at);
                    Value element;
                    Value* address=&element;
                    if(text) {
                        if(index==characters.size())break;
                        element=Value::signed_integer("char",unicode::decode(characters,index));
                    } else if(dictionary) {
                        const auto& entries=source->collection().entries;
                        if(index==entries.size())break;
                        const auto& [key,value]=entries[index++];
                        element=Value::object_value(s.type);
                        element.object().fields.emplace("Key",key);
                        element.object().fields.emplace("Value",value);
                    } else {
                        auto& elements=source->collection().values;
                        if(index==elements.size())break;
                        address=&elements[index++];
                    }
                    ScopeGuard scope(frame);
                    Slot binding;
                    if(is_reference(s.type)) {binding.alias=address;binding.alias_lifetime=address->lifetime;}
                    else binding.value=convert(s.type,*address);
                    frame.scopes.back().emplace(s.name,std::move(binding));
                    auto result=execute(*s.body,frame);
                    if(result.kind==Flow::Kind::Return)return result;
                    if(result.kind==Flow::Kind::Break)break;
                }
                return {};
            }
            Flow execute_handlers(const Stmt& statement,Frame& frame) {
                try { return scoped(*statement.body,frame); }
                catch (InterpretedException& error) {
                    for (const auto& handler:statement.statements) {
                        if (!handler->type.empty() && !derives(error.payload.type,referred(handler->type))) continue;
                        ScopeGuard scope(frame);
                        if (!handler->name.empty()) {
                            error.update_source();
                            Slot binding;
                            binding.alias=&error.payload;
                            binding.alias_lifetime=error.payload.lifetime;
                            frame.scopes.back().emplace(handler->name,std::move(binding));
                        }
                        return execute(*handler->body,frame);
                    }
                    throw;
                }
            }
            Flow execute(const Stmt& s,Frame& frame) {
                tick(s.token.at);
                using K=Stmt::Kind;
                if(s.kind==K::Throw) {
                    if(!s.expression)throw;
                    throw InterpretedException(evaluate(*s.expression,frame),s.token.at);
                }
                if(s.kind==K::ForEach)return for_each(s,frame);
                if(s.kind==K::Try) {
                    if(!s.alternative)return execute_handlers(s,frame);
                    Flow result;
                    try { result=execute_handlers(s,frame); }
                    catch (...) { scoped(*s.alternative,frame); throw; }
                    scoped(*s.alternative,frame);
                    return result;
                }
                if(s.kind==K::SwitchScope) {
                    auto result=execute(*s.body,frame);
                    return result.kind==Flow::Kind::Break?Flow{}:std::move(result);
                }
                if(s.kind==K::Declarations) {
                    for(const auto& child:s.statements){auto result=execute(*child,frame);if(result.kind!=Flow::Kind::Normal)return result;}
                } else if(s.kind==K::Block) {
                    ScopeGuard scope(frame);
                    for(const auto& child:s.statements) {
                        auto result=execute(*child,frame);
                        if(result.kind!=Flow::Kind::Normal)return result;
                    }
                } else if(s.kind==K::Variable) {
                    Slot slot;
                    if(is_reference(s.type)){
                        slot.alias=&reference(*s.expression,frame);
                        slot.alias_lifetime=slot.alias->lifetime;
                    }
                    else if(s.type=="Arena")slot.value=evaluate(*s.expression,frame);
                    else slot.value=s.expression?convert(s.type,evaluate(*s.expression,frame)):make(s.type);
                    frame.scopes.back().emplace(s.name,std::move(slot));
                } else if(s.kind==K::Expression)(void)evaluate(*s.expression,frame);
                else if(s.kind==K::If) {
                    if(evaluate(*s.condition,frame).as_bool())return scoped(*s.body,frame);
                    if(s.alternative)return scoped(*s.alternative,frame);
                } else if(s.kind==K::While||s.kind==K::For) {
                    ScopeGuard scope(frame);
                    if(s.init)(void)execute(*s.init,frame);
                    while(!s.condition||evaluate(*s.condition,frame).as_bool()) {
                        tick(s.token.at);
                        auto result=scoped(*s.body,frame);
                        if(result.kind==Flow::Kind::Return)return result;
                        if(result.kind==Flow::Kind::Break)break;
                        if(s.increment)(void)evaluate(*s.increment,frame);
                    }
                } else if(s.kind==K::Return) {
                    Flow flow;
                    flow.kind=Flow::Kind::Return;
                    if(s.expression)flow.value=is_reference(frame.method->type)?Value::pointer_value(frame.method->type,&reference(*s.expression,frame)):evaluate(*s.expression,frame);
                    return flow;
                } else if(s.kind==K::Break)return {
                    Flow::Kind::Break, {
                    }
                };
                else if(s.kind==K::Continue)return {
                    Flow::Kind::Continue, {
                    }
                };
                return {
                };
            }
            int run_entry(const std::vector<std::string>& arguments = {}) {
                const auto* c=program.find_class(program.entry_class.empty()?"Program":program.entry_class);
                const auto* m=c?find_method(*c,"Main"):nullptr;
                if(!m)throw std::runtime_error("Program.Main is missing");
                std::vector<Value> parameters;
                if(program.entry_accepts_arguments) {
                    if(arguments.size()>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                        throw std::runtime_error("too many command-line arguments");
                    auto values=Value::collection_value("string[]");
                    values.collection().values.reserve(arguments.size());
                    for(const auto& argument:arguments)values.collection().values.emplace_back(argument);
                    values.collection().dimensions={arguments.size()};
                    parameters.push_back(std::move(values));
                }
                auto v=invoke(*c,*m,nullptr,std::move(parameters));
                return m->type=="void"?0:native::cast<int>(v.as_int());
            }
            int nested(const std::string& kind,const std::string& source,const Location& at) {
                detail::RuntimeDepthGuard guard(*state->budget,options.max_runtime_depth);
                if(!guard)throw Error(at,"runtime execution nesting limit exceeded", "BLT3000");
                if(kind.ends_with("Jit")) {
#if defined(BOLT_INTERPRETER_ONLY) || defined(__EMSCRIPTEN__)
                    throw std::runtime_error("Native JIT is unavailable in this interpreter build; use Runtime.Execute instead.");
#else
                    JitOptions settings;settings.execution=options;settings.input=&input;settings.output=&output;settings.preprocessing=program.preprocessor;
                    if(kind=="ExecuteJit")return detail::jit_statements_nested(source,std::move(settings),state->budget);
                    if(kind=="ExecuteFileJit")return detail::jit_file_nested(path_from_utf8(source),std::move(settings),state->budget);
                    return detail::jit_program_nested(source,std::move(settings),state->budget);
#endif
                }
                Program p;
                if(kind=="ExecuteFile")p=load(path_from_utf8(source),true,program.preprocessor);
                else if(kind=="Execute")p=compile_source(statement_program(source),"<runtime statements>",true,program.preprocessor);
                else p=compile_source(source,"<runtime program>",true,program.preprocessor);
                Engine child(std::move(p),options,input,output,state);
                return child.run_entry();
            }
            const Method& exported_method(const Class& owner,const std::string& name,const std::vector<Value>& args,bool stat) {
                const Method* best=nullptr;
                int best_score=std::numeric_limits<int>::max();
                for(const auto& m:owner.methods) {
                    if(m.name!=name||!m.generic_parameters.empty()||!m.pack_parameter.empty()||m.is_static!=stat||m.is_constructor||m.parameters.size()!=args.size())continue;
                    int score=0;
                    bool ok=true;
                    for(std::size_t i=0;i<args.size();++i) {
                        auto t=m.parameters[i].type;
                        if(t==args[i].type)continue;
                        if(assignable(t,args[i].type)||(is_interface(program,t)&&(derives(args[i].type,t)||(t=="System.IStringable"&&stringable_type(program,args[i].type)))))score+=2;
                        else {
                            ok=false;
                            break;
                        }
                    }
                    if(ok&&score<best_score) {
                        best=&m;
                        best_score=score;
                    }
                }
                if(!best)throw std::runtime_error("no matching exported method '"+owner.name+"."+name+"'");
                if(!best->is_public)throw std::runtime_error(best->is_protected?"method is protected":"method is private");
                return *best;
            }
        };
    }
    // namespace
    std::string interpreter_requirement(const Program& program) {
        static const std::unordered_set<std::string> supported={"range-for","exception handling","native standard library / interop","struct value layout","command-line entry arguments","bitwise operators","enum bitwise operators","conditional operator","null coalescing","Unicode char","Unicode char conversions","Unicode string methods","Unicode string ByteLength","Unicode string indexing"};
        const auto* console=program.find_class("Bolt.Runtime.WindowsConsole");
        if(!console)console=program.find_class("Bolt.Runtime.LinuxConsole");
        if(!console)console=program.find_class("Bolt.Runtime.MacOSConsole");
        if(!console)console=program.find_class("Bolt.Runtime.HostConsole");
        const bool console_fallback=console&&console->reachable&&platform_console(*console);
        const auto* datetime=program.find_class("Bolt.Runtime.WindowsDateTime");
        if(!datetime)datetime=program.find_class("Bolt.Runtime.LinuxDateTime");
        if(!datetime)datetime=program.find_class("Bolt.Runtime.MacOSDateTime");
        if(!datetime)datetime=program.find_class("Bolt.Runtime.HostDateTime");
        const bool datetime_fallback=datetime&&datetime->reachable&&platform_datetime(*datetime);
        const bool scheduler_fallback=std::any_of(program.classes.begin(),program.classes.end(),
            [](const Class& c){return c.reachable&&desktop_clock_or_thread_ops(c);});
        const auto* crypto=program.find_class("System.Security.Cryptography.RandomNumberGenerator");
        const bool crypto_fallback=crypto&&crypto->reachable&&crypto_random_source(crypto->token.at.file);
        const bool platform_interop=std::any_of(program.classes.begin(),program.classes.end(),[](const Class& c){return std::any_of(c.methods.begin(),c.methods.end(),[](const Method& m){return platform_api_source(m.token.at.file);});});
        bool needs_interop=false,needs_raw_pointer=false,needs_destructor=false;
        std::unordered_set<std::string> used_types;
        {
            // Semantic reachability includes the active platform Console's private
            // native dependencies even though invoke() handles its public API here.
            // Inspect the remaining program before exempting those dependencies;
            // foreign calls in user code still require a native backend.
            auto type=[&](auto&& visit,std::string name)->void {
                while(is_reference(name)||is_pointer(name)||is_optional(name)||is_array(name))name=element_type(name);
                used_types.insert(name);
                for(const auto& argument:generic_args(name))visit(visit,argument);
            };
            auto expression=[&](auto&& visit,const Expr* e)->void {
                if(!e)return;
                if(e->kind==Expr::Kind::Index&&!e->args.empty()&&is_pointer(e->args[0]->type))needs_raw_pointer=true;
                if(e->kind==Expr::Kind::Call&&e->binding==Expr::Binding::Cast&&e->args.size()==2&&is_pointer(e->type)&&is_pointer(e->args[1]->type)) {
                    const auto target=element_type(e->type),source=element_type(e->args[1]->type);
                    // Value-backed pointers can retain their existing identity
                    // through void*/base conversions, but cannot reinterpret
                    // their pointee's raw memory as another value type.
                    if(target!=source&&target!="void"&&!derives_from(program,source,target))needs_raw_pointer=true;
                }
                for(const auto& name:{e->type,e->declared_type,e->owner,e->callback_owner})type(type,name);
                if(!e->resolved.empty())if(const auto* owner=program.find_class(e->owner))if(const auto* method=method_by_id(*owner,e->resolved))needs_interop|=!method->native_abi.empty();
                for(const auto& argument:e->type_args)type(type,argument);
                visit(visit,e->arena.get());
                for(const auto& argument:e->args)visit(visit,argument.get());
            };
            auto statement=[&](auto&& visit,const Stmt* s)->void {
                if(!s)return;
                type(type,s->type);
                expression(expression,s->expression.get());expression(expression,s->condition.get());expression(expression,s->increment.get());
                visit(visit,s->init.get());visit(visit,s->body.get());visit(visit,s->alternative.get());
                for(const auto& child:s->statements)visit(visit,child.get());
            };
            auto headers=[&](const auto& attributes){for(const auto& attribute:attributes)if(attribute.name=="Header"&&!platform_api_source(attribute.token.at.file))needs_interop=true;};
            for(const auto& c:program.classes) {
                if(platform_console(c)||platform_datetime(c)||desktop_clock_or_thread_ops(c)||windows_counter_frequency(c)||random_word(c))continue;
                // The standard library is embedded as one program. Imports on
                // unused platform types do not belong to the interpreted code.
                if(c.token.at.file.starts_with("<bolt-stdlib>") && !emitted_class(c))continue;
                headers(c.attributes);
                // Headers and extern signatures are prepared even on types
                // whose method bodies are not reachable.
                for(const auto& method:c.methods)if(!platform_api_source(method.token.at.file)) {
                    headers(method.attributes);needs_interop|=!method.native_abi.empty();
                }
                if(!emitted_class(c)||c.is_native)continue;
                type(type,c.base);for(const auto& base:c.interfaces)type(type,base);
                for(const auto& field:c.fields){type(type,field.type);expression(expression,field.initializer.get());}
                for(const auto& method:c.methods) {
                    needs_destructor|=method.is_destructor;
                    if(platform_api_source(method.token.at.file)||(crypto_fallback&&crypto_random_source(method.token.at.file)))continue;
                    type(type,method.type);
                    for(const auto& parameter:method.parameters){type(type,parameter.type);expression(expression,parameter.default_value.get());}
                    for(const auto& argument:method.base_arguments)expression(expression,argument.get());
                    for(const auto& initializer:method.constructor_initializers)expression(expression,initializer.get());
                    for(const auto& initializer:method.field_initializers)expression(expression,initializer.second.get());
                    statement(statement,method.body.get());
                }
            }
        }
        if(needs_raw_pointer)return "raw pointer indexing or reinterpretation";
        for(const auto& feature:program.cpp_features)if(!supported.contains(feature)) {
            if(feature=="destructors and RAII"&&!needs_destructor)continue;
            if(platform_interop&&feature=="native C/C++ interoperability"&&!needs_interop)continue;
            return feature;
        }
        for(const auto& c:program.classes)if(c.is_native&&c.reachable&&c.name!="Bolt.Runtime.Values"&&c.name!="Bolt.Runtime.Bits"&&c.name!="Bolt.Runtime.Failure"&&c.name!="Bolt.Runtime.Clock"&&c.name!="Bolt.Runtime.TimeZone"&&c.name!="Bolt.Runtime.ThreadOps") {
            if(platform_console(c)||platform_datetime(c)||native_exception(c)||generic_base(c.name)=="System.Collections.Generic.KeyValuePair")continue;
            if((console_fallback||datetime_fallback||crypto_fallback||scheduler_fallback)&&!used_types.contains(c.name)&&c.token.at.file.starts_with("<bolt-stdlib>")&&
               (c.name=="Bolt.Runtime.NativeString"||c.token.at.file.ends_with("/System/Exceptions.bolt")||platform_api_source(c.token.at.file)))continue;
            return "native type "+c.name;
        }
        return {};
    }
    struct Runtime::Impl {
        Engine engine;
        Impl(Program p,ExecutionOptions o,std::istream& in,std::ostream& out):engine(std::move(p),o,in,out) {
        }
    };
    Runtime::Runtime(Program p,ExecutionOptions o,std::istream* in,std::ostream* out):impl(std::make_unique<Impl>(std::move(p),o,in?*in:std::cin,out?*out:std::cout)) {
    }
    Runtime::~Runtime()=default;
    Runtime::Runtime(Runtime&&) noexcept=default;
    Runtime& Runtime::operator=(Runtime&&) noexcept=default;
    int Runtime::run() { return run(std::vector<std::string>{}); }
    int Runtime::run(const std::vector<std::string>& arguments) {
        impl->engine.reset_budget();
        return impl->engine.run_entry(arguments);
    }
    Value Runtime::create(const std::string& name) {
        impl->engine.reset_budget();
        return impl->engine.make(name);
    }
    Value Runtime::call(const std::string& name,const std::string& method,std::vector<Value> args) {
        auto& e=impl->engine;
        e.reset_budget();
        auto* c=e.program.find_class(name);
        if(!c)throw std::runtime_error("unknown class '"+name+"'");
        const auto& m=e.exported_method(*c,method,args,true);
        return e.invoke(*c,m,nullptr,std::move(args));
    }
    Value Runtime::call(Value& object,const std::string& method,std::vector<Value> args) {
        auto& e=impl->engine;
        e.reset_budget();
        auto* c=e.program.find_class(object.type);
        if(!c)throw std::runtime_error("unknown class '"+object.type+"'");
        const auto& m=e.exported_method(*c,method,args,false);
        return e.invoke(*c,m,&object,std::move(args));
    }
    namespace detail {
        int execute_program_nested(std::string_view source,ExecutionOptions options,const PreprocessorOptions& preprocessing,
                                   std::istream& input,std::ostream& output,SharedExecutionBudget budget) {
            Engine child(compile_source(source,"<runtime program>",true,preprocessing),options,input,output,
                         std::make_shared<State>(std::move(budget)));
            return child.run_entry();
        }
        int execute_statements_nested(std::string_view source,ExecutionOptions options,const PreprocessorOptions& preprocessing,
                                      std::istream& input,std::ostream& output,SharedExecutionBudget budget) {
            return execute_program_nested(statement_program(source),options,preprocessing,input,output,std::move(budget));
        }
        int execute_file_nested(const std::filesystem::path& path,ExecutionOptions options,const PreprocessorOptions& preprocessing,
                                std::istream& input,std::ostream& output,SharedExecutionBudget budget) {
            Engine child(load(path,true,preprocessing),options,input,output,std::make_shared<State>(std::move(budget)));
            return child.run_entry();
        }
    }
    int execute_program(std::string_view source,ExecutionOptions options,const PreprocessorOptions& preprocessing) {
        Runtime runtime(compile_source(source,"<runtime program>",true,preprocessing),options);
        return runtime.run();
    }
    int execute_statements(std::string_view source,ExecutionOptions options,const PreprocessorOptions& preprocessing) {
        return execute_program(statement_program(source),options,preprocessing);
    }
    int execute_file(const std::filesystem::path& path,ExecutionOptions options,const PreprocessorOptions& preprocessing) {
        Runtime runtime(load(path,true,preprocessing),options);
        return runtime.run();
    }
}
// namespace bolt
