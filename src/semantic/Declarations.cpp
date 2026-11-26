#include "Analyzer.hpp"
#include <bolt/Value.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <functional>

namespace bolt::semantic {

std::string Analyzer::signature(const Method& m) {
    std::string s=m.name+"(";
    for(const auto& p:m.parameters)s+=(p.constant?"const ":"")+p.type+(p.pack?"...":"")+",";
    return s+")";
}

void Analyzer::native_headers(const std::vector<Attribute>& attrs) {
    for(const auto& attr:attrs)if(attr.name=="Header") {
        if(attr.arguments.size()!=1||attr.arguments[0].empty()||attr.arguments[0].find_first_of("\r\n\"")!=std::string::npos)throw Error(attr.token.at,"Header requires one valid header filename","BLT2500");
        if(std::find(program.cpp_headers.begin(),program.cpp_headers.end(),attr.arguments[0])==program.cpp_headers.end())program.cpp_headers.push_back(attr.arguments[0]);
        program.require_cpp("native C/C++ interoperability");
    }
}

void Analyzer::mark_type(std::string t) {
    t=referred(t);if(is_array(t)||is_pointer(t)||is_optional(t)){mark_type(element_type(t));return;}
    if(is_collection(t)){for(const auto& a:generic_args(t))mark_type(a);return;}
    auto* c=find_class(t);if(!c||c->reachable||(!c->generic_parameters.empty()||!c->pack_parameter.empty()))return;
    c->reachable=true;prepare(*c);
    if(!c->base.empty())mark_type(c->base);for(const auto& base:c->interfaces)mark_type(base);
    for(const auto& f:c->fields)mark_type(f.type);
}

void Analyzer::prepare_method(Class& c,Method& m) {
    native_headers(m.attributes);
    if(m.prepared)return;
    if(!m.generic_parameters.empty()||!m.pack_parameter.empty())return;
    SignatureArguments bindings(*this,m.specialization_types);
    m.type=resolve(m.type,&c,m.token,true);
    if(m.is_conversion) {
        if(m.type=="void"||m.type=="Arena"||is_reference(m.type)||m.type==c.name)
            throw Error(m.token.at,"conversion target must be a different, non-void value type (not a reference or Arena)", "BLT2600");
        m.name="@convert."+m.type;
    }
    if(!m.native_abi.empty()) {
        auto supported=[&](std::string t,bool result){t=referred(t);if(result&&t=="void")return true;if(t=="string")return true;if(is_numeric(t)||t=="bool"||is_pointer(t))return true;if(!result&&is_array(t)&&array_rank(t)==1&&is_numeric(element_type(t)))return true;const auto* native=find_class(t);return native&&native->is_native;};
        if(m.native_abi=="C"&&!supported(m.type,true))throw Error(m.token.at,"C interop returns require a numeric/bool type, pointer, UTF-8 string, or explicitly bound native type","BLT2500");
        if(is_reference(m.type))throw Error(m.token.at,"extern functions cannot return Bolt references; use a pointer","BLT2500");
        program.require_cpp("native C/C++ interoperability");
    }
    if(referred(m.type)=="Arena")throw Error(m.token.at,"Arena owners and borrows cannot be returned from a method", "BLT2300");
    if(m.is_constant_template&&(m.type=="void"||is_reference(m.type)))throw Error(m.token.at,"generic constants must have a non-void value type", "BLT2000");
    for(auto& p:m.parameters) {
        p.type=resolve(p.type,&c,p.token);
        if(m.native_abi=="C") {
            const auto t=referred(p.type);auto* native=find_class(t);
            if(!(is_numeric(t)||t=="bool"||t=="string"||is_pointer(t)||(is_array(t)&&array_rank(t)==1&&is_numeric(element_type(t)))||(native&&native->is_native)))throw Error(p.token.at,"Unsupported C ABI parameter; use fixed-width numbers, pointers, UTF-8 strings, numeric arrays, or an explicitly bound native type","BLT2500");
            if(p.variadic)throw Error(p.token.at,"Bolt variadic arrays are not C varargs; use a fixed-signature C wrapper","BLT2500");
        }
        if(p.type=="Arena")throw Error(p.token.at,"Arena parameters must borrow an owner through Arena&", "BLT2300");
    }
    if(m.id.empty())m.id=signature(m);
    if(!m.operator_symbol.empty()) {
        const auto n=m.parameters.size();const auto& op=m.operator_symbol;
        bool arity=(op=="+"||op=="-")?(n==1||n==2):(op=="!"||op=="~"||op=="++"||op=="--")?n==1:n==2;
        if(!arity||!m.is_public||!m.is_static)throw Error(m.token.at,"an operator must be public and static with the correct number of operands", "BLT2210");
        bool own=false;for(const auto& p:m.parameters){own|=referred(p.type)==c.name;if(is_reference(p.type)&&!p.constant)throw Error(p.token.at,"operator reference operands must be const; return an updated value instead", "BLT2210");}
        if(!own)throw Error(m.token.at,"an operator must have an operand of its declaring type '"+c.name+"'", "BLT2210");
        if(m.type=="void"||is_reference(m.type))throw Error(m.token.at,"operators must return a value (not void or a reference)", "BLT2210");
        if((op=="!"||op=="=="||op=="!="||op=="<"||op==">"||op=="<="||op==">=")&&m.type!="bool")throw Error(m.token.at,"comparison and logical-not operators must return bool", "BLT2210");
        if((op=="++"||op=="--")&&(m.type!=c.name||referred(m.parameters[0].type)!=c.name))throw Error(m.token.at,"increment/decrement must accept and return the declaring type", "BLT2210");
    }
    m.prepared=true;
}

void Analyzer::prepare(Class& c) {
    native_headers(c.attributes);
    if(c.prepared||c.is_template_member||(!c.generic_parameters.empty()||!c.pack_parameter.empty()))return;
    if(c.preparing)return;
    c.preparing=true;
    if(emitted_class(c)&&c.is_abstract&&!c.is_native&&!c.is_interface && c.token.at.file!="<bolt-stdlib>/System/IO/StandaloneStreams.bolt" && c.token.at.file!="<bolt-stdlib>/System/Ecs.bolt")program.require_cpp("abstract classes");
    if(c.is_struct&&(c.is_static||c.is_abstract))throw Error(c.token.at,"structs cannot be static or abstract","BLT2220");
    if(c.is_sealed&&c.is_abstract)throw Error(c.token.at,"a class cannot be sealed and abstract","BLT2220");
    // Parse the colon list once; at most one concrete base plus interfaces.
    std::vector<std::string> interfaces;
    std::unordered_set<std::string> bases;
    for(auto name:c.interfaces) {
        name=resolve(name,&c,c.token);
        auto* parent=find_class(name);
        if(!parent)throw Error(c.token.at,"a base must be a class or interface", "BLT2200");
        if(!bases.insert(name).second)throw Error(c.token.at,"duplicate base '"+name+"'", "BLT2200");
        if(parent->preparing)throw Error(c.token.at,"cyclic class/interface inheritance", "BLT2200");
        prepare(*parent);
        if(parent->is_interface)interfaces.push_back(name);
        else {
            if(c.is_interface)throw Error(c.token.at,"an interface can inherit only interfaces", "BLT2200");
            if(c.is_struct||parent->is_struct)throw Error(c.token.at,"structs do not participate in class inheritance; interfaces are allowed","BLT2220");
            if(!c.base.empty())throw Error(c.token.at,"a class may inherit only one class (and multiple interfaces)", "BLT2200");
            c.base=name;
        }
    }
    c.interfaces=std::move(interfaces);
    if(c.is_static&&!c.interfaces.empty())throw Error(c.token.at,"a static class cannot implement an interface", "BLT2200");
    if(!c.base.empty()) {
        c.base=resolve(c.base,&c,c.token);
        auto* b=find_class(c.base);
        if(!b)throw Error(c.token.at,"a class base must be a class", "BLT2000");
        if(b->preparing)throw Error(c.token.at,"cyclic class inheritance", "BLT2000");
        prepare(*b);
        if(b->is_native&&b->is_abstract&&!c.is_native)throw Error(c.token.at,"cannot implement a native abstract base directly in Bolt; use composition or a native adapter", "BLT2222");
        if(b->is_sealed)throw Error(c.token.at,"cannot inherit sealed class '"+b->name+"'", "BLT2000");
        if(c.is_static||b->is_static)throw Error(c.token.at,"static classes cannot participate in inheritance", "BLT2000");
    }
    std::unordered_set<std::string> names;
    for(auto& f:c.fields) {
        if(!names.insert(f.name).second)throw Error(f.token.at,"duplicate member '"+f.name+"'", "BLT2000");
        if(f.is_event&&f.event_type.empty()) {
            f.event_type=resolve(f.type,&c,f.token);
            auto arity=action_arity(f.event_type);
            if(arity<0)throw Error(f.token.at,"an event must use Action or Action<T...>", "BLT2700");
            f.type=generic_name("System.Events.EventStorage",generic_args(f.event_type));
        }
        f.type=resolve(f.type,&c,f.token);
        if(referred(f.type)=="Arena")throw Error(f.token.at,"Arena owners and borrows cannot be stored in fields", "BLT2300");
        if(is_reference(f.type))throw Error(f.token.at,"reference fields are not supported; use a pointer field or a method reference parameter", "BLT2000");
        if(!c.base.empty()&&field(*find_class(c.base),f.name).second)throw Error(f.token.at,"a derived field cannot hide a base field", "BLT2000");
    }
    for(auto& p:c.properties) {
        if(!names.insert(p.name).second)throw Error(p.token.at,"duplicate member '"+p.name+"'", "BLT2250");
        p.type=resolve(p.type,&c,p.token);
        for(auto& a:p.parameters)a.type=resolve(a.type,&c,a.token);
        if(referred(p.type)=="Arena")throw Error(p.token.at,"properties cannot return Arena owners", "BLT2250");
    }
    std::unordered_set<std::string> signatures;
    std::unordered_set<std::string> constant_names;
    for(const auto& m:c.methods)if(m.is_constant_template&&!m.is_specialization) {
        if(!constant_names.insert(m.name).second)throw Error(m.token.at,"duplicate generic constant '"+m.name+"'", "BLT2000");
        if(!c.base.empty()&&field(*find_class(c.base),m.name).second)throw Error(m.token.at,"a generic constant cannot hide a base field", "BLT2000");
    }
    for(auto& m:c.methods) {
        if(!m.is_constructor&&names.contains(m.name))throw Error(m.token.at,"field and method have the same name", "BLT2000");
        if(!m.is_constant_template&&constant_names.contains(m.name))throw Error(m.token.at,"a generic constant and method cannot have the same name", "BLT2000");
        if(c.is_interface) {
            if(!m.generic_parameters.empty()||!m.pack_parameter.empty())throw Error(m.token.at,"interface methods cannot have their own type parameters; use a generic interface instead", "BLT2200");
            m.is_public=true;m.is_virtual=true;m.is_abstract=!m.body;
            m.is_default_implementation=bool(m.body);
        }
        if(!m.operator_symbol.empty()&&(!m.generic_parameters.empty()||!m.pack_parameter.empty()))throw Error(m.token.at,"operators use their class's generic parameters, not a method generic list", "BLT2210");
        prepare_method(c,m);
        if(m.prepared&&!m.is_specialization&&!signatures.insert(m.id).second)throw Error(m.token.at,"duplicate method signature", "BLT2000");
        if(!m.pack_parameter.empty()&&m.type!="void"&&!m.native_pack&&!heterogeneous_pack(m))throw Error(m.token.at,"variadic fan-out methods must return void", "BLT2000");
        if(m.is_constructor&&(!m.generic_parameters.empty()||!m.pack_parameter.empty()))throw Error(m.token.at,"constructors use the class generic parameters, not a separate generic list", "BLT2000");
        if(m.is_static&&(m.is_override||m.is_virtual||m.is_sealed))throw Error(m.token.at,"static methods cannot be virtual, override, or sealed", "BLT2000");
        if((!m.generic_parameters.empty()||!m.pack_parameter.empty())&&(m.is_virtual||m.is_override||m.is_sealed))
            throw Error(m.token.at,"generic methods cannot be virtual, override, or sealed", "BLT2000");
        if(m.is_destructor){if(emitted_class(c))program.require_cpp("destructors and RAII");if(m.is_static||m.is_abstract||m.is_async)throw Error(m.token.at,"invalid destructor modifiers","BLT2220");continue;}
        if(m.is_abstract){if(!c.is_interface&&!c.is_abstract&&!c.is_native)throw Error(m.token.at,"abstract methods require an abstract class","BLT2220");if(m.is_static||m.is_async)throw Error(m.token.at,"abstract methods cannot be static or async","BLT2220");m.is_virtual=true;}
        if(m.readonly_receiver&&(m.is_static||m.is_constructor||m.is_destructor))throw Error(m.token.at,"ReadOnly is only valid on instance readers/methods", "BLT2250");
        if(m.is_constructor||!m.prepared)continue;
        const Method* inherited=nullptr;
        if(!c.base.empty())for(auto [o,b]:methods(*find_class(c.base),m.name)) {
            (void)o;
            if(b->id==m.id) {
                inherited=b;
                break;
            }
        }
        if(!inherited)for(const auto& i:c.interfaces)for(auto [o,b]:methods(*find_class(i),m.name)) {
            (void)o;if(b->id==m.id){inherited=b;break;}
        }
        if(m.is_override&&!inherited)throw Error(m.token.at,"override does not match an inherited method", "BLT2000");
        if(inherited) {
            if(inherited->is_sealed)throw Error(m.token.at,"cannot override sealed method '"+m.name+"'", "BLT2000");
            if(inherited->is_static||m.is_static||(!inherited->is_public&&!inherited->is_protected))throw Error(m.token.at,"cannot override a static or private method", "BLT2000");
            if(inherited->type!=m.type)throw Error(m.token.at,"override return type must match the base method", "BLT2000");
            if(inherited->is_conversion&&inherited->is_implicit&&!m.is_implicit)
                throw Error(m.token.at,"an implicit conversion contract cannot be implemented by an explicit conversion", "BLT2604");
            if(inherited->is_public&&!m.is_public)throw Error(m.token.at,"override cannot reduce public access", "BLT2000");
            if(inherited->is_protected&&!m.is_public&&!m.is_protected)throw Error(m.token.at,"override cannot reduce protected access", "BLT2000");
            m.is_override=true;m.is_virtual=inherited->is_virtual;
            if(inherited->readonly_receiver&&!m.readonly_receiver)throw Error(m.token.at,"override must preserve the ReadOnly receiver contract", "BLT2250");
            if(!inherited->readonly_receiver)m.readonly_receiver=false;
            if(inherited->is_extern)m.native_name=inherited->native_name.empty()?inherited->name:inherited->native_name;
        }
        if(!m.is_static&&(m.is_public||m.is_protected)&&!c.is_struct&&!c.is_sealed)m.is_virtual=true;
    }
    // Interface defaults are selected by nominal specificity, not traversal order.
    // A concrete class/base implementation always wins. A derived abstract
    // redeclaration suppresses its inherited default.
    std::map<std::string,std::vector<std::pair<const Class*,const Method*>>> contracts;
    std::unordered_set<std::string> visited;
    std::function<void(const Class&)> collect=[&](const Class& item) {
        if(!visited.insert(item.name).second)return;
        for(const auto& m:item.methods)if(item.is_interface||m.is_abstract)
            contracts[m.id].emplace_back(&item,&m);
        for(const auto& i:item.interfaces)collect(*find_class(i));
        if(!item.base.empty())collect(*find_class(item.base));
    };
    collect(c);
    for(const auto& [id,all]:contracts) {
        std::vector<std::pair<const Class*,const Method*>> closest;
        for(auto candidate:all) {
            bool shadowed=false;
            for(auto other:all)if(candidate.first!=other.first&&derives(other.first->name,candidate.first->name)){shadowed=true;break;}
            if(!shadowed)closest.push_back(candidate);
            if(candidate.second->type!=all.front().second->type)
                throw Error(c.token.at,"inherited interface contracts disagree on the return type for '"+candidate.second->name+"'", "BLT2203");
        }
        if(c.is_interface)continue;
        const Class* owner=&c;const Method* implementation=nullptr;
        while(owner&&!implementation){implementation=method_by_id(*owner,id);owner=owner->base.empty()?nullptr:find_class(owner->base);}
        const Method* contract=all.front().second;
        bool requires_implicit=false;for(auto item:all)requires_implicit|=item.second->is_conversion&&item.second->is_implicit;
        if(!implementation) {
            std::vector<std::pair<const Class*,const Method*>> defaults;
            for(auto item:closest)if(item.second->body&&!item.second->is_abstract)defaults.push_back(item);
            if(defaults.size()>1)throw Error(c.token.at,"ambiguous interface default for '"+contract->name+"'; implement it in '"+c.name+"'", "BLT2205");
            // Native aliases are fixed foreign layouts. Defaults remain
            // interface methods; do not manufacture an ABI export in the
            // foreign class for a source-authored interface implementation.
            if(defaults.size()==1&&!c.is_native) {
                auto copy=clone(*defaults.front().second);
                copy.is_abstract=false;copy.is_default_implementation=true;
                copy.default_owner=defaults.front().first->name;
                copy.is_override=true;copy.is_virtual=true;copy.id=id;
                c.methods.push_back(std::move(copy));prepare_method(c,c.methods.back());
                implementation=&c.methods.back();
            }
        }
        if(!c.is_abstract&&!c.is_native&&(!implementation||implementation->is_abstract||implementation->is_static||!implementation->is_public||implementation->type!=contract->type))
            throw Error(c.token.at,"class '"+c.name+"' must implement public "+contract->type+" "+id+" required by its interface", "BLT2201");
        if(implementation&&requires_implicit&&!implementation->is_implicit)
            throw Error(implementation->token.at,"an implicit interface conversion requires an implicit implementation", "BLT2604");
    }
    c.preparing=false;
    c.prepared=true;
}

void Analyzer::prepare_enum(Enum& en) {
        if(en.prepared||en.is_template_member)return;
        en.prepared=true;
        if(!is_integer(en.underlying))throw Error(en.token.at,"enum underlying type must be byte, short, ushort, int, uint, long, or ulong", "BLT2000");
        std::unordered_map<std::string,Value> values;
        Value next=default_primitive(en.underlying);
        std::unordered_set<std::string> names;
        std::function<Value(const Expr&)> constant=[&](const Expr& e)->Value {
            if(e.kind==Expr::Kind::SizeOf) {
                Class context;context.name=en.declaring_type;context.namespace_name=en.namespace_name;context.imports=en.imports;
                auto* owner=find_class(en.declaring_type);
                auto sized=clone(e);size_of(*sized,owner?owner:&context);
                return Value::signed_integer("int",std::stoll(sized->resolved));
            }
            if(e.kind==Expr::Kind::Literal) {
                auto v=literal_value(e);
                if(!is_integer(v.type))throw Error(e.token.at,"enum values must be integer constants", "BLT2000");
                return v;
            }
            if(e.kind==Expr::Kind::Name) {
                auto i=values.find(e.text);
                if(i!=values.end())return i->second;
            }
            if(e.kind==Expr::Kind::Member) {
                auto i=values.find(e.text);
                auto owner=qualified_expr(*e.args[0]);
                if(i!=values.end()&&(owner==en.name||owner==en.short_name))return i->second;
                if(is_numeric(owner)&&!e.arrow&&e.type_args.empty()&&e.args[0]->type_args.empty()&&(e.text=="MinValue"||e.text=="MaxValue")) {
                    if(!is_integer(owner))throw Error(e.token.at,"enum values must be integer constants", "BLT2000");
                    return numeric_limit_value(owner,e.text=="MaxValue");
                }
            }
            if(e.kind==Expr::Kind::Binary) {
                auto a=constant(*e.args[0]),b=constant(*e.args[1]);
                return numeric_binary(e.text,common_numeric(a.type,b.type),a,b);
            }
            if(e.kind==Expr::Kind::Unary&&(e.text=="+"||e.text=="-")) {
                auto v=constant(*e.args[0]);
                return e.text=="+"?v:numeric_binary("-",promoted(v.type),default_primitive(promoted(v.type)),v);
            }
            throw Error(e.token.at,"enum initializer must be a constant integer expression", "BLT2000");
        };
        for(std::size_t i=0;i<en.members.size();++i) {
            auto& m=en.members[i];
            if(!names.insert(m.name).second)throw Error(m.token.at,"duplicate enum member", "BLT2000");
            try {
                auto v=convert_value(en.underlying,m.expression?constant(*m.expression):next);
                m.value=v.to_string();
                values.emplace(m.name,v);
                if(i+1<en.members.size()&&!en.members[i+1].expression) {
                    auto one=convert_value(en.underlying,Value(1));
                    next=convert_value(en.underlying,numeric_binary("+",promoted(en.underlying),v,one));
                }
            }catch(const Error&) {
                throw;
            }catch(const std::exception& ex) {
                throw Error(m.token.at,ex.what(), "BLT2000");
            }
        }
}

void Analyzer::enums() {for(std::size_t i=0;i<program.enums.size();++i)prepare_enum(program.enums[i]);}

void Analyzer::layouts() {
    program.layout_order.clear();
    std::unordered_map<std::string,int> seen;
    std::function<void(const std::string&,std::size_t)> layout=[&](const std::string& t,std::size_t depth) {
        if(is_optional(t)){layout(element_type(t),depth+1);return;}
        auto* c=find_class(t);
        if(!c||(!c->generic_parameters.empty()||!c->pack_parameter.empty())||!emitted_class(*c))return;
        if(depth>128)throw Error(c->token.at,"class layout nesting limit exceeded", "BLT2000");
        if(seen[t]==1)throw Error(c->token.at,"recursive by-value class layout involving '"+t+"'", "BLT2000");
        if(seen[t]==2)return;
        seen[t]=1;
        if(!c->base.empty())layout(c->base,depth+1);
        for(const auto& i:c->interfaces)layout(i,depth+1);
        for(const auto& f:c->fields)if(!f.is_static&&!is_pointer(f.type)&&!is_reference(f.type)&&!is_array(f.type)&&!is_collection(f.type))layout(f.type,depth+1);
        seen[t]=2;
        for(std::size_t i=0;i<program.classes.size();++i)if(&program.classes[i]==c) {
            program.layout_order.push_back(i);
            break;
        }
    };
    for(const auto& c:program.classes)layout(c.name,0);
}

} // namespace bolt::semantic
