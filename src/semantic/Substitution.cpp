#include "Substitution.hpp"
#include <bolt/Types.hpp>
#include <cctype>
#include <optional>

namespace bolt::semantic {

bool mentions_type(const std::string& pattern,const std::string& name) {
    for(std::size_t start=0;(start=pattern.find(name,start))!=std::string::npos;++start) {
        auto identifier=[](unsigned char c){return std::isalnum(c)||c=='_';};
        if((start==0||!identifier(pattern[start-1]))&&(start+name.size()==pattern.size()||!identifier(pattern[start+name.size()])))return true;
    }
    return false;
}
static std::vector<std::string> substitute_types(const std::vector<std::string>& types,const Substitution& map) {
    std::vector<std::string> result;
    for(const auto& type:types) {
        bool expanded=false;
        if(type.ends_with("...")) {
            const auto pattern=type.substr(0,type.size()-3);
            for(const auto& [name,pack]:map.type_packs)if(mentions_type(pattern,name)) {
                for(const auto& element:pack){auto local=map;local[name]=element;result.push_back(substitute(pattern,local));}
                expanded=true;break;
            }
        }
        if(!expanded)result.push_back(substitute(type,map));
    }
    return result;
}
std::string substitute(const std::string& t,const Substitution& map) {
    if(auto it=map.find(t);it!=map.end())return it->second;
    if(is_array(t))return substitute(element_type(t),map)+array_suffix(array_rank(t));
    if(is_pointer(t)||is_reference(t)||is_optional(t))return substitute(element_type(t),map)+t.back();
    int nesting=0;std::size_t dot=std::string::npos;
    for(std::size_t i=0;i<t.size();++i) {
        if(t[i]=='<')++nesting;else if(t[i]=='>')--nesting;
        else if(t[i]=='.'&&nesting==0)dot=i;
    }
    if(dot!=std::string::npos)
        return substitute(t.substr(0,dot),map)+"."+substitute(t.substr(dot+1),map);
    auto a=generic_args(t);
    if(!a.empty()) {
        a=substitute_types(a,map);
        return a.empty()?generic_base(t)+"<>":generic_name(generic_base(t),a);
    }
    return t;
}
bool mentions_pack(const Expr& e,const std::string& name) {
    if((e.kind==Expr::Kind::Name||e.kind==Expr::Kind::New||e.kind==Expr::Kind::SizeOf)&&mentions_type(e.text,name))return true;
    for(const auto& t:e.type_args)if(mentions_type(t,name))return true;
    for(const auto& arg:e.args)if(mentions_pack(*arg,name))return true;
    return e.arena&&mentions_pack(*e.arena,name);
}
bool mentions_pack(const Stmt& s,const std::string& name) {
    if(mentions_type(s.type,name))return true;
    if(s.expression&&mentions_pack(*s.expression,name))return true;
    if(s.condition&&mentions_pack(*s.condition,name))return true;
    if(s.increment&&mentions_pack(*s.increment,name))return true;
    if(s.init&&mentions_pack(*s.init,name))return true;
    if(s.body&&mentions_pack(*s.body,name))return true;
    if(s.alternative&&mentions_pack(*s.alternative,name))return true;
    for(const auto& child:s.statements)if(mentions_pack(*child,name))return true;
    return false;
}
static bool expands_pack(const Expr& e,const std::string& name) {
    if(e.kind==Expr::Kind::Unary&&e.text=="..."&&mentions_pack(e,name))return true;
    if(e.text.find("...")!=std::string::npos&&mentions_type(e.text,name))return true;
    for(const auto& type:e.type_args)if(type.find("...")!=std::string::npos&&mentions_type(type,name))return true;
    for(const auto& arg:e.args)if(expands_pack(*arg,name))return true;
    return e.arena&&expands_pack(*e.arena,name);
}
bool expands_pack(const Stmt& s,const std::string& name) {
    if(s.type.find("...")!=std::string::npos&&mentions_type(s.type,name))return true;
    if(s.expression&&expands_pack(*s.expression,name))return true;
    if(s.condition&&expands_pack(*s.condition,name))return true;
    if(s.increment&&expands_pack(*s.increment,name))return true;
    if(s.init&&expands_pack(*s.init,name))return true;
    if(s.body&&expands_pack(*s.body,name))return true;
    if(s.alternative&&expands_pack(*s.alternative,name))return true;
    for(const auto& child:s.statements)if(expands_pack(*child,name))return true;
    return false;
}
static void substitute(Stmt& s,const Substitution& map);
void substitute(Expr& e,const Substitution& map) {
    if(e.lambda) {
        for(auto& p:e.lambda->parameters)p.type=substitute(p.type,map);
        substitute(*e.lambda->body,map);
    }
    if(e.kind==Expr::Kind::Name||e.kind==Expr::Kind::New||e.kind==Expr::Kind::SizeOf)e.text=substitute(e.text,map);
    e.type_args=substitute_types(e.type_args,map);
    if(e.arena)substitute(*e.arena,map);
    std::vector<ExprPtr> arguments;
    for(auto& a:e.args) {
        const Expr* expanded=a.get();
        const bool expansion=a->kind==Expr::Kind::Unary&&a->text=="...";
        if(expansion)expanded=a->args.front().get();
        const bool argument_list=e.kind==Expr::Kind::Call||e.kind==Expr::Kind::New||e.kind==Expr::Kind::Array;
        if(expansion&&argument_list) {
            std::optional<std::size_t> count;
            auto size=[&](const std::string& name,std::size_t n) {if(mentions_pack(*expanded,name)){if(count&&*count!=n)throw Error(a->token.at,"expanded packs must have the same length","BLT2000");count=n;}};
            for(const auto& [name,pack]:map.type_packs)size(name,pack.size());
            for(const auto& [name,pack]:map.value_packs)size(name,pack.size());
            if(count) {
                for(std::size_t i=0;i<*count;++i) {
                    auto local=map;
                    for(const auto& [name,pack]:map.type_packs)if(mentions_pack(*expanded,name)){local[name]=pack[i];local.type_packs.erase(name);}
                    for(const auto& [name,pack]:map.value_packs)if(mentions_pack(*expanded,name)){local[name]=pack[i];local.value_packs.erase(name);}
                    auto item=clone(*expanded);substitute(*item,local);arguments.push_back(std::move(item));
                }
                continue;
            }
        }
        auto pack=argument_list&&expanded->kind==Expr::Kind::Name?map.value_packs.find(expanded->text):map.value_packs.end();
        if(pack!=map.value_packs.end()) {
            for(const auto& name:pack->second)arguments.push_back(std::make_unique<Expr>(Expr::Kind::Name,expanded->token,name));
        } else {substitute(*a,map);arguments.push_back(std::move(a));}
    }
    e.args=std::move(arguments);
}
static void substitute(Stmt& s,const Substitution& map) {
    s.type=substitute(s.type,map);
    if(s.expression)substitute(*s.expression,map);
    if(s.condition)substitute(*s.condition,map);
    if(s.increment)substitute(*s.increment,map);
    if(s.init)substitute(*s.init,map);
    if(s.body)substitute(*s.body,map);
    if(s.alternative)substitute(*s.alternative,map);
    for(auto& c:s.statements)substitute(*c,map);
}
void substitute(Method& m,Substitution map) {
    m.type=substitute(m.type,map);
    if(m.is_conversion)m.name="@convert."+m.type;
    std::vector<Parameter> parameters;
    for(auto& p:m.parameters) {
        bool expanded=false;
        if(p.pack)for(const auto& [name,pack]:map.type_packs)if(mentions_type(p.type,name)) {
            auto& values=map.value_packs[p.name];values.clear();
            for(std::size_t i=0;i<pack.size();++i) {
                auto local=map;local[name]=pack[i];auto element=p;
                element.type=substitute(p.type,local);element.name=p.name+"@"+std::to_string(i);element.pack=false;
                values.push_back(element.name);parameters.push_back(std::move(element));
            }
            expanded=true;break;
        }
        if(!expanded){p.type=substitute(p.type,map);if(p.default_value)substitute(*p.default_value,map);parameters.push_back(std::move(p));}
    }
    m.parameters=std::move(parameters);
    for(auto& t:m.specialization_types)t=substitute(t,map);
    for(auto& c:m.constraints)for(auto& n:c.concepts)n=substitute(n,map);
    for(auto& e:m.base_arguments)substitute(*e,map);
    for(auto& e:m.constructor_initializers)substitute(*e,map);
    for(auto& [name,e]:m.field_initializers)substitute(*e,map);
    if(m.body)substitute(*m.body,map);
}

// Parameters declared by a nested class or method shadow the enclosing bindings.
Substitution Substitution::without_parameters(const std::vector<std::string>& parameters,
                                             const std::string& pack) const {
    auto map = *this;
    for (const auto& name : parameters) {
        map.erase(name);
        map.type_packs.erase(name);
    }
    map.erase(pack);
    map.type_packs.erase(pack);
    return map;
}

void substitute(Class& instance, const Substitution& map) {
    instance.base = substitute(instance.base, map);
    for (auto& base : instance.interfaces) base = substitute(base, map);
    for (auto& field : instance.fields) {
        field.type = substitute(field.type, map);
        if (field.initializer) substitute(*field.initializer, map);
    }
    for (auto& property : instance.properties) {
        property.type = substitute(property.type, map);
        for (auto& parameter : property.parameters) parameter.type = substitute(parameter.type, map);
    }
    for (auto& method : instance.methods) {
        substitute(method, map.without_parameters(method.generic_parameters, method.pack_parameter));
    }
}

} // namespace bolt::semantic
