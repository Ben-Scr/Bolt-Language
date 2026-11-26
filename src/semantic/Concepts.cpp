#include "Analyzer.hpp"
#include "Substitution.hpp"
#include <bolt/Types.hpp>
#include <algorithm>
#include <functional>

namespace bolt::semantic {

const Concept* Analyzer::find_concept(const std::string& n,Class* context,const Token& at) const {
    auto find=[&](const std::string& name)->const Concept*{for(const auto& c:program.concepts)if(c.name==name)return &c;return nullptr;};
    if(context)if(auto* c=find(fullname(context->namespace_name,n)))return c;
    if(auto* c=find(n))return c;
    const Concept* found=nullptr;
    if(context)for(const auto& imp:context->imports)if(auto* c=find(fullname(imp,n))) {
        if(found&&found!=c)throw Error(at.at,"ambiguous concept '"+n+"'","BLT2100");found=c;
    }
    return found;
}

Analyzer::IterationElement Analyzer::iteration_element(const std::string& source,Class* context,const Token& at,bool iterator,bool constant) {
    const auto type=referred(source);
    if(!iterator) {
        if(is_array(type))return {element_type(type),true,constant};
        if(type=="string")return {"char",false,true};
        if(is_collection(type))return {generic_args(type)[0],true,constant};
        if((type=="Ecs.Query"||type=="Ecs.ComponentQuery")&&program.find_class(type)->is_native)return {resolve(type=="Ecs.Query"?"Ecs.Row":"Ecs.Components",context,at),false,false};
    }
    // Check the same ordinary calls/member access used by range-for lowering.
    // This checks accessibility, overloads, reference results and const receivers
    // without executing user code or leaking optional-flow facts into the caller.
    auto* saved_class=current_class;auto* saved_method=current_method;const bool saved_static=static_context;
    auto saved_scopes=std::move(scopes);auto saved_flow=flow;auto saved_features=program.cpp_features;
    auto restore=[&]{current_class=saved_class;current_method=saved_method;static_context=saved_static;scopes=std::move(saved_scopes);flow=std::move(saved_flow);program.cpp_features=std::move(saved_features);};
    current_class=context;current_method=nullptr;static_context=true;scopes.clear();scopes.emplace_back();flow={};
    auto member=[&](const std::string& receiver,const std::string& name) {
        auto result=std::make_unique<Expr>(Expr::Kind::Member,at,name);
        result->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,at,receiver));return result;
    };
    auto call=[&](const std::string& receiver,const std::string& name) {
        auto result=std::make_unique<Expr>(Expr::Kind::Call,at);result->args.push_back(member(receiver,name));
        expression(*result);value(*result);return result;
    };
    try {
        std::string iterator_type=type;
        if(!iterator) {
            add_symbol("@iteration_source",type+"&",constant,at,true);
            auto enumerator=call("@iteration_source","GetEnumerator");iterator_type=enumerator->type;
        }
        add_symbol("@iteration_iterator",iterator_type+"&",iterator&&constant,at,true);
        auto next=call("@iteration_iterator","MoveNext");
        if(next->type!="bool"||(!next->declared_type.empty()&&next->declared_type!="bool"))
            throw Error(at.at,"iteration MoveNext() must return bool","BLT2021");
        auto current=member("@iteration_iterator","Current");expression(*current);value(*current);
        IterationElement result{current->type,current->lvalue,current->constant};
        restore();return result;
    }catch(...){restore();throw;}
}

bool Analyzer::satisfies_types(const std::string& concept_name,const std::vector<std::string>& types,Class* context,const Token& at) {
    if(const auto* found=find_concept(concept_name,context,at)) {
        const auto& params=found->parameters;
        if(params.size()!=types.size())throw Error(at.at,"concept '"+concept_name+"' expects "+std::to_string(params.size())+" type argument(s)","BLT2100");
        const auto key=generic_name(found->name,types);
        if(concept_active[key])throw Error(at.at,"recursive concept definition '"+key+"'","BLT2100");
        concept_active[key]=true;
        struct ActiveReset{bool& value;~ActiveReset(){value=false;}} reset{concept_active[key]};
        Substitution substitutions;for(std::size_t i=0;i<types.size();++i)substitutions[params[i]]=types[i];
        Class predicate_scope;predicate_scope.namespace_name=found->namespace_name;predicate_scope.imports=found->imports;
        std::function<bool(const Expr&)> evaluate=[&](const Expr& e)->bool {
            if(e.kind==Expr::Kind::Literal&&(e.text=="true"||e.text=="false"))return e.text=="true";
            if(e.kind==Expr::Kind::Unary&&e.text=="!")return !evaluate(*e.args[0]);
            if(e.kind==Expr::Kind::Binary&&e.text=="&&")return evaluate(*e.args[0])&&evaluate(*e.args[1]);
            if(e.kind==Expr::Kind::Binary&&e.text=="||")return evaluate(*e.args[0])||evaluate(*e.args[1]);
            if((e.kind==Expr::Kind::Name||e.kind==Expr::Kind::Member)&&!e.type_args.empty()) {
                std::vector<std::string> actual;
                for(const auto& t:e.type_args)actual.push_back(resolve(substitute(t,substitutions),&predicate_scope,e.token));
                return satisfies_types(qualified_expr(e),actual,&predicate_scope,e.token);
            }
            throw Error(e.token.at,"concept expressions use Concept<T...>, &&, ||, !, true, or false","BLT2100");
        };
        if(found->expression&&!evaluate(*found->expression))return false;
        auto* saved_class=current_class;auto* saved_method=current_method;const bool saved_static=static_context;
        auto saved_scopes=std::move(scopes);auto saved_flow=flow;flow={};auto saved_features=program.cpp_features;
        auto restore=[&]{current_class=saved_class;current_method=saved_method;static_context=saved_static;scopes=std::move(saved_scopes);flow=std::move(saved_flow);program.cpp_features=std::move(saved_features);};
        current_class=&predicate_scope;current_method=nullptr;static_context=true;
        try {
            for(const auto& requirement:found->requirements) {
                scopes.clear();scopes.emplace_back();
                for(const auto& parameter:requirement.parameters) {
                    auto t=resolve(substitute(parameter.type,substitutions),&predicate_scope,parameter.token);
                    add_symbol(parameter.name,t,parameter.constant,parameter.token,true);
                }
                auto expression_copy=clone(*requirement.expression);substitute(*expression_copy,substitutions);
                expression(*expression_copy);value(*expression_copy,true);
                if(!requirement.result_type.empty()) {
                    const auto expected=resolve(substitute(requirement.result_type,substitutions),&predicate_scope,requirement.token,true);
                    const auto actual=expression_copy->declared_type.empty()?expression_copy->type:expression_copy->declared_type;
                    if(is_reference(expected)?(!expression_copy->lvalue||referred(expected)!=expression_copy->type):(expected!=referred(actual))){restore();return false;}
                }
            }
        }catch(const Error&){restore();return false;}catch(...){restore();throw;}
        restore();return true;
    }
    if(concept_name=="Iterable"||concept_name=="Iterator") {
        if(types.empty()||types.size()>2)throw Error(at.at,"built-in concept '"+concept_name+"' expects a type and an optional element type","BLT2100");
        const auto key=generic_name(concept_name,types);
        if(concept_active[key])return false;
        concept_active[key]=true;struct ActiveReset{bool& value;~ActiveReset(){value=false;}} reset{concept_active[key]};
        std::unordered_map<const Class*,bool> prior_classes;
        std::unordered_map<const Method*,bool> prior_methods;
        for(const auto& c:program.classes) {
            prior_classes.emplace(&c,c.reachable);
            for(const auto& m:c.methods)prior_methods.emplace(&m,m.referenced);
        }
        const auto prior_references=program.semantic_references.size();
        auto discard_probe=[&] {
            for(auto& c:program.classes) {
                if(auto previous=prior_classes.find(&c);previous!=prior_classes.end())c.reachable=previous->second;
                else {c.reachable=false;deferred_iteration_classes.insert(&c);}
                for(auto& m:c.methods) {
                    if(auto previous=prior_methods.find(&m);previous!=prior_methods.end())m.referenced=previous->second;
                    else if(m.is_specialization){m.referenced=false;deferred_iteration_methods.insert(&m);}
                }
            }
            program.semantic_references.resize(prior_references);
        };
        try {
            auto element=iteration_element(types[0],context,at,concept_name=="Iterator");
            const bool matches=types.size()==1||(element.type==referred(types[1])&&(!is_reference(types[1])||(element.lvalue&&!element.constant)));
            if(!matches)discard_probe();return matches;
        }catch(const Error&){discard_probe();return false;}catch(...){discard_probe();throw;}
    }
    if(types.size()!=1)throw Error(at.at,"built-in concept '"+concept_name+"' expects one type argument; declare a structural concept for mixed types","BLT2100");
    const auto& t=types.front();const auto v=referred(t);
    if(auto name=resolve_name(concept_name,context);!name.empty()&&is_interface(program,name)) {
        require_type_access(name,context,at);
        if(name=="System.IStringable")return stringable(v);
        return derives(v,name);
    }
    if(concept_name=="Number")return is_numeric(v);
    if(concept_name=="Integer"||concept_name=="Integral")return is_integer(v);
    if(concept_name=="FloatingPoint")return is_floating(v);
    if(concept_name=="Signed")return is_numeric(v)&&!is_unsigned(v);
    if(concept_name=="Unsigned")return is_unsigned(v);
    if(concept_name=="Addable")return is_numeric(v)||v=="string"||has_operator(v,"+",2);
    if(concept_name=="Subtractable"||concept_name=="Multipliable"||concept_name=="Dividable")return is_numeric(v)||has_operator(v,concept_name=="Subtractable"?"-":concept_name=="Multipliable"?"*":"/",2);
    if(concept_name=="Moduloable")return is_integer(v)||has_operator(v,"%",2);
    if(concept_name=="Comparable")return is_numeric(v)||v=="string"||find_enum(v)||(has_operator(v,"<",2)&&has_operator(v,">",2));
    if(concept_name=="Equatable"||concept_name=="Hashable")return v!="void"&&v!="Arena"&&!v.empty();
    if(concept_name=="Pointer")return is_pointer(t);
    if(concept_name=="Reference")return is_reference(t);
    if(concept_name=="Enum")return find_enum(v)!=nullptr;
    if(concept_name=="Class"){const auto* c=find_class(v);return c&&!c->is_struct&&!c->is_interface;}
    if(concept_name=="Struct"){const auto* c=find_class(v);return c&&c->is_struct;}
    if(concept_name=="Constructible") {
        if(is_reference(t)||v=="Arena")return false;
        if(auto* c=find_class(v)) {
            prepare(*c);
            if(c->is_static||c->is_interface||c->is_abstract)return false;
            if(!c->base.empty()&&!satisfies("Constructible",c->base,context,at))return false;
            for(const auto& f:c->fields)if(!f.is_static&&!f.initializer&&!satisfies("Constructible",f.type,context,at))return false;
            bool any=false;
            for(const auto& m:c->methods)if(m.is_constructor) {
                any=true;
                if(m.is_public&&std::all_of(m.parameters.begin(),m.parameters.end(),[](const Parameter& p){return p.variadic||bool(p.default_value);}))return true;
            }
            return !any;
        }
        return true;
    }

    throw Error(at.at,"unknown concept '"+concept_name+"'","BLT2100");
}

bool Analyzer::satisfies(const std::string& concept_name,const std::string& t,Class* context,const Token& at) {
    auto args=generic_args(concept_name);
    if(args.empty())args.push_back(t);
    return satisfies_types(generic_base(concept_name),args,context,at);
}

void Analyzer::constraints(const Method& m,const Substitution& map,Class& owner,const Token& at) {
    for(const auto& constraint:m.constraints)for(const auto& name:constraint.concepts) {
        auto arguments=generic_args(name);
        if(!arguments.empty()) {
            for(auto& t:arguments)t=resolve(substitute(t,map),&owner,constraint.token);
            if(!satisfies_types(generic_base(name),arguments,&owner,at))
                throw Error(at.at,"type arguments do not satisfy '"+generic_name(generic_base(name),arguments)+"' required by '"+m.name+"'","BLT2101");
            continue;
        }
        // Backward compatible shorthand is unambiguous only with one T.
        std::vector<std::string> types;
        if(!constraint.parameter.empty()) {
            auto i=map.find(constraint.parameter);if(i==map.end())throw Error(constraint.token.at,"unknown constrained type parameter '"+constraint.parameter+"'","BLT2100");types.push_back(i->second);
        }else {
            for(const auto& n:m.generic_parameters)types.push_back(map.at(n));
            if(!m.pack_parameter.empty())types.push_back(map.at(m.pack_parameter));
        }
        if(types.size()!=1)throw Error(constraint.token.at,"specify the constrained types explicitly, for example Addable<T1>, Comparable<T2>","BLT2100");
        if(!satisfies_types(name,types,&owner,at))throw Error(at.at,"type '"+types[0]+"' does not satisfy concept '"+name+"' for '"+m.name+"'","BLT2101");
    }
}

} // namespace bolt::semantic
