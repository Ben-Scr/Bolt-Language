#pragma once
#include <bolt/Ast.hpp>
#include <bolt/Types.hpp>
#include <functional>
#include <set>
#include <map>
#include <algorithm>
// Conservative receiver-effect inference for ordinary getters.  A missing setter
// controls assignment to a property; it does not prohibit side effects in a getter.
// Infer const receiver access only for getters proven not to mutate their receiver.
namespace bolt {
inline void infer_getter_receivers(Program& program) {
    struct Info { std::string type; bool own=false, type_name=false; };
    auto find_type=[&](std::string type,const Class& scope)->const Class*{
        while(is_reference(type)||is_pointer(type)||is_array(type)||is_optional(type))type=element_type(type);
        auto base=generic_base(type);
        if(auto c=program.find_class(type))return c;
        if(auto c=program.find_class(base))return c;
        if(auto c=program.find_class(scope.namespace_name.empty()?base:scope.namespace_name+"."+base))return c;
        const Class* found=nullptr;
        for(const auto& c:program.classes)if(c.short_name==base&&(c.namespace_name.empty()||c.namespace_name==scope.namespace_name||std::find(scope.imports.begin(),scope.imports.end(),c.namespace_name)!=scope.imports.end())){if(found&&found!=&c)return nullptr;found=&c;}
        return found;
    };
    std::vector<std::pair<Class*,Method*>> candidates;
    for(auto& c:program.classes)for(auto& m:c.methods)
        if(m.is_getter&&m.body&&!m.is_static&&!m.readonly_receiver&&!is_reference(m.type)&&!c.is_interface&&!m.is_abstract){m.readonly_receiver=true;candidates.emplace_back(&c,&m);}
    bool changed=true;
    while(changed){changed=false;
        for(auto [scope,method]:candidates){if(!method->readonly_receiver)continue;
            bool safe=true;std::map<std::string,std::string> locals;
            for(const auto& p:method->parameters)locals[p.name]=p.type;
            std::function<void(const Stmt&)> collect=[&](const Stmt& s){if(s.kind==Stmt::Kind::Variable||s.kind==Stmt::Kind::ForEach)locals[s.name]=s.type;if(s.init)collect(*s.init);if(s.body)collect(*s.body);if(s.alternative)collect(*s.alternative);for(const auto& x:s.statements)collect(*x);};collect(*method->body);
            std::function<Info(const Expr&)> visit;
            std::function<Info(const Class*,const std::string&,bool,bool)> member;
            member=[&](const Class* c,const std::string& name,bool own,bool stat)->Info{
                if(!c){if(own)safe=false;return {};}
                if(const auto* f=find_field(*c,name))return {f->type,own&&!f->is_static,false};
                if(const auto* p=find_property(*c,name)){
                    const Method* g=nullptr;for(const auto& m:c->methods)if(m.name==p->getter)g=&m;
                    if(own&&(!g||!g->readonly_receiver&&!g->is_static))safe=false;
                    return {p->type,own&&is_reference(p->type),false};
                }
                if(!c->base.empty())return member(find_type(c->base,*c),name,own,stat);
                if(own)safe=false;return {};
            };
            visit=[&](const Expr& e)->Info{
                using K=Expr::Kind;
                if(e.arena)visit(*e.arena);
                if(e.kind==K::Name){
                    if(e.text=="this")return {scope->name,true,false};
                    if(auto l=locals.find(e.text);l!=locals.end())return {l->second,false,false};
                    if(auto c=find_type(e.text,*scope))return {c->name,false,true};
                    return member(scope,e.text,true,false);
                }
                if(e.kind==K::OptionalHas){visit(*e.args[0]);return {"bool",false,false};}
                if(e.kind==K::OptionalValue){auto info=visit(*e.args[0]);return {element_type(info.type),info.own,false};}
                if(e.kind==K::Literal)return {};
                if(e.kind==K::Member){
                    // Qualified type names are resolved as declarations, not text guesses.
                    std::function<std::string(const Expr&)> path=[&](const Expr& x){if(x.kind==K::Name)return x.text;if(x.kind==K::Member&&!x.args.empty()){auto p=path(*x.args[0]);return p.empty()?p:p+"."+x.text;}return std::string{};};
                    if(auto c=find_type(path(e),*scope))return {c->name,false,true};
                    auto receiver=visit(*e.args[0]);
                    if(is_optional(referred(receiver.type))) {
                        if(e.text=="HasValue")return {"bool",false,false};
                        if(e.text=="Value")return {element_type(referred(receiver.type)),receiver.own,false};
                    }
                    if((is_array(receiver.type)||receiver.type=="string")&&(e.text=="Length"||e.text=="ByteLength"))return {"int",false,false};
                    return member(find_type(receiver.type,*scope),e.text,receiver.own,receiver.type_name);
                }
                if(e.kind==K::Call&&!e.args.empty()){
                    const auto& target=*e.args[0];const Class* owner=nullptr;bool own=false;std::string name;
                    if(target.kind==K::Name){name=target.text;
                        if(auto c=find_type(name,*scope)){for(std::size_t i=1;i<e.args.size();++i){auto a=visit(*e.args[i]);if(a.own&&is_reference(a.type))safe=false;}return {c->name,false,false};}
                        owner=scope;own=true;
                    }else if(target.kind==K::Member){auto receiver=visit(*target.args[0]);owner=find_type(receiver.type,*scope);own=receiver.own;name=target.text;}
                    std::vector<Info> args;for(std::size_t i=1;i<e.args.size();++i)args.push_back(visit(*e.args[i]));
                    if(!owner){if(own)safe=false;return {};}
                    bool found=false;std::string result;
                    for(const auto& m:owner->methods)if(m.name==name&&(m.parameters.size()==args.size()||!m.parameters.empty()&&m.parameters.back().variadic)){
                        found=true;result=m.type;if(own&&!m.is_static&&!m.readonly_receiver)safe=false;
                        for(std::size_t i=0;i<std::min(args.size(),m.parameters.size());++i)if(args[i].own&&is_reference(m.parameters[i].type)&&!m.parameters[i].constant)safe=false;
                    }
                    if(!found&&own)safe=false;
                    return {result,own&&is_reference(result),false};
                }
                if(e.kind==K::Index){auto receiver=visit(*e.args[0]);for(std::size_t i=1;i<e.args.size();++i)visit(*e.args[i]);return {is_array(receiver.type)?element_type(receiver.type):std::string{},receiver.own,false};}
                if(e.kind==K::Assign||e.kind==K::Postfix||(e.kind==K::Unary&&(e.text=="++"||e.text=="--"))){if(!e.args.empty()&&visit(*e.args[0]).own)safe=false;for(std::size_t i=1;i<e.args.size();++i)visit(*e.args[i]);return {};}
                if(e.kind==K::Delete){safe=false;return {};}
                for(const auto& a:e.args)visit(*a);
                return {};
            };
            std::function<void(const Stmt&)> check=[&](const Stmt& s){if(s.expression)visit(*s.expression);if(s.condition)visit(*s.condition);if(s.increment)visit(*s.increment);if(s.init)check(*s.init);if(s.body)check(*s.body);if(s.alternative)check(*s.alternative);for(const auto& x:s.statements)check(*x);};check(*method->body);
            if(!safe){method->readonly_receiver=false;changed=true;}
        }
    }
}
}
