#include <algorithm>
#include <bolt/Frontend.hpp>
#include <bolt/Version.hpp>
#include <bolt/native/Layout.hpp>
#include <bolt/Types.hpp>
#include <bolt/Value.hpp>
#include <iomanip>
#include <limits>
#include <sstream>
#include <map>
#include <functional>
namespace bolt {
    namespace {
        std::string quote(const std::string& s) {
            std::ostringstream out;
            out<<'"';
            for(unsigned char c:s) {
                if(c=='"'||c=='\\')out<<'\\'<<static_cast<char>(c);
                else if(c>=32&&c<127)out<<static_cast<char>(c);
                else out<<'\\'<<std::oct<<std::setw(3)<<std::setfill('0')<<static_cast<unsigned int>(c)<<std::dec;
            }
            out<<'"';
            return out.str();
        }
        std::string mangle(const std::string& s) {
            const char* hex="0123456789abcdef";
            std::string r;
            for(unsigned char c:s) {
                if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9'))r+=static_cast<char>(c);
                else {
                    r+="_x";
                    r+=hex[c>>4];
                    r+=hex[c&15];
                    r+='_';
                }
            }
            return r;
        }
        // Built-in scalar arithmetic can throw but cannot replace an already
        // evaluated string. User calls, overloads, accessors and static
        // initializers keep the owning snapshot required by evaluation order.
        bool readonly_argument(const Expr& e) {
            using K=Expr::Kind; using B=Expr::Binding;
            if (e.kind == K::Literal || (e.kind == K::Name && e.binding == B::Local)) return true;
            bool read = (e.kind == K::Name || e.kind == K::Member) && e.binding == B::Field && !e.is_static;
            if (e.kind == K::Index && e.binding == B::None)
                read = is_array(e.owner) || collection_kind(e.owner) == "List" || e.owner == "string";
            if (e.kind == K::Binary && e.binding == B::None)
                read = is_numeric(e.type) || e.type == "bool" || e.type == "char";
            if (e.kind == K::Unary && e.binding == B::None)
                read = e.text == "+" || e.text == "-" || e.text == "~" || e.text == "!";
            return read && std::all_of(e.args.begin(),e.args.end(),[](const auto& child) { return readonly_argument(*child); });
        }
        thread_local const Program* type_context=nullptr;
        std::string cpp_type(const std::string& t);
        std::string class_type(const std::string& t) {
            if(type_context)if(const auto* c=type_context->find_class(t);c&&c->is_native) {
                const auto args=generic_args(c->native_cpp);auto name=generic_base(c->native_cpp);
                if(!args.empty()){name+="<";for(std::size_t i=0;i<args.size();++i){if(i)name+=", ";name+=cpp_type(args[i]);}name+=">";}return name;
            }
            return "bC_"+mangle(t);
        }
        std::string interface_value_type(const std::string& t) {return "bI_"+mangle(t);}
        std::string cpp_type(const std::string& t) {
            if(is_optional(t))return "bolt::native::Optional<"+cpp_type(element_type(t))+">";
            if(is_reference(t)||is_pointer(t)) {
                auto element=element_type(t);
                return (type_context&&is_interface(*type_context,element)?class_type(element):cpp_type(element))+t.back();
            }
            if(type_context&&is_interface(*type_context,t))return interface_value_type(t);
            if(is_array(t))return array_rank(t)==1?"bolt::native::Array<"+cpp_type(element_type(t))+">":"bolt::native::RectArray<"+cpp_type(element_type(t))+", "+std::to_string(array_rank(t))+">";
            if(is_collection(t)) {
                std::string r=(collection_kind(t)=="List"?"bolt::memory::Buffer": "bolt::native::"+collection_kind(t))+"<";
                for(const auto& a:generic_args(t)) {
                    if(r.back()!='<')r+=", ";
                    r+=cpp_type(a);
                }
                return r+">";
            }
            if(type_context)if(const auto* e=type_context->find_enum(t);e&&!e->native_cpp.empty())return e->native_cpp;
            if(t=="char")return "char32_t";
            if(t=="int")return "std::int32_t";
            if(t=="short")return "std::int16_t";
            if(t=="long")return "std::int64_t";
            if(t=="byte")return "std::uint8_t";
            if(t=="ushort")return "std::uint16_t";
            if(t=="uint")return "std::uint32_t";
            if(t=="ulong")return "std::uint64_t";
            if(t=="string")return "bolt::native::String";
            if(t=="Arena")return "bolt::memory::Arena";
            if(t=="null")return "std::nullptr_t";
            if(t=="bool"||t=="float"||t=="double"||t=="void")return t;
            return class_type(t);
        }
        std::string default_value(const std::string& t) {
            return is_pointer(t) ? "nullptr" : cpp_type(t)+"{}";
        }
        std::string qualified_type(const std::string& t,bool constant) {
            if(!constant)return cpp_type(t);
            if(is_reference(t)){auto inner=element_type(t);return (type_context&&is_interface(*type_context,inner)?"const "+class_type(inner):qualified_type(inner,true))+"&";}
            if(is_pointer(t))return cpp_type(t)+" const";
            return "const "+cpp_type(t);
        }
        std::string method_name(const Method& m) {
            return !m.native_name.empty()?m.native_name:m.is_extern?m.name:"bM_"+mangle(m.id);
        }
        std::string accessor_const(const Method& m){return m.readonly_receiver&&!m.is_static?" const":"";}
        std::string field_name(const std::string& n) {
            return "bF_"+mangle(n);
        }
        std::string local_name(std::size_t id,const std::string& n) {
            return "b_v"+std::to_string(id)+"_"+mangle(n);
        }
        std::string cast_to(const std::string& t,const std::string& e) {
            if(t=="Arena"||t=="void")return e;
            if(is_reference(t))return "static_cast<"+cpp_type(t)+">("+e+")";
            return "bolt::native::cast<"+cpp_type(t)+">("+e+")";
        }
        std::string literal(const Expr& e) {
            if(e.type=="null")return "nullptr";
            auto v=literal_value(e);
            if(v.type=="string")return "bolt::native::String("+quote(v.as_string())+", "+std::to_string(v.as_string().size())+")";
            if(v.type=="char")return "char32_t{"+std::to_string(v.as_int())+"}";
            if(v.type=="bool")return v.as_bool()?"true":"false";
            if(is_integer(v.type)) {
                if(!is_unsigned(v.type)&&v.as_int()==std::numeric_limits<std::int64_t>::min())return "std::numeric_limits<std::int64_t>::min()";
                return cpp_type(v.type)+"{"+v.to_string()+(is_unsigned(v.type)?"ULL}":"LL}");
            }
            std::ostringstream out;
            out.imbue(std::locale::classic());
            if(v.type=="float")out<<std::setprecision(std::numeric_limits<float>::max_digits10)<<std::get<float>(v.data);
            else out<<std::setprecision(std::numeric_limits<double>::max_digits10)<<std::get<double>(v.data);
            auto s=out.str();
            if(s.find_first_of(".eE")==std::string::npos)s+=".0";
            if(v.type=="float")s+="f";
            return s;
        }
        class Emitter {
            const Program& program;
            std::ostringstream out;
            int indent=0;
            bool field_initializer=false;
            std::string method_return;
            bool async_instance=false;
            std::size_t temporary=0;
            std::string self()const{return async_instance?"b_async_self":"(*this)";}
            std::string capture()const {
                return field_initializer?"[]":"[&]";
            }
            const Token* pending_source=nullptr;
            bool previous_line_was_bolt=false;
            std::size_t physical_line=0;
            std::string mapped_file;
            std::string mapped_label;
            void source(const Token& token) { pending_source=&token; }
            void line(const std::string& s= {}) {
                // A source token identifies one generated line. Switch back before
                // the compiler assigns later glue successive (possibly absent) Bolt lines.
                if(pending_source) {
                    const auto& location=pending_source->at;
                    if(mapped_file!=location.file||mapped_label.empty()) {
                        mapped_file=location.file;
                        // Clang rejects numeric byte escapes in #line filenames.
                        // Keep UTF-8 intact and substitute paths containing control bytes.
                        const bool control=std::any_of(mapped_file.begin(),mapped_file.end(),
                            [](unsigned char c){return c<32||c==127;});
                        std::ostringstream label;
                        label<<std::quoted(control?std::string("<bolt source>"):mapped_file);
                        mapped_label=label.str();
                    }
                    out<<"#line "<<location.line<<' '<<mapped_label<<'\n';
                    ++physical_line;
                } else if(previous_line_was_bolt) {
                    out<<"#line "<<physical_line+2<<" \"<bolt generated>\"\n";
                    ++physical_line;
                }
                previous_line_was_bolt=pending_source!=nullptr;
                pending_source=nullptr;
                out<<std::string(static_cast<std::size_t>(indent)*4,' ')<<s<<'\n';
                ++physical_line;
            }
            std::string arithmetic(const std::string& op,const std::string& a,const std::string& b,const std::string& x,const std::string& y) {
                if(op=="&"||op=="|"||op=="^"||op=="<<"||op==">>")if(const auto* en=program.find_enum(a)){
                    const auto rhs=program.find_enum(b)?cast_to(en->underlying,y):y;
                    return cast_to(a,"bolt::native::bitwise<"+cpp_type(en->underlying)+">("+quote(op)+", "+cast_to(en->underlying,x)+", "+rhs+")");
                }
                if(op=="&"||op=="|"||op=="^"||op=="<<"||op==">>")return "bolt::native::bitwise<"+cpp_type(op=="<<"||op==">>"?promoted(a):common_numeric(a,b))+">("+quote(op)+", "+x+", "+y+")";
                std::string name=op=="+"?"add":op=="-"?"sub":op=="*"?"mul":op=="/"?"div":"mod";
                return "bolt::native::"+name+"<"+cpp_type(common_numeric(a,b))+">("+x+", "+y+")";
            }
            std::string receiver(const Expr& member) {
                auto s=expression(*member.args[0]);
                return member.arrow?"bolt::native::deref("+s+")":s;
            }
            std::string add_arguments(std::string& body,const std::vector<ExprPtr>& args,std::size_t start,const std::vector<std::string>& types,const std::vector<bool>& constants= {
            }) {
                std::string passed;
                for(std::size_t i=start;i<args.size();++i) {
                    const auto n="b_arg"+std::to_string(i-start);
                    const auto& t=types.at(i-start);
                    const bool constant=!constants.empty()&&constants[i-start];
                    if(is_reference(t)) {
                        body+=qualified_type(t,constant)+" "+n+" = ("+expression(*args[i])+"); ";
                        if(!passed.empty())passed+=", ";
                        passed+=n;
                    } else {
                        body+="auto "+n+" = "+cast_to(t,expression(*args[i]))+"; ";
                        if(!passed.empty())passed+=", ";
                        passed+="std::move("+n+")";
                    }
                }
                return passed;
            }
            std::string construct(const Expr& e,bool allocate) {
                const auto t=allocate?element_type(e.type):e.type;
                const auto* c=program.find_class(t);
                const auto* m=c&&!e.resolved.empty()?method_by_id(*c,e.resolved):nullptr;
                std::string body="("+capture()+"() -> "+cpp_type(e.type)+" { ";
                if(allocate&&e.arena)body+="auto& b_arena = ("+expression(*e.arena)+"); ";
                std::vector<std::string> types;
                std::vector<bool> constants;
                if(m)for(const auto& p:m->parameters) {
                    types.push_back(p.type);
                    constants.push_back(p.constant);
                } else for(std::size_t i=allocate?0:1;i<e.args.size();++i)types.push_back(t);
                auto passed=add_arguments(body,e.args,allocate?0:1,types,constants);
                if(allocate&&e.arena)return body+"return bolt::native::arena_new<"+cpp_type(t)+">(b_arena"+(passed.empty()?"":", "+passed)+"); }())";
                return body+"return "+(allocate?"new ":"")+cpp_type(t)+"("+passed+"); }())";
            }
            std::string kernel_method(const std::string& name) const {
                const auto* kernel=program.find_class("Bolt.Runtime.ThreadKernel");
                const auto* method=kernel?find_method(*kernel,name):nullptr;
                if(!method)throw std::runtime_error("missing Bolt thread operation: "+name);
                return class_type(kernel->name)+"::"+method_name(*method);
            }
            void thread_adapters() {
                const auto handle=class_type("Bolt.Runtime.ThreadHandle"), state=class_type("Bolt.Runtime.ThreadResource");
                const auto ptr=field_name("Pointer");
                const auto storage=class_type("Bolt.Runtime.LockStorage");
                line("static_assert(offsetof("+state+","+field_name("References")+") % 8 == 0 && offsetof("+state+","+field_name("Published")+") % 8 == 0);");
                line("#ifdef _WIN32");
                line("static_assert(sizeof("+storage+") >= sizeof(SRWLOCK) && alignof("+storage+") >= alignof(SRWLOCK));");
                line("static_assert(sizeof("+storage+") >= sizeof(CONDITION_VARIABLE) && alignof("+storage+") >= alignof(CONDITION_VARIABLE));");
                line("#else");
                line("static_assert(sizeof("+storage+") >= sizeof(pthread_mutex_t) && alignof("+storage+") >= alignof(pthread_mutex_t));");
                line("static_assert(sizeof("+storage+") >= sizeof(pthread_cond_t) && alignof("+storage+") >= alignof(pthread_cond_t));");
                const auto clock=class_type("Bolt.Runtime.Threading.TimeSample");
                line("static_assert(sizeof("+clock+") == sizeof(timespec) && offsetof("+clock+","+field_name("Nanoseconds")+") == offsetof(timespec,tv_nsec));");
                line("#endif");
                if(const auto* owner=program.find_class("Bolt.Runtime.ThreadHandle");owner && emitted_class(*owner)) {
                    line("inline "+handle+"::"+handle+"(const "+handle+"& other) : "+ptr+"(other."+ptr+") { "+kernel_method("Retain")+"("+ptr+"); }");
                    line("inline "+handle+"& "+handle+"::operator=(const "+handle+"& other) {");
                    line("    auto* next=other."+ptr+"; "+kernel_method("Retain")+"(next); auto* old=std::exchange("+ptr+",next); "+kernel_method("Release")+"(old); return *this;");
                    line("}");
                    line("inline "+handle+"::~"+handle+"() { "+kernel_method("Release")+"("+ptr+"); }");
                }
                line("#ifdef _WIN32");
                line("inline DWORD WINAPI bolt_thread_entry(void* argument) {");
                line("#else");
                line("inline void* bolt_thread_entry(void* argument) {");
                line("#endif");
                line("    auto* state=static_cast<"+state+"*>(argument);");
                line("    auto* previous=bolt::platform::ExecutionMemory::Activate(state->"+field_name("Context")+");");
                line("    "+kernel_method("Run")+"(state);");
                line("    bolt::platform::ExecutionMemory::Activate(previous);");
                line("    return 0;");
                line("}");
                line("template<class F> inline "+state+"* bolt_capture(F work) {");
                line("    auto invoke=+[]("+state+"* state, std::uint8_t* slot){");
                line("        auto& body=*reinterpret_cast<F*>(slot);");
                line("        if constexpr(std::is_void_v<decltype(body())>)body();");
                line("        else { auto result=body(); "+kernel_method("StoreResult")+"(state,bolt::native::erased_descriptor<decltype(result)>(),reinterpret_cast<std::uint8_t*>(&result)); }");
                line("    };");
                line("    return "+kernel_method("Capture")+"(bolt::native::erased_descriptor<F>(),reinterpret_cast<std::uint8_t*>(&work),reinterpret_cast<void*>(invoke));");
                line("}");
                line("template<class S,class F> inline S bolt_schedule(F body) {");
                line("    auto* state=bolt_capture(std::move(body)); "+kernel_method("Schedule")+"(state);");
                line("    S result; result."+field_name("Handle")+"."+ptr+"=state; return result;");
                line("}");
            }
            struct ExpressionCode {
                std::string text;
                // A result type marks statements requiring an immediately invoked lambda.
                std::string block_type;
                ExpressionCode(std::string value, std::string result_type = {})
                    : text(std::move(value)), block_type(std::move(result_type)) {}
            };
            static ExpressionCode block_expression(std::string result_type, std::string statements) {
                return {std::move(statements), std::move(result_type)};
            }
            ExpressionCode call(const Expr& e) {
                using B=Expr::Binding;
                const auto& target=*e.args[0];
                if(e.binding==B::Construct)return construct(e,false);
                if(e.binding==B::Cast) {
                    // A C-style explicit pointer cast preserves valid base
                    // adjustments and also permits reinterpreting raw buffers.
                    if(is_pointer(e.type)&&is_pointer(e.args[1]->type))return "(("+cpp_type(e.type)+")("+expression(*e.args[1])+"))";
                    if(e.type=="string" && primitive_formatter(e.args[1]->type).owner)
                        return "bolt_generated::primitive_text("+expression(*e.args[1])+")";
                    return e.type=="string"?"bolt::native::text("+expression(*e.args[1])+")":cast_to(e.type,expression(*e.args[1]));
                }
                if(e.binding==B::Builtin) {
                    if(e.owner=="String") {
                        std::string body="("+capture()+"() -> "+cpp_type(e.type)+" { ";
                        std::string args;
                        // Capture earlier values before an argument can mutate them.
                        // Reading a local or literal cannot change a previously evaluated value.
                        std::size_t last_effect = 0;
                        for (std::size_t i = 1; i < e.args.size(); ++i) {
                            const auto& argument = *e.args[i];
                            if (!readonly_argument(argument)) last_effect = i;
                        }
                        if (!e.is_static) {
                            body += std::string(last_effect ? "auto " : "auto&& ") +
                                    "b_receiver = (" + receiver(target) + "); ";
                            args = "b_receiver";
                        }
                        for (std::size_t i = 1; i < e.args.size(); ++i) {
                            const auto name = "b_arg" + std::to_string(i);
                            auto value = expression(*e.args[i]);
                            if (e.args[i]->type != e.type_args[i-1]) value = cast_to(e.type_args[i-1],value);
                            body += std::string(i < last_effect ? "auto " : "auto&& ") +
                                    name + " = (" + value + "); ";
                            if (!args.empty()) args += ", ";
                            args += name;
                        }
                        return body+"return bolt::library::Strings::"+e.text+"("+args+"); }())";
                    }
                    if(e.owner=="Array"&&e.text=="GetLength") {
                        return "("+capture()+"() -> std::int32_t { auto&& b_receiver = ("+receiver(target)+"); auto b_dimension = ("+expression(*e.args[1])+"); return b_receiver.GetLength(b_dimension); }())";
                    }
                    if(e.owner=="Arena") {
                        if(e.text=="Reset"||e.text=="Dispose")return "("+receiver(target)+")."+(e.text=="Reset"?"reset":"dispose")+"()";
                        std::string body="("+capture()+"() -> bolt::memory::Arena { ";
                        std::vector<std::string> types(e.args.size()-1,"ulong");
                        add_arguments(body,e.args,1,types);
                        return body+"return bolt::memory::Arena(bolt::native::checked_size(b_arg0), bolt::native::checked_size("+(e.text=="Fixed"?"b_arg0":"b_arg1")+"), "+(e.text=="Fixed"?"false":"true")+"); }())";
                    }
                    if(e.owner=="Intrinsic"&&e.text=="Reinterpret")return "bolt::native::reinterpret_bits<"+cpp_type(e.type)+">("+expression(*e.args[1])+")";
                    if(e.owner=="Runtime") {
                        auto name=e.text=="Execute"?"execute_statements":e.text=="ExecuteProgram"?"execute_program":e.text=="ExecuteFile"?"execute_file":e.text=="ExecuteJit"?"jit_statements":e.text=="ExecuteProgramJit"?"jit_program":"jit_file";
                        auto arg=expression(*e.args[1]);
                        if(e.text=="ExecuteFile"||e.text=="ExecuteFileJit")arg="bolt::path_from_utf8("+arg+")";
                        return std::string("bolt::")+name+"("+arg+")";
                    }
                    if(e.owner=="Type")return "bolt::native::String("+quote(e.type_args[0])+")";
                    throw Error(e.token.at,"unsupported builtin call: "+e.owner+"."+e.text);
                }
                if(e.binding==B::Collection) {
                    std::string body="auto&& b_receiver = ("+receiver(target)+"); ";
                    auto a=generic_args(e.owner);
                    auto kind=collection_kind(e.owner);
                    std::vector<std::string> types;
                    if(e.text=="Swap")types={e.owner+"&"};
                    else if(e.text=="Insert")types= {
                        "int",a[0]
                    };
                    else if(e.text=="RemoveAt"||e.text=="Reserve"||e.text=="Truncate")types= {
                        "int"
                    };
                    else if(e.text=="TryDequeue")types= {
                        a[0]+"&"
                    };
                    else if(e.text=="TryGetValue")types= {
                        a[0],a[1]+"&"
                    };
                    else if(kind=="Dictionary"&&(e.text=="Add"||e.text=="Set"))types= {
                        a[0],a[1]
                    };
                    else if(e.args.size()>1)types= {
                        a[0]
                    };
                    std::vector<bool> constness;
                    if(e.text=="AppendRange"){types={a[0]+"[]&","int","int"};constness={true,false,false};}
                    if(e.text=="AppendBuffer"){types={"int","int",e.owner+"&"};constness={false,false,true};}
                    auto args=add_arguments(body,e.args,1,types,constness);
                    return block_expression(cpp_type(e.type), body+"return b_receiver."+e.text+"("+args+");");
                }
                const auto& m=*method_by_id(*program.find_class(e.owner),e.resolved);
                std::string body;
                std::string callee;
                if(!m.native_abi.empty())callee=method_name(m).starts_with("::")?method_name(m):"::"+method_name(m);
                else if(m.is_static)callee=class_type(e.owner)+"::"+method_name(m);
                else {
                    body+="auto&& b_receiver = ("+(target.kind==Expr::Kind::Member?receiver(target):self())+"); ";
                    callee="b_receiver."+method_name(m);
                }
                if(m.is_extern&&!m.specialization_types.empty()) {
                    callee+="<";for(std::size_t i=0;i<m.specialization_types.size();++i){if(i)callee+=", ";callee+=cpp_type(m.specialization_types[i]);}callee+=">";
                }
                std::vector<std::string> types;
                std::vector<bool> constants;
                for(const auto& p:m.parameters) {
                    types.push_back(p.type);
                    constants.push_back(p.constant);
                }
                if(!m.operator_symbol.empty())for(auto& t:types)t=referred(t);
                auto args=add_arguments(body,e.args,1,types,constants);
                if(m.native_abi=="C") {
                    args.clear();for(std::size_t i=0;i<m.parameters.size();++i){if(i)args+=", ";const auto& p=m.parameters[i];const auto type=referred(p.type);const auto v="b_arg"+std::to_string(i);
                        if(type=="string")args+="bolt::library::NativeInterop::Utf8("+v+")";
                        else if(is_array(type))args+="("+v+".Count()==0 ? nullptr : &"+v+".At(0))";
                        else if(is_reference(p.type))args+="&"+v;
                        else args+=v;
                    }
                    // fcntl's third argument is variadic (stack-passed on Darwin ARM64).
                    // The platform binding supplies the integer form of that argument.
                    if(callee=="::fcntl" && m.token.at.file=="<bolt-stdlib>/System/ProcessApi.bolt")
                        return block_expression(cpp_type(m.type),body+"return "+callee+"("+args+");");
#if defined(BOLT_LINUX_MUSL)
                    // Alpine's fortified C declarations can be overload sets. Keep the
                    // ABI conversions for ordinary functions. For a fortified overload,
                    // let the C++ compiler select the call using its real argument types.
                    const std::string forwarded="std::forward<decltype(b_values)>(b_values)...";
                    const auto adapted="bolt::native::ffi_call("+callee+", "+forwarded+")";
                    const auto call="([](auto&&... b_values) -> decltype(auto) { if constexpr (requires { "+
                        adapted+"; }) return "+adapted+"; else return "+callee+"("+forwarded+"); }("+args+"))";
#else
                    const auto call="bolt::native::ffi_call("+callee+(args.empty()?"":", "+args)+")";
#endif
                    if(m.type=="string")return block_expression(cpp_type(m.type), body+"auto* b_utf8="+call+"; return bolt::library::NativeInterop::CopyUtf8(b_utf8);");
                    if(m.type=="void")return block_expression(cpp_type(m.type), body+call+"; return;");
                    if(is_reference(m.type))return block_expression(cpp_type(m.type), body+"auto* b_result="+call+"; if(!b_result) throw std::runtime_error(\"native function returned null for a Bolt reference\"); return *b_result;");
                    return block_expression(cpp_type(m.type), body+"return bolt::native::ffi_result<"+cpp_type(m.type)+">("+call+");");
                }
                if(e.owner=="Bolt.Runtime.ExecutionMemory") {
                    if(m.name=="RunEntry")return block_expression("void",body+"bolt_thread_entry(b_arg0);");
                    if(m.name=="Entry")return std::string("reinterpret_cast<void*>(&bolt_thread_entry)");
                    if(m.name=="Invoke")return block_expression("void",body+"reinterpret_cast<void(*)("+class_type("Bolt.Runtime.ThreadResource")+"*, std::uint8_t*)>(b_arg0)(b_arg1,b_arg2);");
                }
                if(e.owner=="Bolt.Runtime.ThreadCallback") {
                    if(m.name=="Cancel")return block_expression("void",body+kernel_method("Release")+"(static_cast<"+class_type("Bolt.Runtime.ThreadResource")+"*>(b_arg0));");
                    if(m.name=="Entry")return std::string("reinterpret_cast<void*>(&bolt_thread_entry)");
                }
                if(!e.callback_method.empty()) {
                    const auto& callback=*method_by_id(*program.find_class(e.callback_owner),e.callback_method);
                    if(e.owner=="Bolt.Runtime.ThreadCallback")
                        return block_expression(cpp_type(m.type), body+"return bolt_capture([b_handler = b_arg0]() mutable { b_handler."+method_name(callback)+"(); });");
                    const auto component=cpp_type(generic_args(e.owner).front());
                    const auto capture=m.name=="ConnectRef"?"b_handler = &b_arg0":"b_handler = std::move(b_arg0)";
                    const auto access=m.name=="ConnectRef"?"b_handler->":"b_handler.";
                    return block_expression(cpp_type(m.type), body+"return b_receiver.Connect(["+capture+"](bolt::library::ecs::Registry& b_registry, bolt::library::ecs::Entity b_entity, "+component+"& b_component) mutable { "+access+method_name(callback)+"(b_registry,b_entity,b_component); });");
                }
                return block_expression(cpp_type(m.type), body+"return "+callee+"("+args+");");
            }
            std::string at(const Location& loc) const {
                return "bolt::NativeSourceLocation{"+quote(loc.file)+", "+std::to_string(loc.line)+", "+std::to_string(loc.column)+", "+std::to_string(loc.length)+"}";
            }
            std::string catch_at(const Location& loc) const {
                return " catch (const std::exception&) { bolt::library::rethrow_checked("+at(loc)+"); }";
            }
            std::string guarded_body(const Location& loc, const std::string& type, const std::string& body) {
                const auto lambda = capture() + "() -> " + type + " { ";
                if (program.cpp_features.empty())
                    return "bolt::checked_at(" + at(loc) + ", " + lambda + body + " })";
                // Successful evaluations do not need a source location or an extra function call.
                return "(" + lambda + "try { " + body + " }" + catch_at(loc) + " }())";
            }
            std::string guarded(const Location& loc, const std::string& type, const std::string& code) {
                return guarded_body(loc, type, "return (" + code + ");");
            }
            std::string expression(const Expr& e) {
                using K=Expr::Kind;
                using B=Expr::Binding;
                // Keep primitive operations visible to the host optimizer. In
                // particular MSVC does not inline many lambdas containing EH.
                const auto local = [](const Expr& v) { return v.kind==K::Name && v.binding==B::Local; };
                const auto atom = [&](const Expr& v) { return local(v) || v.kind==K::Literal; };
                // These expressions either cannot fail or already report
                // their own source location. Properties with implicit
                // conversions still need the surrounding expression handler.
                const auto located = [&](const Expr& v) {
                    return atom(v) || v.kind==K::Call || v.kind==K::Index || v.kind==K::Binary;
                };
                if (e.binding==B::None && !e.lvalue && e.type=="int" && e.args.size()==2 &&
                    ((e.kind==K::Binary && (e.text=="+" || e.text=="-" || e.text=="*")) ||
                     (e.kind==K::Assign && local(*e.args[0]) && (e.text=="+=" || e.text=="-=" || e.text=="*="))) &&
                    promoted(e.args[0]->type)=="int" && promoted(e.args[1]->type)=="int" &&
                    located(*e.args[0]) && located(*e.args[1])) {
                    const auto op=e.text.substr(0,1);
                    const auto fail=guarded_body(e.token.at,"std::int32_t","throw std::runtime_error(\"integer overflow in "+op+"\");");
                    const auto left=expression(*e.args[0]);
                    const auto target=e.kind==K::Assign ? left+" = " : "";
                    return "("+capture()+"() -> std::int32_t { const auto b_left=("+left+"); const auto b_right=("+
                        expression(*e.args[1])+"); return "+target+"bolt::native::checked_int32<'"+op+
                        "'>(b_left,b_right,[] { return "+fail+"; }); }())";
                }
                if (!field_initializer && e.kind==K::Index && e.binding==B::None && array_rank(e.owner)==1 &&
                    is_numeric(e.type) && e.args.size()==2 && local(*e.args[0]) && e.args[1]->type=="int" &&
                    located(*e.args[1])) {
                    const auto array=expression(*e.args[0]);
                    const auto type=e.lvalue ? qualified_type(e.type,e.constant)+"&" : cpp_type(e.type);
                    const auto failure=guarded(e.token.at,type,array+".At(b_key)");
                    return "("+capture()+"() -> "+type+" { auto b_key=("+expression(*e.args[1])+
                        "); if(auto* b_item="+array+".TryAt(b_key)) return *b_item; return "+failure+"; }())";
                }
                if (e.binding==B::None && e.kind==K::Binary && e.args.size()==2) {
                    const auto& a=*e.args[0];
                    const auto& b=*e.args[1];
                    // One inert operand can be reordered with a read-only
                    // subexpression whose own handler covers any failure.
                    const auto read = [&](const Expr& v) {
                        return atom(v) || (v.kind==K::Binary && v.binding==B::None && readonly_argument(v));
                    };
                    if (a.type==b.type && (is_numeric(a.type) || a.type=="bool") &&
                        ((atom(a) && read(b)) || (atom(b) && read(a)))) {
                        const bool comparison=e.text=="<" || e.text=="<=" || e.text==">" ||
                                              e.text==">=" || e.text=="==" || e.text=="!=";
                        const bool bits=is_integer(a.type) && (e.text=="&" || e.text=="|" || e.text=="^");
                        if (comparison || bits)
                            return "("+expression(a)+" "+e.text+" "+expression(b)+")";
                    }
                }
                if (e.binding==B::None && e.kind==K::Assign && e.text=="=" && !e.lvalue &&
                    local(*e.args[0]) && is_numeric(e.type) && e.args[1]->type==e.type &&
                    (e.args[1]->kind==K::Call || e.args[1]->kind==K::Binary || atom(*e.args[1]))) {
                    // The RHS handles its own exceptions. Copying the resulting
                    // scalar into a named local cannot raise another one.
                    return "static_cast<"+cpp_type(e.type)+">("+expression(*e.args[0])+" = "+expression(*e.args[1])+")";
                }
                if (e.binding==B::None && (e.kind==K::Unary || e.kind==K::Postfix) &&
                    (e.text=="++" || e.text=="--") && !e.lvalue && is_integer(e.type) &&
                    e.args[0]->type==e.type && local(*e.args[0])) {
                    const auto operand=expression(*e.args[0]);
                    const auto type=cpp_type(e.type);
                    const auto limit="std::numeric_limits<"+type+">::"+(e.text=="++"?"max()":"min()");
                    const auto update=e.kind==K::Postfix?operand+e.text:e.text+operand;
                    // Reproduce the checked promotion and narrowing at the
                    // boundary without taking the address of the live counter.
                    const std::string checked_op=e.text=="++" ? "bolt::native::add<" : "bolt::native::sub<";
                    const auto boundary=cast_to(e.type,checked_op+cpp_type(promoted(e.type))+">("+limit+", 1)");
                    return "("+operand+" == "+limit+" ? "+guarded(e.token.at,type,boundary)+" : "+update+")";
                }
                auto code=expression_raw(e);
                const bool checked=e.kind==K::OptionalValue||e.kind==K::Call||e.kind==K::Default||e.kind==K::Binary||e.kind==K::Assign||e.kind==K::Unary||e.kind==K::Postfix||e.kind==K::Index||e.kind==K::New||e.kind==K::Delete||(e.kind==K::Member&&e.arrow);
                if (!checked) return std::move(code.text);
                auto type=e.lvalue?qualified_type(e.type,e.constant)+(is_reference(e.type)?"":"&"):cpp_type(e.type);
                if(e.lvalue&&is_interface(program,e.type)&&(is_reference(e.declared_type)||(e.kind==K::Unary&&e.text=="*")))type=qualified_type(e.type+"&",e.constant);
                if (!code.block_type.empty()) {
                    // Preserve any conversion between a method's declared result and the checked expression.
                    if (code.block_type == type) return guarded_body(e.token.at, type, code.text);
                    code.text = "(" + capture() + "() -> " + code.block_type + " { " + code.text + " }())";
                }
                return guarded(e.token.at,type,code.text);
            }
            ExpressionCode expression_raw(const Expr& e) {
                using K=Expr::Kind;
                using B=Expr::Binding;
                if(e.kind==K::SizeOf)return "std::int32_t{"+e.resolved+"}";
                if(e.kind==K::OptionalHas)return "("+expression(*e.args[0])+").HasValue()";
                if(e.kind==K::OptionalValue)return "("+expression(*e.args[0])+").Value()";
                if(e.kind==K::Literal)return literal(e);
                if(e.kind==K::Default)return default_value(e.type);
                if(e.kind==K::Let){
                    const auto name=local_name(e.local_id,e.text);
                    const auto init=is_reference(e.declared_type)?"("+expression(*e.args[0])+")":cast_to(e.declared_type,expression(*e.args[0]));
                    return "("+capture()+"() -> "+cpp_type(e.type)+" { "+cpp_type(e.declared_type)+" "+name+" = "+init+"; return "+expression(*e.args[1])+"; }())";
                }
                if(e.binding==B::EnumValue)return cpp_type(e.type)+"::"+(program.find_enum(e.type)->native_cpp.empty()?"b_"+mangle(e.text):e.text);
                if(e.binding==B::Property) {
                    if(e.owner=="String"&&e.text=="Empty")return std::string("bolt::native::String{}");
                    if(e.owner=="string"&&e.text=="ByteLength")return "bolt::native::cast<std::int32_t>(("+receiver(e)+").size())";
                    if(e.owner=="Arena") {
                        const auto name=e.text=="UsedBytes"?"used":e.text=="CapacityBytes"?"capacity":e.text=="PeakBytes"?"peak":"limit";
                        return "static_cast<std::uint64_t>(("+receiver(e)+")."+name+"())";
                    }
                    if(e.owner=="Math")return "static_cast<"+cpp_type(e.type)+">(bolt::native::"+e.text+")";
                    if(is_numeric(e.owner))return "std::numeric_limits<"+cpp_type(e.owner)+">::"+(e.text=="MaxValue"?"max":"lowest")+"()";
                    auto base=receiver(e);
                    if(is_array(e.owner)&&e.text=="Rank")return "("+base+").Rank()";
                    return e.owner=="string"?"bolt::native::cast<std::int32_t>(bolt::unicode::length("+base+"))":"("+base+")."+(e.text=="Capacity"?"Capacity()":"Count()");
                }
                if(e.kind==K::Name||e.kind==K::Member) {
                    if(e.binding==B::Local)return local_name(e.local_id,e.text);
                    if(e.binding==B::This)return self();
                    if(e.binding==B::Class)return class_type(e.owner);
                    if(e.binding==B::Field) {
                        if(const auto* owner=program.find_class(e.owner);owner&&owner->is_native) {
                            const auto* field=find_field(*owner,e.text);
                            const auto access=e.is_static?class_type(e.owner)+"::":e.kind==K::Name?"("+self()+").":"("+receiver(e)+").";
                            return access+(field->native_getter.empty()?e.text:field->native_getter+"()");
                        }
                        if(e.is_static)return class_type(e.owner)+"::"+field_name(e.text)+"()";
                        if(e.kind==K::Name)return "("+self()+")."+field_name(e.text);
                        return "("+receiver(e)+")."+field_name(e.text);
                    }
                }
                if(e.kind==K::Call)return call(e);
                if(e.kind==K::Pack) {
                    std::string body="("+capture()+"() -> void { ";
                    for(const auto& call:e.args)body+="(void)("+expression(*call)+"); ";
                    return body+"}())";
                }
                if(e.kind==K::Array) {
                    std::string result=cpp_type(e.type)+"{";
                    if(array_rank(e.type)>1) {
                        result+="std::array<std::size_t, "+std::to_string(array_rank(e.type))+">{";
                        for(auto dimension:e.dimensions) {
                            if(result.back()!='{')result+=", ";
                            result+=std::to_string(dimension);
                        }
                        result+="}, {";
                    }
                    for(const auto& v:e.args) {
                        if(result.back()!='{')result+=", ";
                        result+=cast_to(element_type(e.type),expression(*v));
                    }
                    return result+(array_rank(e.type)>1?"}}":"}");
                }
                if(e.kind==K::New) {
                    if(e.arrow) {
                        std::string body="("+capture()+"() -> "+cpp_type(e.type)+" { ";
                        std::string dimensions;
                        for(std::size_t axis=0;axis<e.args.size();++axis) {
                            const auto name="b_dimension"+std::to_string(axis);
                            body+="auto "+name+" = bolt::native::checked_dimension("+expression(*e.args[axis])+"); ";
                            if(!dimensions.empty())dimensions+=", ";
                            dimensions+=name;
                        }
                        if(array_rank(e.type)>1)dimensions="std::array<std::size_t, "+std::to_string(array_rank(e.type))+">{"+dimensions+"}";
                        return body+"return "+cpp_type(e.type)+"::Create("+dimensions+"); }())";
                    }
                    return construct(e,true);
                }
                if(e.kind==K::Delete)return "bolt::native::checked_delete("+expression(*e.args[0])+")";
                if(e.kind==K::Index) {
                    auto type=e.lvalue?qualified_type(e.type,e.constant)+"&":cpp_type(e.type);
                    const bool snapshot = e.owner == "string" && !readonly_argument(*e.args.at(1));
                    std::string body=std::string(snapshot ? "auto b_receiver = (" : "auto&& b_receiver = (")+expression(*e.args[0])+"); ";
                    std::string keys;
                    for(std::size_t axis=1;axis<e.args.size();++axis) {
                        auto key=expression(*e.args[axis]);
                        if(collection_kind(e.owner)=="Dictionary")key=cast_to(generic_args(e.owner)[0],key);
                        const auto name="b_key"+std::to_string(axis);
                        body+="auto "+name+" = ("+key+"); ";
                        if(!keys.empty())keys+=", ";
                        keys+=name;
                    }
                    if(is_pointer(e.owner))return block_expression(type, body+"if(!b_receiver)throw std::runtime_error(\"null pointer dereference\"); return b_receiver["+keys+"];");
                    if(e.owner=="string")return block_expression(type, body+"return bolt::unicode::at(b_receiver, bolt::native::checked_size("+keys+"));");
                    return block_expression(type, body+"return b_receiver."+(e.is_static?"GetOrAdd":"At")+"("+keys+");");
                }
                if(e.binding==B::Operator) {
                    const auto& m=*method_by_id(*program.find_class(e.owner),e.resolved);
                    std::string body="("+capture()+"() -> "+cpp_type(e.type)+" { auto& b_dest=("+expression(*e.args[0])+"); auto b_old=b_dest; ";
                    std::string args=cast_to(referred(m.parameters[0].type),"b_old");
                    if(e.args.size()==2){body+="auto b_rhs=("+expression(*e.args[1])+"); ";args+=", "+cast_to(referred(m.parameters[1].type),"b_rhs");}
                    body+="b_dest="+class_type(e.owner)+"::"+method_name(m)+"("+args+"); return "+(e.kind==K::Postfix?"b_old":"b_dest")+"; }())";
                    return body;
                }
                if(e.kind==K::Conditional)return "("+expression(*e.args[0])+" ? "+cast_to(e.type,expression(*e.args[1]))+" : "+cast_to(e.type,expression(*e.args[2]))+")";
                if(e.kind==K::Assign) {
                    const auto& a=*e.args[0];
                    const auto& b=*e.args[1];
                    if(e.text=="="&&a.kind==K::Index&&collection_kind(a.owner)=="Dictionary"){
                        auto body="auto&& b_receiver = ("+expression(*a.args[0])+"); ";
                        body+="auto b_key = "+cast_to(generic_args(a.owner)[0],expression(*a.args[1]))+"; ";
                        body+="auto b_rhs = "+cast_to(e.type,expression(b))+"; ";
                        return block_expression(cpp_type(e.type), body+"b_receiver.Set(b_key, b_rhs); return b_rhs;");
                    }
                    std::string body="auto& b_dest = ("+expression(a)+"); ";
                    if(e.text=="?" "?=")return block_expression(cpp_type(e.type), body+"if(b_dest==nullptr)b_dest="+cast_to(e.type,expression(b))+"; return b_dest;");
                    if(e.text=="+=" && a.type=="string") {
                        body+="auto b_text = bolt::native::text(b_dest); ";
                        auto rhs=expression(b);
                        if(b.type!="string")rhs="bolt::native::text("+rhs+")";
                        return block_expression(cpp_type(e.type),body+"b_text.append(std::string_view("+rhs+")); b_dest = b_text; return b_dest;");
                    }
                    if(e.text!="=")body+="auto b_old = b_dest; ";
                    body+="auto b_rhs = ("+expression(b)+"); ";
                    auto updated=e.text=="="?"b_rhs":arithmetic(e.text.substr(0,e.text.size()-1),a.type,b.type,"b_old","b_rhs");
                    return block_expression(cpp_type(e.type), body+"b_dest = "+cast_to(e.type,updated)+"; return b_dest;");
                }
                if(e.kind==K::Unary||e.kind==K::Postfix) {
                    auto operand=expression(*e.args[0]);
                    if(e.text=="await")return "("+operand+").GetResult()";
                    if(e.text=="~")if(const auto* en=program.find_enum(e.type))return cast_to(e.type,"bolt::native::bitwise<"+cpp_type(en->underlying)+">(\"^\", "+cast_to(en->underlying,operand)+", "+(is_unsigned(en->underlying)?"std::numeric_limits<"+cpp_type(en->underlying)+">::max()":cpp_type(en->underlying)+"{-1}")+")");
                    if(e.text=="~")return "bolt::native::bitwise<"+cpp_type(e.type)+">(\"^\", "+operand+", "+(is_unsigned(e.type)?"std::numeric_limits<"+cpp_type(e.type)+">::max()":cpp_type(e.type)+"{-1}")+")";
                    if(e.text=="&")return "(&("+operand+"))";
                    if(e.text=="*")return "bolt::native::deref("+operand+")";
                    if(e.text=="!")return "(!("+operand+"))";
                    if(e.text=="+")return cast_to(e.type,operand);
                    if(e.text=="-")return "bolt::native::sub<"+cpp_type(e.type)+">("+cpp_type(e.type)+"{0}, "+operand+")";
                    auto op=e.text=="++"?"add":"sub";
                    auto updated=std::string("bolt::native::")+op+"<"+cpp_type(promoted(e.type))+">(b_old, 1)";
                    return "("+capture()+"() -> "+cpp_type(e.type)+" { auto& b_dest = ("+operand+"); auto b_old = b_dest; b_dest = "+cast_to(e.type,updated)+"; return "+(e.kind==K::Postfix?"b_old":"b_dest")+"; }())";
                }
                if(e.kind==K::Binary) {
                    const auto& a=*e.args[0];
                    const auto& b=*e.args[1];
                    auto left=expression(a),right=expression(b);
                    if(e.text=="??"&&is_optional(a.type))return "("+capture()+"() -> "+cpp_type(e.type)+" { auto&& b_left=("+left+"); if(b_left.HasValue())return "+cast_to(e.type,is_optional(e.type)?"b_left":"b_left.Value()")+"; return "+cast_to(e.type,right)+"; }())";
                    if(e.text=="??")return "("+capture()+"() -> "+cpp_type(e.type)+" { auto b_left=("+left+"); if(b_left!=nullptr)return b_left; return "+cast_to(e.type,right)+"; }())";
                    if(e.text=="&&"||e.text=="||")return "(("+left+") "+e.text+" ("+right+"))";
                    if(e.text==",")return "("+capture()+"() -> "+cpp_type(e.type)+" { (void)("+left+"); return ("+right+"); }())";
                    if(e.text=="+"&&(a.type=="string"||b.type=="string")) {
                        if(b.type!="string")right="bolt::native::text("+right+")";
                        return "("+capture()+"() -> std::string { auto b_left=bolt::native::text("+left+"); b_left.append(std::string_view("+right+")); return b_left; }())";
                    }
                    std::string result="auto b_left = ("+left+"); auto b_right = ("+right+"); return ";
                    if(e.text=="=="||e.text=="!="||e.text=="<"||e.text==">"||e.text=="<="||e.text==">=") {
                        auto common=common_numeric(a.type,b.type);
                        result+=common.empty()?"(b_left "+e.text+" b_right)":"("+cast_to(common,"b_left")+" "+e.text+" "+cast_to(common,"b_right")+")";
                    } else result+=arithmetic(e.text,a.type,b.type,"b_left","b_right");
                    return block_expression(cpp_type(e.type), result+";");
                }
                throw Error(e.token.at,"internal error: cannot emit expression");
            }
            std::string variable(const Stmt& s) {
                std::string init=s.expression?(is_reference(s.type)&&s.constant?"("+expression(*s.expression)+")":cast_to(s.type,expression(*s.expression))):default_value(s.type);
                return qualified_type(s.type,s.constant)+" "+local_name(s.local_id,s.name)+" = "+guarded(s.expression?s.expression->token.at:s.token.at,qualified_type(s.type,s.constant),init);
            }
            void expression_statement(const Expr& e) {
                using K=Expr::Kind;
                using B=Expr::Binding;
                const Expr* invocation=&e;
                const bool assign=e.kind==K::Assign && e.binding==B::None && e.text=="=" &&
                    e.args[0]->kind==K::Name && e.args[0]->binding==B::Local &&
                    is_numeric(e.type) && e.args[1]->type==e.type;
                if (assign) invocation=e.args[1].get();
                if (!program.cpp_features.empty() && invocation->kind==K::Call &&
                    invocation->binding==B::Method && !invocation->lvalue &&
                    (invocation->type=="void" || is_numeric(invocation->type))) {
                    // A statement can own the handler directly. Keep argument
                    // staging and result adaptation identical to a nested call.
                    const auto code=call(*invocation);
                    if (code.block_type==cpp_type(invocation->type)) {
                        const auto prefix=assign ? expression(*e.args[0])+" = " : "(void)";
                        line("try { "+prefix+"("+capture()+"() -> "+code.block_type+" { "+code.text+" }()); }"+
                             catch_at(invocation->token.at));
                        return;
                    }
                }
                line("(void)("+expression(e)+");");
            }
            void scoped(const Stmt& s) {
                line("{");
                ++indent;
                statement(s);
                --indent;
                line("}");
            }
            void statement(const Stmt& s) {
                using K=Stmt::Kind;
                if(s.kind==K::Declarations) {
                    for(const auto& c:s.statements)statement(*c);
                    return;
                }
                source(s.token);
                if(s.kind==K::Block) {
                    line("{");
                    ++indent;
                    for(const auto& c:s.statements)statement(*c);
                    --indent;
                    line("}");
                } else if(s.kind==K::Variable)line(variable(s)+";");
                else if(s.kind==K::Expression)expression_statement(*s.expression);
                else if(s.kind==K::If) {
                    line("if ("+expression(*s.condition)+")");
                    scoped(*s.body);
                    if(s.alternative) {
                        line("else");
                        scoped(*s.alternative);
                    }
                } else if(s.kind==K::While) {
                    line("while ("+expression(*s.condition)+")");
                    scoped(*s.body);
                } else if(s.kind==K::For) {
                    std::string init;
                    const bool group=s.init&&s.init->kind==K::Declarations;
                    if(group){line("{");++indent;statement(*s.init);}
                    else if(s.init)init=s.init->kind==K::Variable?variable(*s.init):expression(*s.init->expression);
                    source(s.token);
                    line("for ("+init+"; "+(s.condition?expression(*s.condition):"")+"; "+(s.increment?expression(*s.increment):"")+")");
                    scoped(*s.body);
                    if(group){--indent;line("}");}
                } else if(s.kind==K::SwitchScope) {
                    line("switch (0) { default:");scoped(*s.body);line("}");
                } else if(s.kind==K::ForEach) {
                    line("{");++indent;
                    const auto range="b_range"+std::to_string(++temporary);
                    source(s.expression->token);
                    line("auto&& "+range+" = ("+expression(*s.expression)+");");
                    source(s.token);
                    line("for ("+qualified_type(s.type,s.constant)+" "+local_name(s.local_id,s.name)+" : bolt::library::range_view("+range+"))");
                    scoped(*s.body);--indent;line("}");
                } else if(s.kind==K::Try) {
                    const auto guard="b_finally"+std::to_string(++temporary);
                    if(s.alternative){line("{");++indent;line("auto "+guard+" = bolt::library::finally([&]()");scoped(*s.alternative);line(");");line("try {");++indent;}
                    if(!s.statements.empty())line("try");
                    scoped(*s.body);
                    for(const auto& handler:s.statements){
                        source(handler->token);
                        line(handler->type.empty()?"catch (...)":"catch ("+cpp_type(referred(handler->type))+"& "+local_name(handler->local_id,handler->name)+")");scoped(*handler->body);
                    }
                    if(s.alternative){--indent;line("} catch (...) { auto b_pending = std::current_exception(); "+guard+".Run(); std::rethrow_exception(b_pending); }");line(guard+".Run();");--indent;line("}");}
                } else if(s.kind==K::Throw) {
                    if(s.expression && derives_from(program,referred(s.expression->type),"System.Exception"))
                        line("("+expression(*s.expression)+").BoltThrowCopy();");
                    else line(s.expression?"throw "+expression(*s.expression)+";":"throw;");
                }
                else if(s.kind==K::Return)line(s.expression?"return "+cast_to(method_return,expression(*s.expression))+";":"return;");
                else if(s.kind==K::Break)line("break;");
                else if(s.kind==K::Continue)line("continue;");
                else if(s.kind!=K::Empty)throw Error(s.token.at,"internal error: unhandled C++ statement");
                else line(";");
            }
            void collection_metadata() {
                bool opened=false;
                for(auto index:program.layout_order){const auto& c=program.classes[index];
                    if(c.is_native||!emitted_class(c)||(!c.generic_parameters.empty()||!c.pack_parameter.empty()))continue;
                    for(const auto& a:c.attributes)if(a.name=="SerializeValue") {
                        if(a.arguments.size()!=2)throw Error(a.token.at,"invalid serialization value contract", "BLT2410");
                        const auto* get=find_method(c,a.arguments[0]);const auto* make=find_method(c,a.arguments[1]);
                        if(!get||!make||!get->is_public||get->is_static||!get->readonly_receiver||!get->parameters.empty()||get->type=="void"||!make->is_public||!make->is_static||make->parameters.size()!=1||referred(make->parameters[0].type)!=get->type||make->type!=c.name)
                            throw Error(a.token.at,"SerializeValue requires a public ReadOnly reader and a public static inverse factory", "BLT2410");
                        if(!opened){--indent;line("} // namespace bolt_generated");line("namespace bolt::library::json_detail {");++indent;opened=true;}
                        line("using namespace bolt_generated;");
                        const auto owner="bolt_generated::"+class_type(c.name);
                        line("template<> struct SourceContainer<"+owner+"> {");++indent;
                        line("static constexpr int Kind=3; using Payload="+cpp_type(get->type)+";");
                        line("static Payload Export(const "+owner+"& value){return value."+method_name(*get)+"();}");
                        line("static "+owner+" Import(const Payload& value){return "+owner+"::"+method_name(*make)+"(value);}");
                        --indent;line("};");
                    }
                    for(const auto& a:c.attributes)if(a.name=="SerializeSequence"||a.name=="SerializeMap"){
                        if(a.arguments.size()!=3)throw Error(a.token.at,"invalid serialization contract", "BLT2410");
                        const auto* count=find_property(c,a.arguments[0]);
                        const auto* get=find_method(c,a.arguments[1]);const auto* add=find_method(c,a.arguments[2]);
                        if(!count||count->type!="int"||count->getter.empty()||!get||!add||get->parameters.size()!=1||get->parameters[0].type!="int"||!get->readonly_receiver)
                            throw Error(a.token.at,"serialization requires an int count property, ReadOnly element accessor(int), and insertion method", "BLT2410");
                        if(!opened){--indent;line("} // namespace bolt_generated");line("namespace bolt::library::json_detail {");++indent;opened=true;}
                        const auto owner="bolt_generated::"+class_type(c.name);auto cpp=[&](const std::string& t){return cpp_type(t);};
                        // All generated user type identifiers are qualified below using a namespace import.
                        line("using namespace bolt_generated;");
                        line("template<> struct SourceContainer<"+owner+"> {");++indent;
                        line("static constexpr int Kind="+std::string(a.name=="SerializeMap"?"2":"1")+";");
                        line("using Value="+owner+";");
                        line("static std::int32_t Count(const Value& value){return value."+method_name(*find_method(c,count->getter))+"();}");
                        line("static "+cpp(get->type)+" At(const Value& value,std::int32_t i){return value."+method_name(*get)+"(i);}");
                        if(a.name=="SerializeSequence"){
                            if(add->parameters.size()!=1||referred(add->parameters[0].type)!=referred(get->type))throw Error(a.token.at,"sequence insertion type must match its element accessor", "BLT2410");
                            line("using Element="+cpp(referred(get->type))+";");
                            line("static void Add(Value& value,const Element& item){value."+method_name(*add)+"(item);}");
                        }else{
                            const auto* pair=program.find_class(referred(get->type));const auto* key=pair?find_property(*pair,"Key"):nullptr;const auto* value=pair?find_property(*pair,"Value"):nullptr;
                            if(!key||!value||add->parameters.size()!=2)throw Error(a.token.at,"map element must expose Key/Value properties and insertion takes two parameters", "BLT2410");
                            line("using Pair="+cpp(referred(get->type))+"; using Key="+cpp(key->type)+"; using Element="+cpp(value->type)+";");
                            line("static Key GetKey(const Pair& p){return p."+method_name(*find_method(*pair,key->getter))+"();}");
                            line("static Element GetValue(const Pair& p){return p."+method_name(*find_method(*pair,value->getter))+"();}");
                            line("static void Add(Value& value,const Key& key,const Element& item){value."+method_name(*add)+"(key,item);}");
                        }
                        --indent;line("};");
                    }
                }
                if(opened){--indent;line("} // namespace bolt::library::json_detail");line("namespace bolt_generated {");++indent;}
            }
            void reflection_metadata() {
                bool any=false;for(const auto& c:program.classes)any|=c.is_reflected&&!c.is_native&&(c.generic_parameters.empty()&&c.pack_parameter.empty());if(!any)return;
                --indent;line("} // namespace bolt_generated");line("namespace bolt::library {");++indent;line("using namespace bolt_generated;");
                for(auto index:program.layout_order) {
                    const auto& c=program.classes[index];if(!c.is_reflected||c.is_native||(!c.generic_parameters.empty()||!c.pack_parameter.empty()))continue;
                    const auto owner="bolt_generated::"+class_type(c.name);
                    line("template<> struct Metadata<"+owner+"> {");++indent;
                    line("static constexpr bool Reflected=true;");line("static constexpr bool Struct="+std::string(c.is_struct?"true":"false")+";");
                    line("static constexpr const char* Name="+quote(c.name)+";");line("static constexpr const char* Namespace="+quote(c.namespace_name)+";");line("static constexpr const char* Attributes="+quote(attribute_text(c.attributes))+";");
                    std::string fields="std::tuple{";
                    for(const auto& f:c.fields) {
                        bool explicit_field=false,ignore=false;std::string jsonName=f.name;
                        for(const auto& a:f.attributes){explicit_field|=a.name=="Reflect";ignore|=a.name=="JsonIgnore";if(a.name=="JsonName"&&!a.arguments.empty())jsonName=a.arguments[0];}
                        if(f.name.starts_with("@")||f.is_event||f.is_static||(!f.is_public&&!explicit_field))continue;
                        if(fields.back()!='{')fields+=", ";
                        fields+="bolt::library::field("+quote(f.name)+", "+quote(jsonName)+", "+quote(f.type)+", &"+owner+"::"+field_name(f.name)+", "+(f.constant?"true":"false")+", "+(ignore?"true":"false")+", "+quote(attribute_text(f.attributes))+")";
                    }
                    for(const auto& p:c.properties) {
                        if(p.is_static||p.is_indexer||!p.is_public||p.getter.empty())continue;
                        const auto* get=find_method(c,p.getter); const auto* set=p.setter.empty()?nullptr:find_method(c,p.setter);
                        if(!get||!get->is_public)continue;
                        std::string jsonName=p.name;bool ignore=false;
                        for(const auto& a:p.attributes){ignore|=a.name=="JsonIgnore";if(a.name=="JsonName"&&!a.arguments.empty())jsonName=a.arguments[0];}
                        const auto value=cpp_type(referred(p.type));
                        // ReadOnly accessors can read the original directly. Mutable value-type getters read a snapshot.
                        std::string getter="[](const "+owner+"& object) -> "+value+" { ";
                        if(get->readonly_receiver)getter+="return object."+method_name(*get)+"(); }";
                        else getter+="auto snapshot=object; return snapshot."+method_name(*get)+"(); }";
                        std::string setter="[]("+owner+"& object,const "+value+"& value) { ";
                        bool readonly=!set||!set->is_public;
                        if(readonly)setter+="(void)object;(void)value; }";
                        else setter+="object."+method_name(*set)+"(value); }";
                        if(fields.back()!='{')fields+=", ";
                        fields+="bolt::library::property<"+owner+", "+value+">("+quote(p.name)+", "+quote(jsonName)+", "+quote(p.type)+", "+getter+", "+setter+", "+(readonly?"true":"false")+", "+(ignore?"true":"false")+", "+quote(attribute_text(p.attributes))+")";
                    }
                    fields+="}";
                    const auto* base=c.base.empty()?nullptr:program.find_class(c.base);
                    if(base&&base->is_reflected)fields="std::tuple_cat(Metadata<bolt_generated::"+class_type(c.base)+">::Fields(), "+fields+")";
                    line("static auto Fields(){return "+fields+";}");
                    std::string methods="std::tuple{";
                    for(const auto& m:c.methods) {
                        if(!m.property_name.empty()||!m.is_public||m.is_constructor||m.is_destructor||!concrete(m)||m.is_extern||m.is_constant_template)continue;
                        if(methods.back()!='{')methods+=", ";std::string types;
                        for(const auto& p:m.parameters){if(!types.empty())types+="\n";types+=(p.constant?"const ":"")+p.type;}
                        methods+="bolt::library::method("+quote(m.name)+", "+quote(m.type)+", "+quote(types)+", &"+owner+"::"+method_name(m)+", "+(m.is_static?"true":"false")+", "+quote(attribute_text(m.attributes))+")";
                    }
                    methods+="}";line("static auto Methods(){return "+methods+";}");
                    --indent;line("};");
                }
                --indent;line("} // namespace bolt::library");line("namespace bolt_generated {");++indent;
            }
            std::string parameters(const Method& m,bool defaults=false) {
                std::string result;
                for(const auto& p:m.parameters) {
                    if(!result.empty())result+=", ";
                    result+=qualified_type(p.type,p.constant)+" "+local_name(p.local_id,p.name);
                    if(defaults&&p.default_value){
                        const auto previous=field_initializer;
                        field_initializer=true; // C++ default arguments are non-local contexts.
                        result+=" = "+cast_to(p.type,expression(*p.default_value));
                        field_initializer=previous;
                    } else if(defaults&&p.variadic) {
                        result+=" = {}"; // An implicit constructor call supplies an empty variadic array.
                    }
                }
                return result;
            }
            bool concrete(const Method& m)const {
                return m.generic_parameters.empty()&&m.pack_parameter.empty()&&(m.analyzed||m.is_abstract||m.is_extern);
            }
            void equality(const Class& c) {
                if(c.is_static||c.is_interface)return;
                line("public:");
                auto* eq=equality_contract(c);auto* hash=hash_contract(c);
                std::string equal="true";
                if(eq)equal=method_name(*eq)+"(*this, other)";
                else {
                    if(!c.base.empty())equal+=" && (static_cast<const "+class_type(c.base)+"&>(*this) == static_cast<const "+class_type(c.base)+"&>(other))";
                    for(const auto& f:c.fields)if(!f.is_static)equal+=" && bolt::native::value_equal("+field_name(f.name)+", other."+field_name(f.name)+")";
                }
                line("bool operator==(const "+class_type(c.name)+"& other) const { (void)other; return "+equal+"; }");
                line("std::size_t BoltHash() const {");++indent;
                if(hash)line("return static_cast<std::size_t>("+method_name(*hash)+"());");
                else if(eq)line("return 0; // A custom equality without a matching hash is always safe, never a mismatched hash.");
                else {
                    line("std::size_t value = 0;");
                    if(!c.base.empty())line("value = static_cast<const "+class_type(c.base)+"&>(*this).BoltHash();");
                    for(const auto& f:c.fields)if(!f.is_static)line("value = bolt::native::hash_combine(value, bolt::native::Hasher<"+cpp_type(f.type)+">{}("+field_name(f.name)+"));");
                    line("return value;");
                }
                --indent;line("}");
            }
            std::vector<const Method*> interface_methods(const Class& c)const {
                std::map<std::string,const Method*> unique;
                std::function<void(const Class&)> collect=[&](const Class& i){for(const auto& p:i.interfaces)collect(*program.find_class(p));for(const auto& m:i.methods)unique[m.id]=&m;};collect(c);
                std::vector<const Method*> result;for(const auto& [id,m]:unique){(void)id;result.push_back(m);}return result;
            }
            void interface_wrapper(const Class& c) {
                const auto raw=class_type(c.name),box=interface_value_type(c.name);
                line("class "+box+" : public "+raw+" {");++indent;
                if(capture_cell_type(c.name)) {
                    line("std::shared_ptr<bolt::native::ObjectRoot> value;");
                    line("public:");
                    line(box+"() = default;");
                    line("template<class T> "+box+"(const T& other):value(bolt::native::interface_clone<"+raw+">(other)) {}");
                }else {
                line("bolt::native::ObjectPointer value;");
                line("public:");
                line(box+"() = default;");
                line(box+"(const "+box+"& other):value(other.value ? other.value->BoltClone(bolt::memory::owner_of(this)) : nullptr) {}");
                line(box+"("+box+"&& other):"+box+"(static_cast<const "+box+"&>(other)) {}");
                line(box+"& operator=(const "+box+"& other) { if(this!=&other)value=other.value ? other.value->BoltClone(bolt::memory::owner_of(this)) : nullptr; return *this; }");
                line(box+"& operator=("+box+"&& other) { return *this=static_cast<const "+box+"&>(other); }");
                line("template<class T> "+box+"(const T& other):value(bolt::native::interface_clone<"+raw+">(other,bolt::memory::owner_of(this))) {}");
                }
                line(raw+"& Get() const { auto* p=dynamic_cast<"+raw+"*>(value.get()); if(!p)throw std::runtime_error(\"uninitialized interface value\"); return *p; }");
                line("bolt::native::ObjectPointer BoltClone(bolt::memory::Arena* destination=nullptr) const override { return Get().BoltClone(destination); }");
                line("bool BoltObjectEquals(const bolt::native::ObjectRoot& other) const override { auto copy=other.BoltClone(); return Get().BoltObjectEquals(*copy); }");
                line("std::size_t BoltObjectHash() const override { return Get().BoltObjectHash(); }");
                line("std::size_t BoltHash() const { return BoltObjectHash(); }");
                line("bool operator==(const "+box+"& other) const { return Get().BoltObjectEquals(other.Get()); }");
                for(const auto* m:interface_methods(c))line(cpp_type(m->type)+" "+method_name(*m)+"("+parameters(*m)+")"+accessor_const(*m)+" override;");
                --indent;line("};");
            }
            void interface_bodies(const Class& c) {
                for(const auto* m:interface_methods(c)) {
                    std::string args;
                    for(const auto& p:m->parameters){if(!args.empty())args+=", ";const auto name=local_name(p.local_id,p.name);args+=is_reference(p.type)?name:"std::move("+name+")";}
                    line("inline "+cpp_type(m->type)+" "+interface_value_type(c.name)+"::"+method_name(*m)+"("+parameters(*m)+")"+accessor_const(*m)+" { return Get()."+method_name(*m)+"("+args+"); }");
                }
            }
            void primitive_text_overloads(bool definitions) {
                for(const std::string type:{"bool","char","byte","short","ushort","int","uint","long","ulong","float","double"}) {
                    const auto formatter=primitive_formatter(type);
                    const auto* cls=program.find_class(formatter.owner);
                    const auto* method=cls?find_method(*cls,formatter.method):nullptr;
                    if(!method || !concrete(*method))continue;
                    const auto signature="inline bolt::native::String primitive_text("+cpp_type(type)+" value)";
                    if(definitions)line(signature+" { return "+class_type(cls->name)+"::"+method_name(*method)+"(value); }");
                    else line(signature+";");
                }
            }
            void primitive_adapter(const Class& c) {
                const auto raw="bolt_generated::"+class_type(c.name);
                --indent;line("} // namespace bolt_generated");line("namespace bolt::native {");++indent;
                line("template<class T> struct PrimitiveInterfaceAdapter<"+raw+",T> final : "+raw+" {");++indent;
                line("T value;");line("explicit PrimitiveInterfaceAdapter(const T& v):value(v) {}");
                line("bolt::native::String "+method_name(*find_method(c,"ToString"))+"() override { return bolt_generated::primitive_text(value); }");
                line("ObjectPointer BoltClone(memory::Arena* destination=nullptr) const override { return ObjectPointer(owned_new<PrimitiveInterfaceAdapter>(destination,*this)); }");
                line("bool BoltObjectEquals(const ObjectRoot& other) const override { auto p=dynamic_cast<const PrimitiveInterfaceAdapter*>(&other); return p && value_equal(value,p->value); }");
                line("std::size_t BoltObjectHash() const override { return Hasher<T>{}(value); }");
                --indent;line("};");--indent;line("} // namespace bolt::native");line("namespace bolt_generated {");++indent;
            }
            void struct_interface_adapters() {
                bool any=false;for(const auto& c:program.classes)any|=c.is_struct&&!c.interfaces.empty()&&!c.is_native;
                if(!any)return;
                --indent;line("} // namespace bolt_generated");line("namespace bolt::native {");++indent;line("using namespace bolt_generated;");
                for(const auto& c:program.classes)if(c.is_struct&&!c.interfaces.empty()&&!c.is_native&&(c.generic_parameters.empty()&&c.pack_parameter.empty())) {
                    std::vector<std::string> interfaces;
                    std::function<void(const std::string&)> collect=[&](const std::string& name){
                        if(std::find(interfaces.begin(),interfaces.end(),name)!=interfaces.end())return;
                        interfaces.push_back(name);const auto* iface=program.find_class(name);if(!iface)return;for(const auto& parent:iface->interfaces)collect(parent);
                    };
                    for(const auto& name:c.interfaces)collect(name);
                    for(const auto& name:interfaces) {
                        const auto* iface=program.find_class(name);if(!iface)continue;
                        const auto raw="bolt_generated::"+class_type(name),value_type="bolt_generated::"+class_type(c.name);
                        line("template<> struct PrimitiveInterfaceAdapter<"+raw+", "+value_type+"> final : "+raw+" {");++indent;
                        line(value_type+" value;");
                        line("explicit PrimitiveInterfaceAdapter(const "+value_type+"& v):value(v) {}");
                        line("ObjectPointer BoltClone(memory::Arena* destination=nullptr) const override { return ObjectPointer(owned_new<PrimitiveInterfaceAdapter>(destination,*this)); }");
                        line("bool BoltObjectEquals(const ObjectRoot& other) const override { auto p=dynamic_cast<const PrimitiveInterfaceAdapter*>(&other); return p && value_equal(value,p->value); }");
                        line("std::size_t BoltObjectHash() const override { return Hasher<"+value_type+">{}(value); }");
                        for(const auto* contract:interface_methods(*iface)) {
                            const auto* implementation=method_by_id(c,contract->id);
                            if(!implementation||implementation->is_abstract)throw std::runtime_error("missing struct interface implementation: "+c.name+" -> "+name+"."+contract->name);
                            std::string args;
                            for(const auto& parameter:contract->parameters){if(!args.empty())args+=", ";const auto local=local_name(parameter.local_id,parameter.name);args+=is_reference(parameter.type)?local:"std::move("+local+")";}
                            line(cpp_type(contract->type)+" "+method_name(*contract)+"("+parameters(*contract)+")"+accessor_const(*contract)+" override { return value."+method_name(*implementation)+"("+args+"); }");
                        }
                        --indent;line("};");
                    }
                }
                --indent;line("} // namespace bolt::native");line("namespace bolt_generated {");++indent;
            }
            bool dependency_class(const Class& c) const {
                return std::find(program.external_sources.begin(),program.external_sources.end(),c.token.at.file)!=program.external_sources.end();
            }
            bool inline_class(const Class& c) const {
                return c.token.at.file.starts_with("<bolt-stdlib>") || c.name.find('<')!=std::string::npos;
            }
            bool emit_body(const Class& c,const Method* method=nullptr) const {
                if(method&&c.is_global){
                    if(method->is_specialization||method->token.at.file.starts_with("<bolt-stdlib>"))return true;
                    return std::find(program.external_sources.begin(),program.external_sources.end(),method->token.at.file)==program.external_sources.end();
                }
                return !dependency_class(c) || inline_class(c) || (method && method->is_specialization);
            }
            std::string body_prefix(const Class& c,const Method* method=nullptr) const {
                return inline_class(c)||(method&&(method->is_specialization||method->token.at.file.starts_with("<bolt-stdlib>")))?"inline ":"";
            }
            bool plain_native_struct_type(const std::string& type,std::vector<std::string>& active) const {
                const auto t=referred(type);
                if(is_pointer(t)||t=="bool"||t=="char"||is_numeric(t)||program.find_enum(t))return true;
                const auto* c=program.find_class(t);
                if(!c||!c->is_struct||c->is_native)return false;
                if(std::find(active.begin(),active.end(),t)!=active.end())return false;
                active.push_back(t);
                for(const auto& field:c->fields)if(!field.is_static&&!plain_native_struct_type(field.type,active)){active.pop_back();return false;}
                active.pop_back();return true;
            }
            bool plain_native_struct_type(const std::string& type) const {std::vector<std::string> active;return plain_native_struct_type(type,active);}
            public: explicit Emitter(const Program& p):program(p) {
            }
            std::string run(bool mobile_entry) {
                auto* entry=program.find_class(program.entry_class.empty()?"Program":program.entry_class);
                const Method* main=nullptr;
                if(entry)for(const auto& m:entry->methods)if(m.name=="Main"&&m.generic_parameters.empty()&&m.pack_parameter.empty()&&(m.parameters.empty()||(m.parameters.size()==1&&m.parameters[0].type=="string[]")))main=&m;
                if(!main&&!program.library_mode)throw std::runtime_error("native emission requires a resolved Main");
                line("// Generated by Bolt " BOLT_VERSION_STRING ". This is the portable C++20 backend, not LLVM IR.");
                const auto* kernel=program.find_class("Bolt.Runtime.ThreadKernel");
                const bool threads=kernel && emitted_class(*kernel);
                const auto* atomics=program.find_class("Bolt.Runtime.AtomicOps");
                if(threads || (atomics && emitted_class(*atomics)))line("#include <bolt/detail/HostedIntrinsics.hpp>");
                if(mobile_entry)line("#include <bolt/MobileAot.h>");
                line("#include <cstddef>");line("#include <type_traits>");
                line("#include <bolt/Std.hpp>");line("#include <bolt/Platform.hpp>");
                line("#include <bolt/API.hpp>");
                line("#include <bolt/native/Jit.hpp>");
                line("#include <bolt/Path.hpp>");
                if(!program.cpp_features.empty())line("#include <bolt/library/Library.hpp>");
                // Header names are preprocessor tokens: UTF-8 bytes and native
                // path separators must reach the driver without string-literal escapes.
                for(const auto& header:program.cpp_headers) {
                    if(header.empty()||header.find_first_of("\r\n\"")!=std::string::npos||header.find('\0')!=std::string::npos)
                        throw std::runtime_error("invalid native header filename");
                    line("#include \""+header+"\"");
                }
                for(const auto& c:program.classes)for(const auto& m:c.methods)if(m.native_abi=="C"&&concrete(m)) {
                    if(m.token.at.file.starts_with("<bolt-stdlib>")&&m.token.at.file.ends_with("/System/WindowsApi.bolt"))continue;
                    bool header=false;for(const auto& a:m.attributes)header|=a.name=="Header";if(header)continue;
                    auto abi_type=[&](const std::string& type,bool constant){const auto t=referred(type);if(t=="string")return std::string("const char*");if(is_array(t))return std::string(constant?"const ":"")+cpp_type(element_type(t))+"*";if(is_reference(type))return std::string(constant?"const ":"")+cpp_type(t)+"*";return cpp_type(t);};
                    std::string params;for(const auto& p:m.parameters){if(!params.empty())params+=", ";params+=abi_type(p.type,p.constant);}line("extern \"C\" "+abi_type(m.type,false)+" "+method_name(m)+"("+params+");");
                }
                line("#if defined(_WIN32) && defined(BOLT_SHARED_LIBRARY)");
                line("#define BOLT_PROJECT_EXPORT __declspec(dllexport)");
                line("#else");line("#define BOLT_PROJECT_EXPORT");line("#endif");
                line("namespace bolt_generated {");
                ++indent;
                line("template<class T> inline bolt::native::String primitive_text(const T& value) { return bolt::native::text(value); }");
                primitive_text_overloads(false);
                for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native){
                    line("class "+class_type(c.name)+";");
                    if(c.is_interface)line("class "+interface_value_type(c.name)+";");
                }
                for(const auto& e:program.enums) {
                    if(e.is_template_member)continue;
                    if(!e.native_cpp.empty())continue;
                    source(e.token);
                    line("enum class "+cpp_type(e.name)+" : "+cpp_type(e.underlying)+" {");
                    ++indent;
                    for(const auto& m:e.members) {
                        std::string number=m.value;
                        if(is_unsigned(e.underlying))number+="ULL";
                        else if(number=="-9223372036854775808")number="(-9223372036854775807LL - 1LL)";
                        else number+="LL";
                        source(m.token);
                        line("b_"+mangle(m.name)+" = "+number+",");
                    }
                    --indent;
                    line("};");
                }
                for(auto index:program.layout_order) {
                    const auto& c=program.classes[index];
                    if(c.is_native)continue;
                    source(c.token);
                    std::string parents;
                    if(!c.is_static&&!c.is_struct)parents=" : virtual public bolt::native::ObjectRoot";
                    if(!c.base.empty())parents+=(parents.empty()?" : public ":", public ")+class_type(c.base);
                    if(!c.is_struct)for(const auto& i:c.interfaces)parents+=", virtual public "+class_type(i);
                    line("class "+std::string(program.library_mode&&!dependency_class(c)&&!inline_class(c)?"BOLT_PROJECT_EXPORT ":"")+class_type(c.name)+(c.is_sealed?" final":"")+parents+" {");
                    ++indent;
                    line("public:");
                    line("static void bolt_internal_initialize();");
                    if(c.token.at.file=="<bolt-stdlib>/System/Reflection.bolt" && !c.is_static)
                        line("friend class "+class_type("System.Reflection.Reflection")+";");
                    if(!c.is_abstract&&!c.is_native&&derives_from(program,c.name,"System.Exception")){
                        line("std::uint8_t* BoltDescriptor() const override { return bolt::native::erased_descriptor<"+class_type(c.name)+">(); }");
                        line("void BoltThrowCopy() const override { throw *this; }");
                    }
                    if(c.is_reflected) {
                        line("friend struct bolt::library::Metadata<"+class_type(c.name)+">;");
                        for(const auto* name:{"Bolt.Runtime.ValueTree","System.Reflection.Reflection"})
                            line("friend class "+class_type(name)+";");
                    }
                    for(const auto& adapter:program.classes)if(adapter.delegate_owner==c.name)line("friend class "+class_type(adapter.name)+";");
                    bool has_ctor=false,has_dtor=false;
                    for(const auto& m:c.methods){has_ctor|=m.is_constructor;has_dtor|=m.is_destructor;}
                    if(c.name=="Bolt.Runtime.ThreadHandle") {
                        const auto t=class_type(c.name);
                        line(t+"() = default;");
                        line(t+"(const "+t+"&);");
                        line(t+"& operator=(const "+t+"&);");
                        line("~"+t+"();");
                    }else if(c.is_static)line(class_type(c.name)+"() = delete;");
                    else {
                        if(!has_ctor)line(class_type(c.name)+"() = default;");
                        bool polymorphic=!c.is_struct&&!c.base.empty();
                        for(const auto& m:c.methods)polymorphic|=!c.is_struct&&m.is_virtual;
                        for(const auto& d:program.classes)polymorphic|=!c.is_struct&&d.base==c.name;
                        if(!has_dtor)line(std::string(polymorphic?"virtual ~":"~")+class_type(c.name)+"() = default;");
                    }
                    for(const auto& f:c.fields) {
                        line(c.is_struct||f.is_public?"public:":"private:");
                        field_initializer=true;
                        auto init=f.initializer?cast_to(f.type,expression(*f.initializer)):default_value(f.type);
                        source(f.token);
                        if(f.is_static)line("static "+cpp_type(f.type)+"& "+field_name(f.name)+"();");
                        else if(has_ctor&&!f.initializer)line(cpp_type(f.type)+" "+field_name(f.name)+";");
                        else line(cpp_type(f.type)+" "+field_name(f.name)+" = "+init+";");
                        field_initializer=false;
                    }
                    for(const auto& m:c.methods)if(concrete(m)&&!m.is_extern) {
                        line(m.is_public?"public:":"private:");
                        source(m.token);
                        if(m.is_destructor)line("~"+class_type(c.name)+"() noexcept;");
                        else if(m.is_constructor)line(class_type(c.name)+"("+parameters(m,true)+");");
                        else line(std::string(m.is_static?"static ":m.is_virtual&&!c.is_struct?"virtual ":"")+cpp_type(m.type)+" "+method_name(m)+"("+parameters(m,true)+")"+accessor_const(m)+(m.is_override&&!c.is_struct?" override":"")+(m.is_sealed&&m.is_virtual&&!c.is_struct?" final":"")+(m.is_abstract?" = 0":"")+";");
                    }
                    equality(c);
                    if(!c.is_static&&!c.is_interface&&!c.is_abstract&&!c.is_struct) {
                        line("public:");
                        line("bolt::native::ObjectPointer BoltClone(bolt::memory::Arena* destination=nullptr) const override { return bolt::native::ObjectPointer(bolt::native::owned_new<"+class_type(c.name)+">(destination,*this)); }");
                        line("bool BoltObjectEquals(const bolt::native::ObjectRoot& other) const override { auto p=dynamic_cast<const "+class_type(c.name)+"*>(&other); return p && typeid(*this)==typeid(other) && (*this == *p); }");
                        line("std::size_t BoltObjectHash() const override { return BoltHash(); }");
                    }
                    if(c.is_interface) {
                        line("bool operator==(const "+class_type(c.name)+"& other) const { return BoltObjectEquals(other); }");
                        line("std::size_t BoltHash() const { return BoltObjectHash(); }");
                    }
                    if(c.name=="System.IStringable") {
                        const auto* m=find_method(c,"ToString");
                        line("std::string BoltFormat() const override { auto copy=BoltClone(); auto* value=dynamic_cast<"+class_type(c.name)+"*>(copy.get()); if(!value)throw std::runtime_error(\"invalid IStringable clone\"); return value->"+method_name(*m)+"(); }");
                    }
                    --indent;
                    line("};");
                    if(c.is_struct&&plain_native_struct_type(c.name)) {
                        const auto layout=native_abi::ValueLayouts(program).get(c.name);
                        line("static_assert(std::is_standard_layout_v<"+class_type(c.name)+">, \"Bolt native struct must remain standard-layout in the C++ backend\");");
                        line("static_assert(sizeof("+class_type(c.name)+") == "+std::to_string(layout.size)+", \"Bolt struct size diverged from its native ABI layout\");");
                        line("static_assert(alignof("+class_type(c.name)+") == "+std::to_string(layout.alignment)+", \"Bolt struct alignment diverged from its native ABI layout\");");
                        for(const auto& field:layout.fields)line("static_assert(offsetof("+class_type(c.name)+", "+field_name(field.name)+") == "+std::to_string(field.offset)+", \"Bolt struct field offset diverged from its native ABI layout\");");
                    }
                    if(c.is_interface)interface_wrapper(c);
                    if(c.name=="System.IStringable")primitive_adapter(c);
                }
                primitive_text_overloads(true);
                if(threads)thread_adapters();
                struct_interface_adapters();
                for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native&&c.is_interface)interface_bodies(c);
                reflection_metadata();
                collection_metadata();
                for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native&&emit_body(c)) {
                    for(const auto& f:c.fields)if(f.is_static) {
                        field_initializer=true;
                        auto init=f.initializer?cast_to(f.type,expression(*f.initializer)):default_value(f.type);
                        if(f.initializer)source(f.initializer->token);
                        line(body_prefix(c)+cpp_type(f.type)+"& "+class_type(c.name)+"::"+field_name(f.name)+"() { static "+cpp_type(f.type)+" value = "+init+"; return value; }");
                        field_initializer=false;
                    }
                    line((c.is_global?std::string("inline "):body_prefix(c))+"void "+class_type(c.name)+"::bolt_internal_initialize() {");
                    ++indent;
                    for(const auto& f:c.fields)if(f.is_static)line("(void)"+field_name(f.name)+"();");
                    --indent;
                    line("}");
                }
                for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native)for(const auto& m:c.methods)if(concrete(m)&&!m.is_abstract&&!m.is_extern&&emit_body(c,&m)) {
                    source(m.token);
                    method_return=m.type;
                    if(m.is_destructor)line(body_prefix(c,&m)+class_type(c.name)+"::~"+class_type(c.name)+"() noexcept {");
                    else if(m.is_constructor) {
                        std::string signature=body_prefix(c,&m)+class_type(c.name)+"::"+class_type(c.name)+"("+parameters(m)+")";
                        if(!c.base.empty()) {
                            const auto* base=program.find_class(c.base);
                            const auto* target=m.base_constructor.empty()?nullptr:method_by_id(*base,m.base_constructor);
                            std::string args;
                            for(std::size_t i=0;i<m.base_arguments.size();++i) {
                                if(!args.empty())args+=", ";
                                auto v=expression(*m.base_arguments[i]);
                                args+=target?cast_to(target->parameters[i].type,v):v;
                            }
                            // Braced initializer guarantees left-to-right argument evaluation.
                            signature+=" : "+class_type(c.base)+"{"+args+"}";
                        }
                        bool has_initializer=!c.base.empty();
                        for(const auto& field:c.fields)if(!field.is_static) {
                            auto it=std::find_if(m.field_initializers.begin(),m.field_initializers.end(),[&](const auto& item){return item.first==field.name;});
                            if(it!=m.field_initializers.end()||!field.initializer) {
                                signature+=has_initializer?", ":" : ";has_initializer=true;
                                signature+=field_name(field.name)+"("+(it==m.field_initializers.end()?default_value(field.type):cast_to(field.type,expression(*it->second)))+")";
                            }
                        }
                        line(signature+" {");
                    }else line(body_prefix(c,&m)+cpp_type(m.type)+" "+class_type(c.name)+"::"+method_name(m)+"("+parameters(m)+")"+accessor_const(m)+" {");
                    ++indent;
                    if(m.is_async) {
                        const auto payload=generic_args(m.type).front();method_return=payload;
                        std::string captures=m.is_static?"":"b_async_self=*this";
                        for(const auto& parameter:m.parameters){if(!captures.empty())captures+=", ";const auto n=local_name(parameter.local_id,parameter.name);captures+=n+"=std::move("+n+")";}
                        line("return "+cpp_type(m.type)+"(bolt_schedule<"+class_type("Bolt.Runtime.TaskState<"+payload+">")+">(["+captures+"]() mutable -> "+cpp_type(payload)+" {");++indent;async_instance=!m.is_static;
                    }
                    line("try"); scoped(*m.body);
                    if(!program.cpp_features.empty()) {
                        line("catch (bolt::library::Exception& error) {");++indent;
                        line("error.AddFrame("+at(m.token.at)+".location(), "+quote(c.name+"."+m.name)+");");
                        if(m.is_destructor)line("std::cerr << error.ToString() << std::endl; std::terminate();");else line("throw;");
                        --indent;line("}");
                    }
                    line("catch (bolt::Error& error) {"); ++indent;
                    line("error.diagnostic.frames.push_back({"+at(m.token.at)+".location(), "+quote(c.name+"."+m.name)+"});");
                    if(m.is_destructor)line("std::cerr << bolt::format_diagnostic(error.diagnostic); std::terminate();");else line("throw;"); --indent; line("}");
                    if(method_return!="void")line("throw std::logic_error(\"unreachable: method completed without a return value\");");
                    if(m.is_async){async_instance=false;--indent;line("}));");}
                    --indent; line("}");
                }
                --indent;
                line("} // namespace bolt_generated");
                if(mobile_entry && program.library_mode)throw std::runtime_error("mobile AOT requires an executable project with Main");
                if(program.library_mode)return out.str();
                line("#line 1 \"<bolt native entry>\"");
                if(mobile_entry)line("extern \"C\" int BOLT_MOBILE_ENTRY(int argc, const char* const* argv) {");
                else if(program.entry_accepts_arguments){line("#ifdef _WIN32");line("int wmain(int argc, wchar_t** argv) {");line("#else");line("int main(int argc, char** argv) {");line("#endif");}else line("int main() {");
                ++indent;
                line("try {");
                ++indent;
                if(threads) {
                    line("alignas(8) std::uint8_t b_registry[8+sizeof(void*)]{};");
                    line("auto* b_previous=bolt::platform::ExecutionMemory::Activate(b_registry);");
                    line("auto b_workers=bolt::library::Finally([&]{ bolt_generated::"+kernel_method("Sweep")+"(b_registry,true); bolt::platform::ExecutionMemory::Activate(b_previous); });");
                }
                if(mobile_entry){
                    line("if(argc < 0 || (argc && !argv))return 1;");
                    line("for(int i=0;i<argc;++i)if(!argv[i])return 1;");
                }
                for(const auto& c:program.classes)if(emitted_class(c)&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_native &&
                    std::any_of(c.fields.begin(),c.fields.end(),[](const Field& f){return f.is_static;}))
                    line("bolt_generated::"+class_type(c.name)+"::bolt_internal_initialize();");
                if(program.entry_accepts_arguments){
                    if(mobile_entry){
                        line("auto b_arguments = bolt::native::Array<bolt::native::String>::Create(argc);");
                        line("for(int i=0;i<argc;++i)b_arguments.At(i)=bolt::library::NativeInterop::Argument(argv[i]);");
                    }else{
                        line("auto b_arguments = bolt::native::Array<bolt::native::String>::Create(argc > 0 ? argc-1 : 0);");
                        line("for(int i=1;i<argc;++i)b_arguments.At(i-1)=bolt::library::NativeInterop::Argument(argv[i]);");
                    }
                }
                auto entry_call="bolt_generated::"+class_type(entry->name)+"::"+method_name(*main)+"("+(program.entry_accepts_arguments?"std::move(b_arguments)":"")+")";
                if(main->is_async) {
                    const auto* task=program.find_class(main->type);
                    const auto* get=task?find_method(*task,"GetResult"):nullptr;
                    if(!get)throw std::runtime_error("async Task is missing GetResult");
                    entry_call+="."+method_name(*get)+"()";
                }
                const auto entry_result=main->is_async?generic_args(main->type).front():main->type;
                if(entry_result=="void") {
                    line(entry_call+";");
                    line("return 0;");
                }else line("return "+entry_call+";");
                --indent;
                if(!program.cpp_features.empty()) {
                    line("} catch (const bolt::library::Exception& error) {");++indent;
                    line("auto b_diagnostic = bolt::runtime_diagnostic({std::string(error.SourceFile()), error.SourceLine(), error.SourceColumn()}, std::string(error.TypeName()) + \": \" + error.what());");
                    line("if (b_diagnostic.code != \"BLT3000\") std::cerr << bolt::format_diagnostic(b_diagnostic) << error.StackTrace();");
                    line("else std::cerr << error.ToString() << std::endl;");
                    line("return 1;");--indent;
                }
                line("} catch (const bolt::Error& error) {");
                ++indent;
                line("std::cerr << bolt::format_diagnostic(error.diagnostic);");
                line("return 1;");
                --indent;
                line("} catch (const std::exception& error) {");
                ++indent;
                line("std::cerr << bolt::format_diagnostic(bolt::runtime_diagnostic({\"<native>\",1,1},error.what()));");
                line("return 1;");
                --indent;
                line("}");
                --indent;
                line("}");
                return out.str();
            }
        };
    }
    // namespace
    Program prepare_cpp_emission(const Program& original);
    static std::string emit_entry(const Program& program,bool mobile) {
        if(const auto* kernel=program.find_class("Bolt.Runtime.ThreadKernel");kernel && emitted_class(*kernel) &&
           kernel->token.at.file=="<bolt-stdlib>/System/ThreadKernel.bolt" && !find_method(*kernel,"Run"))
            return emit_entry(prepare_cpp_emission(program),mobile);
        if(!program.preprocessor.defines.contains("BoltCppCompatibility") &&
           std::any_of(program.classes.begin(), program.classes.end(), [](const Class& c) {
               return emitted_class(c) && !c.is_native && c.token.at.file=="<bolt-stdlib>/System/Ecs.bolt";
           }))
            return emit_entry(prepare_cpp_emission(program),mobile);
        struct ContextGuard { const Program* previous; explicit ContextGuard(const Program& p):previous(type_context){type_context=&p;} ~ContextGuard(){type_context=previous;} } context(program);
        return Emitter(program).run(mobile);
    }
    std::string emit_cpp(const Program& program){return emit_entry(program,false);}
    std::string emit_mobile_cpp(const Program& program){return emit_entry(program,true);}
}
// namespace bolt
