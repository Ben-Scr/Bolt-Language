#include "Analyzer.hpp"
#include <bolt/Types.hpp>
#include <algorithm>

namespace bolt::semantic {

bool Analyzer::protected_scope(const Class& owner) const {
    if(!current_class)return false;
    const Class* scope=current_class;
    std::unordered_set<std::string> seen;
    while(scope&&seen.insert(scope->name).second) {
        if(scope->name==owner.name)return true;
        if(scope->base.empty())break;
        scope=find_class(scope->base);
    }
    return false;
}

bool Analyzer::member_accessible(const Class& owner,bool pub,bool prot) const {
    if(pub)return true;
    if(current_class&&current_class->name==owner.name)return true;
    return prot&&protected_scope(owner);
}

void Analyzer::accessible(const Class& owner,bool pub,bool prot,const Expr& e) const {
    if(member_accessible(owner,pub,prot))return;
    if(!prot&&current_class&&current_class->delegate_owner==owner.name&&current_method&&current_method->name=="Invoke"&&e.text==current_class->delegate_method)return;
    throw Error(e.token.at,"'"+owner.name+"."+e.text+"' is "+(prot?"protected":"private"), "BLT2000");
}

std::pair<Class*,const Field*> Analyzer::field(Class& c,const std::string& n) {
    prepare(c);
    if(auto* f=find_field(c,n))return {
        &c,f
    };
    if(!c.base.empty())return field(*find_class(c.base),n);
    return {
        nullptr,nullptr
    };
}

std::pair<Class*,const Property*> Analyzer::property(Class& c,const std::string& n) {
    prepare(c);
    if(auto* p=find_property(c,n))return {&c,p};
    if(!c.base.empty())if(auto found=property(*find_class(c.base),n);found.second)return found;
    for(const auto& base:c.interfaces)if(auto found=property(*find_class(base),n);found.second)return found;
    return {nullptr,nullptr};
}

std::vector<std::pair<Class*,Method*>> Analyzer::methods(Class& c,const std::string& n,bool constructors) {
    prepare(c);
    std::vector<std::pair<Class*,Method*>> out;
    std::unordered_set<std::string> seen;
    auto lookup_key = [](const Method& m) {
        if (m.generic_parameters.empty() && m.pack_parameter.empty()) return m.id;
        return signature(m) + "#" + std::to_string(m.generic_parameters.size()) +
            (m.pack_parameter.empty() ? "" : "...");
    };
    for(auto& m:c.methods)if((constructors?m.is_constructor:(!m.is_constructor&&m.name==n))&&!m.is_specialization) {
        out.emplace_back(&c,&m);
        seen.insert(lookup_key(m));
    }
    if(!constructors&&!c.base.empty())for(auto [o,m]:methods(*find_class(c.base),n))if(seen.insert(lookup_key(*m)).second)out.emplace_back(o,m);
    if(!constructors)for(const auto& i:c.interfaces)for(auto [o,m]:methods(*find_class(i),n))if(seen.insert(lookup_key(*m)).second)out.emplace_back(o,m);
    return out;
}

std::string Analyzer::qualified_expr(const Expr& e) const {
    if(e.kind==Expr::Kind::Name)return e.text;
    if(e.kind==Expr::Kind::Member&&!e.arrow) {
        auto base=qualified_expr(*e.args[0]);
        if(!base.empty())return base+"."+e.text;
    }
    return {
    };
}

bool Analyzer::namespace_exists(const std::string& n) const {
    if(n=="System"||n=="System.Collections"||n=="System.Collections.Generic")return true;
    for(const auto& c:program.classes)if(c.namespace_name==n||c.namespace_name.starts_with(n+"."))return true;
    for(const auto& e:program.enums)if(e.namespace_name==n||e.namespace_name.starts_with(n+"."))return true;
    for(const auto& c:program.concepts)if(c.namespace_name==n||c.namespace_name.starts_with(n+"."))return true;
    return false;
}

bool Analyzer::bind_type_or_namespace(Expr& e,const std::string& n) {
    auto name=resolve_name(generic_base(n),current_class);
    if(!collection_kind(generic_base(n)).empty()||!name.empty()) {
        if(!name.empty())require_type_access(name,current_class,e.token);
        if(auto* en=find_enum(name)) {
            prepare_enum(*en);
            e.binding=Expr::Binding::Enum;
            e.owner=en->name;
            e.type="@enum:"+en->name;
            return true;
        }
        std::string requested=n;
        if(!e.type_args.empty())requested=generic_name(n,e.type_args);
        if(auto* c=find_class(name);c&&c->is_static&&e.type_args.empty()) {
            e.binding=Expr::Binding::Class;
            e.owner=name;
            e.type="@class:"+name;
            return true;
        }
        const auto t=resolve(requested,current_class,e.token);
        e.binding=Expr::Binding::Class;
        e.owner=t;
        e.type="@class:"+t;
        return true;
    }
    if(namespace_exists(n)) {
        if(!e.type_args.empty())throw Error(e.token.at,"namespaces are not generic", "BLT2000");
        e.binding=Expr::Binding::Namespace;
        e.owner=n;
        e.type="@namespace:"+n;
        return true;
    }
    return false;
}

void Analyzer::bind_member(Expr& e,Class& c,bool class_receiver,bool lvalue,bool constant) {
    if(auto [owner,p]=property(c,e.text);p) {
        if(!e.type_args.empty())throw Error(e.token.at,"property '"+e.text+"' is not a generic method", "BLT2250");
        accessible(*owner,p->is_public,p->is_protected,e);
        if(class_receiver!=p->is_static)throw Error(e.token.at,p->is_static?"access static properties through their type":"instance property requires an object", "BLT2250");
        if(e.token.text==p->name)program.semantic_references.push_back({e.token,"property:"+owner->name+":"+p->name,p->name,"property",p->type+" "+p->name,owner->name,p->token.at});
        e.binding=Expr::Binding::UserProperty;e.owner=owner->name;e.type=referred(p->type);e.declared_type=p->type;
        e.is_static=p->is_static;e.lvalue=p->is_static||lvalue;e.constant=!p->is_static&&constant;
        return;
    }
    if(auto [owner,f]=field(c,e.text);f) {
        if(!e.type_args.empty())throw Error(e.token.at,"field '"+e.text+"' is not generic", "BLT2000");
        const bool reflected=generated_metadata(e.token) && owner->is_reflected &&
            std::any_of(f->attributes.begin(),f->attributes.end(),[](const Attribute& a){return a.name=="Reflect";});
        if(!reflected)accessible(*owner,f->is_public,f->is_protected,e);
        if(class_receiver!=f->is_static)throw Error(e.token.at,f->is_static?"access static fields through their class name":"instance field requires an object", "BLT2000");
        if(e.token.text==f->name)program.semantic_references.push_back({e.token,"field:"+owner->name+":"+f->name,f->name,f->is_event?"event":f->native_getter.empty()?"field":"property",f->type+" "+f->name,owner->name,f->token.at});
        if(owner->is_native)program.require_cpp("native standard library / interop");
        if(f->is_event&&!e.authorized_event_access) {
            e.binding=Expr::Binding::Event;e.owner=owner->name;e.type="#event";
            e.declared_type=f->type;e.is_static=f->is_static;e.lvalue=f->is_static||lvalue;e.constant=constant;
            return;
        }
        e.binding=Expr::Binding::Field;
        e.owner=owner->name;
        e.type=f->type;
        e.is_static=f->is_static;
        e.lvalue=f->native_getter.empty()&&(f->is_static||lvalue);
        e.constant=f->constant||(!f->is_static&&constant);
        return;
    }
    auto candidates=methods(c,e.text);
    if(candidates.empty())throw Error(e.token.at,"class '"+c.name+"' has no member '"+e.text+"'", "BLT2004");
    e.binding=Expr::Binding::Method;
    e.owner=c.name;
    e.is_static=class_receiver;
    e.type="#method";
    e.constant=constant;
}

void Analyzer::bind_constant_template(Expr& e,bool callee) {
    auto candidates=methods(*find_class(e.owner),e.text);
    candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[](const auto& candidate) {
        return !candidate.second->is_constant_template;
    }),candidates.end());
    if(candidates.empty())return;
    if(callee)throw Error(e.token.at,"generic constants are values; omit the call parentheses", "BLT2000");
    if(e.type_args.empty())throw Error(e.token.at,"generic constant '"+e.text+"' requires explicit type arguments", "BLT2000");
    std::vector<std::string> type_args;
    for(const auto& t:e.type_args)type_args.push_back(resolve(t,current_class,e.token));
    std::vector<Expr*> arguments;
    auto [owner,m]=select(std::move(candidates),type_args,arguments,e.token);
    accessible(*owner,m->is_public,m->is_protected,e);
    if(!e.is_static)throw Error(e.token.at,"access generic constants through their class name", "BLT2000");
    if(m->analyzing)throw Error(e.token.at,"recursive generic constant initializer for '"+e.text+"'", "BLT2000");
    if(!m->analyzed) {
        // Initializers are analyzed in their declaration's scope; retain the
        // caller's locals, method and loop state during nested expansion.
        auto* caller_class=current_class;
        auto* caller_method=current_method;
        const bool caller_static=static_context;
        const int caller_loops=loops;
        auto caller_scopes=std::move(scopes);auto caller_flow=flow;
        auto restore=[&] {
            current_class=caller_class;
            current_method=caller_method;
            static_context=caller_static;
            loops=caller_loops;
            scopes=std::move(caller_scopes);flow=std::move(caller_flow);
        };
        try { analyze_method(*owner,*m); }
        catch(...) { restore(); throw; }
        restore();
    }
    auto initializer=clone(*m->body->statements[0]->expression);
    // A declaration can convert its initializer (for example double to
    // float). Preserve that conversion when replacing the value reference.
    if(initializer->type!=m->type) {
        auto conversion=std::make_unique<Expr>(Expr::Kind::Call,e.token,"@explicit-cast");
        conversion->binding=Expr::Binding::Cast;
        conversion->type=m->type;
        if(auto* en=find_enum(m->type))conversion->declared_type=en->underlying;
        auto target=std::make_unique<Expr>(Expr::Kind::Name,e.token,m->type);
        target->binding=Expr::Binding::Cast;
        target->owner=m->type;
        target->type="#cast";
        conversion->args.push_back(std::move(target));
        conversion->args.push_back(std::move(initializer));
        initializer=std::move(conversion);
    }
    initializer->token.at=e.token.at;
    initializer->constant_owner=owner->name;
    initializer->constant_token=e.token;
    initializer->lvalue=false;
    initializer->constant=true;
    e=std::move(*initializer);
}

ExprPtr Analyzer::temporary_name(const Token& at,const std::string& name) {
    return std::make_unique<Expr>(Expr::Kind::Name,at,name);
}

ExprPtr Analyzer::property_call(const Expr& source,bool get) {
    auto* c=find_class(source.owner);
    const auto* p=find_property(*c,source.text);
    const auto& name=get?p->getter:p->setter;
    if(name.empty())throw Error(source.token.at,"property '"+p->name+"' has no "+(get?"getter":"setter"), "BLT2251");
    ExprPtr target;
    if(source.kind==Expr::Kind::Member||source.kind==Expr::Kind::Index) {
        target=std::make_unique<Expr>(Expr::Kind::Member,source.token,name);
        target->arrow=source.arrow;target->args.push_back(clone(*source.args[0]));
    }else target=std::make_unique<Expr>(Expr::Kind::Name,source.token,name);
    auto call=std::make_unique<Expr>(Expr::Kind::Call,source.token);
    call->args.push_back(std::move(target));
    if(source.kind==Expr::Kind::Index)for(std::size_t i=1;i<source.args.size();++i)call->args.push_back(clone(*source.args[i]));
    return call;
}

void Analyzer::read_property(Expr& e) {
    auto call=property_call(e,true);expression(*call);
    e=std::move(*call);
}

void Analyzer::update_property(Expr& e) {
    using K=Expr::Kind;
    auto original=clone(*e.args[0]);
    auto* c=find_class(original->owner);const auto* p=find_property(*c,original->text);
    if(is_reference(p->type)) {
        read_property(*e.args[0]); // ref properties behave like explicitly returned aliases.
        expression(e);return;
    }
    // Get-only auto properties can be initialized by their own constructor.
    if(p->setter.empty()&&e.text=="="&&!p->backing_field.empty()&&current_method&&current_method->is_constructor&&current_class==c&&
        (original->kind==K::Name||(original->kind==K::Member&&!original->arrow&&original->args[0]->binding==Expr::Binding::This))) {
        auto field=std::make_unique<Expr>(K::Name,original->token,p->backing_field);
        e.args[0]=std::move(field);expression(e);return;
    }
    if(p->setter.empty())throw Error(original->token.at,"property '"+p->name+"' is read-only (no set accessor)", "BLT2251");
    if(!original->lvalue||original->constant)throw Error(original->token.at,"a property setter requires a mutable, addressable receiver", "BLT2252");
    struct Capture { std::string name,type; ExprPtr value; };
    std::vector<Capture> captures;
    auto bind=[&](ExprPtr init,std::string type) {
        auto name="@property_temp"+std::to_string(++property_temporary);
        captures.push_back({name,std::move(type),std::move(init)});return temporary_name(e.token,name);
    };
    // Capture receiver and each index before evaluating the getter or RHS.
    if(!p->is_static&&(original->kind==K::Member||original->kind==K::Index)){
        auto type=original->args[0]->type+(original->arrow?"":"&");
        original->args[0]=bind(std::move(original->args[0]),type);
    }
    if(original->kind==K::Index)for(std::size_t i=1;i<original->args.size();++i){
        auto t=p->parameters.at(i-1).type;
        original->args[i]=bind(std::move(original->args[i]),t);
    }
    auto set_value=[&](ExprPtr assigned)->ExprPtr {
        // The result of an assignment is the converted RHS, not a reread of the property.
        auto value=bind(std::move(assigned),referred(p->type));
        auto set=property_call(*original,false);set->args.push_back(clone(*value));
        auto result=std::make_unique<Expr>(K::Binary,e.token,",");result->args.push_back(std::move(set));result->args.push_back(std::move(value));return result;
    };
    ExprPtr result;
    if(e.text=="=")result=set_value(clone(*e.args[1]));
    else {
        auto old=bind(property_call(*original,true),referred(p->type));
        if(e.text=="??=") {
            // RHS captures must stay in the null branch, otherwise RHS side effects would be eager.
            const auto outer=captures.size();
            auto yes=set_value(clone(*e.args[1]));
            for(auto i=captures.size();i>outer;--i){auto& a=captures[i-1];auto let=std::make_unique<Expr>(K::Let,e.token,a.name);let->declared_type=a.type;let->args.push_back(std::move(a.value));let->args.push_back(std::move(yes));yes=std::move(let);}
            captures.resize(outer);
            auto condition=std::make_unique<Expr>(K::Binary,e.token,"==");condition->args.push_back(clone(*old));auto token=e.token;token.kind=Token::Kind::Identifier;condition->args.push_back(std::make_unique<Expr>(K::Literal,token,"null"));
            result=std::make_unique<Expr>(K::Conditional,e.token,"?:");result->args.push_back(std::move(condition));result->args.push_back(std::move(yes));result->args.push_back(std::move(old));
        }else if(e.kind==K::Unary||e.kind==K::Postfix) {
            auto updated=bind(clone(*old),referred(p->type));
            auto change=std::make_unique<Expr>(K::Unary,e.token,e.text);change->args.push_back(clone(*updated));
            auto set=property_call(*original,false);set->args.push_back(clone(*updated));
            auto perform=std::make_unique<Expr>(K::Binary,e.token,",");perform->args.push_back(std::move(change));perform->args.push_back(std::move(set));
            result=std::make_unique<Expr>(K::Binary,e.token,",");result->args.push_back(std::move(perform));result->args.push_back(e.kind==K::Postfix?std::move(old):std::move(updated));
        }else {
            auto combine=std::make_unique<Expr>(K::Binary,e.token,e.text.substr(0,e.text.size()-1));combine->args.push_back(std::move(old));combine->args.push_back(clone(*e.args[1]));
            result=set_value(std::move(combine));
        }
    }
    for(auto i=captures.size();i>0;--i){auto& a=captures[i-1];auto let=std::make_unique<Expr>(K::Let,e.token,a.name);let->declared_type=a.type;let->args.push_back(std::move(a.value));let->args.push_back(std::move(result));result=std::move(let);}
    expression(*result);e=std::move(*result);
}

void Analyzer::bind_name(Expr& e, bool callee) {
    using B = Expr::Binding;
    if(const auto* s=symbol(e.text)) {
        if(!e.type_args.empty())throw Error(e.token.at,"a local variable is not a generic type or method", "BLT2000");
        e.binding=B::Local;
        e.local_id=s->id;
        e.type=referred(s->type);
        e.declared_type=s->type;
        e.constant=s->constant;
        e.lvalue=true;
    } else if(e.text=="this") {
        if(!e.type_args.empty())throw Error(e.token.at,"'this' is not generic", "BLT2000");
        if(static_context||!current_class||!current_method)throw Error(e.token.at,"'this' is only available in non-static instance methods and constructors", "BLT2000");
        e.binding=B::This;
        e.type=current_class->name;
        e.constant=current_method->readonly_receiver;
        // The receiver is an lvalue so member access and explicit
        // references/pointers operate on the original instance.
        e.lvalue=true;
    } else if(member_context()&&(field(*member_context(),e.text).second||property(*member_context(),e.text).second||!methods(*member_context(),e.text).empty())) {
        auto f=field(*member_context(),e.text);auto prop=property(*member_context(),e.text);
        bool stat=f.second?f.second->is_static:prop.second?prop.second->is_static:methods(*member_context(),e.text)[0].second->is_static;
        if(!stat&&static_context)throw Error(e.token.at,"instance member is unavailable in a static context", "BLT2000");
        bind_member(e,*member_context(),stat,true,current_method&&current_method->readonly_receiver);
    }else if(callee&&e.text=="Random"&&find_class("System.Random")) {
        if(!imported(current_class,"Random")&&!imported(current_class,"System"))throw Error(e.token.at,"missing import Random","BLT2000");
        e.binding=B::Class;e.owner="System.Random";e.type="@class:System.Random";
    }else if(e.text=="Random"&&find_class("System.SharedRandom")) {
        if(!imported(current_class,"Random")&&!imported(current_class,"System"))throw Error(e.token.at,"missing import Random", "BLT2000");
        e.binding=B::Class;e.owner="System.SharedRandom";e.type="@class:"+e.owner;
    }else if(e.text=="Runtime"||e.text=="Math"||e.text=="Type"||e.text=="Arena") {
        if(!e.type_args.empty())throw Error(e.token.at,"builtin class '"+e.text+"' is not generic", "BLT2000");
        if(e.text!="Console"&&e.text!="Runtime"&&e.text!="Arena"&&!imported(current_class,e.text))throw Error(e.token.at,"missing import "+e.text, "BLT2000");
        e.binding=B::Builtin;
        e.owner=e.text;
        e.type="@builtin:"+e.text;
    }else if(callee&&(is_primitive(e.text))) {
        e.binding=B::Cast;
        e.owner=resolve(e.text,current_class,e.token);
        e.type="#cast";
    }else if(e.text=="string"){e.binding=B::Builtin;e.owner="String";e.type="@builtin:String";}else if(is_numeric(e.text)) {
        if(!e.type_args.empty())throw Error(e.token.at,"numeric types are not generic", "BLT2000");
        e.binding=B::Builtin;
        e.owner=resolve(e.text,current_class,e.token);
        e.type="@builtin:"+e.owner;
    } else {
        Class* global=nullptr;
        if(current_class)global=find_class(fullname("@Global",current_class->namespace_name));
        if(current_class&&current_class->namespace_name.empty())global=find_class("@Global");
        if(global&&!methods(*global,e.text).empty()) {
            e.binding=B::Method;
            e.owner=global->name;
            e.is_static=true;
            e.type="#method";
        } else if(callee&&e.text=="Reinterpret") {
            e.binding=B::Builtin;
            e.owner="Intrinsic";
            e.type="#builtin";
        } else if(callee&&bind_implicit_static(e)) {
            // Ordinary library methods, not compiler intrinsics.
        } else if(!bind_type_or_namespace(e,e.text))throw Error(e.token.at,"unknown identifier '"+e.text+"'", "BLT2003");
    }
}

void Analyzer::bind_member_access(Expr& e, bool callee) {
    using B = Expr::Binding;
    auto& base=*e.args[0];
    if(base.binding==B::Namespace) {
        auto n=base.owner+"."+e.text;
        if(!bind_type_or_namespace(e,n)) {
            auto* global=find_class("@Global."+base.owner);
            if(global&&!methods(*global,e.text).empty()) {
                e.binding=B::Method;
                e.owner=global->name;
                e.is_static=true;
                e.type="#method";
            }else throw Error(e.token.at,"unknown namespace member '"+n+"'", "BLT2000");
        }
    }else if(base.binding==B::Class&&!e.arrow&&(find_class(base.owner+"."+e.text)||find_enum(base.owner+"."+e.text))&&bind_type_or_namespace(e,base.owner+"."+e.text)) {
        // A lexical nested declaration is addressed through its type owner.
    }else if(base.binding==B::Enum) {
        if(!e.type_args.empty())throw Error(e.token.at,"enum constants are not generic", "BLT2000");
        auto* en=find_enum(base.owner);
        auto it=std::find_if(en->members.begin(),en->members.end(),[&](const auto& m) {
            return m.name==e.text;
        });
        if(it==en->members.end())throw Error(e.token.at,"unknown enum member", "BLT2000");
        e.binding=B::EnumValue;
        e.type=en->name;
        e.owner=en->name;
        e.resolved=it->value;
        e.declared_type=en->underlying;
        e.constant=true;
    }else if(base.type.starts_with("@builtin:")) {
        e.owner=base.owner;
        e.binding=B::Builtin;
        e.type="#builtin";
        if(e.owner=="String"){if(e.text=="MinValue"||e.text=="MaxValue")throw Error(e.token.at,"unknown identifier 'string."+e.text+"'","BLT2001");program.require_cpp("Unicode string methods");e.is_static=true;if(e.text=="Empty"){e.binding=B::Property;e.type="string";e.constant=true;}}else if(e.owner=="Arena") {
            if(e.arrow)throw Error(e.token.at,"access Arena factories with .", "BLT2300");
            if(e.text!="Fixed"&&e.text!="Growable")throw Error(e.token.at,"unknown Arena factory '"+e.text+"'", "BLT2300");
            if(!e.type_args.empty())throw Error(e.token.at,"Arena factories are not generic", "BLT2300");
            e.is_static=true;
        }else if(is_numeric(e.owner)) {
            if(e.arrow)throw Error(e.token.at,"access numeric type members with .", "BLT2000");
            if(e.text=="Parse"||e.text=="TryParse") {
                if(!e.type_args.empty())throw Error(e.token.at,"numeric parsing methods are not generic", "BLT2000");
                e.is_static=true;
            }else {
                if(e.text!="MinValue"&&e.text!="MaxValue")throw Error(e.token.at,"unknown numeric type member '"+e.text+"'", "BLT2000");
                if(!e.type_args.empty())throw Error(e.token.at,"numeric constants are not generic", "BLT2000");
                e.binding=B::Property;
                e.type=e.owner;
                e.constant=true;
            }
        }else if(e.owner=="Math"&&(e.text=="Pi"||e.text=="E"||e.text=="Tau")) {
            if(e.arrow)throw Error(e.token.at,"access Math constants with .", "BLT2000");
            if(e.type_args.size()>1)throw Error(e.token.at,"Math."+e.text+" accepts one optional FloatingPoint type argument", "BLT2000");
            e.binding=B::Property;
            e.type=e.type_args.empty()?"double":resolve(e.type_args[0],current_class,e.token);
            if(!is_floating(e.type))throw Error(e.token.at,"Math."+e.text+" requires the FloatingPoint concept (float or double)", "BLT2000");
            if(!e.type_args.empty())e.type_args[0]=e.type;
            e.constant=true;
        }
    }else {
        bool class_receiver=base.binding==B::Class;
        auto t=class_receiver?base.owner:base.type;
        if(e.arrow) {
            if(!is_pointer(t))throw Error(e.token.at,"-> requires a pointer; use . for values/references", "BLT2000");
            t=element_type(t);
        } else if(is_pointer(t))throw Error(e.token.at,"use -> to access pointer members", "BLT2000");
        auto* member_class=find_class(t);
        const bool declared_member=member_class&&(field(*member_class,e.text).second||property(*member_class,e.text).second||!methods(*member_class,e.text).empty());
        if(callee&&!class_receiver&&!declared_member&&!builtin_instance_member(t,e.text)&&bind_extension_member(e)) {
            // Keep the evaluated receiver until argument types select the static overload.
        }else if(t=="Arena") {
            if(!base.lvalue)throw Error(e.token.at,"Arena access requires an owner or borrowed parameter", "BLT2300");
            if(!e.type_args.empty())throw Error(e.token.at,"Arena members are not generic", "BLT2300");
            e.owner="Arena";
            if(e.text=="UsedBytes"||e.text=="CapacityBytes"||e.text=="PeakBytes"||e.text=="LimitBytes") {
                e.binding=B::Property;e.type="ulong";e.constant=true;
            }else if(e.text=="Reset"||e.text=="Dispose") {
                if(base.constant)throw Error(e.token.at,"cannot modify a const Arena", "BLT2300");
                e.binding=B::Builtin;e.type="#builtin";
            }else throw Error(e.token.at,"unknown Arena member '"+e.text+"'", "BLT2300");
        }else if(!class_receiver&&(is_primitive(t)||find_enum(t)||is_collection(t))&&e.text=="ToString") {
            if(!stringable(t))throw Error(e.token.at,"ToString() requires a primitive, enum, or IStringable value", "BLT2204");
            e.binding=B::Builtin;e.owner="Primitive";e.type="#builtin";
        } else if(!class_receiver&&t=="string"&&e.text=="ByteLength"){
            program.require_cpp("Unicode string ByteLength");e.binding=B::Property;e.owner="string";e.type="int";e.constant=true;
        } else if(!class_receiver&&t=="string"&&e.text!="Length"&&e.text!="Count") {
            e.binding=B::Builtin;e.owner="String";e.type="#builtin";e.is_static=false;
        } else if(!class_receiver&&is_array(t)&&e.text=="GetLength") {
            e.binding=B::Builtin;e.owner="Array";e.type="#builtin";
        } else if(!class_receiver&&(((is_array(t)||is_collection(t)||t=="string")&&(e.text=="Length"||e.text=="Count"||(collection_kind(t)=="List"&&e.text=="Capacity")))||(is_array(t)&&e.text=="Rank"))) {
            if(!e.type_args.empty())throw Error(e.token.at,"collection properties are not generic", "BLT2000");
            e.binding=B::Property;
            e.owner=t;

            e.type="int";
            e.constant=true;
        } else if(!class_receiver&&is_collection(t)) {
            e.binding=B::Collection;
            e.owner=t;
            e.type="#collection";
        } else if(auto* c=find_class(t))bind_member(e,*c,class_receiver,base.lvalue||e.arrow,e.arrow?false:base.constant);
        else throw Error(e.token.at,"member access requires a class or collection value", "BLT2000");
    }
}

} // namespace bolt::semantic
