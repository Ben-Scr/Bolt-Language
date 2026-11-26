#include "Analyzer.hpp"
#include "Substitution.hpp"
#include <bolt/TypeAccess.hpp>
#include <bolt/Types.hpp>
#include <algorithm>

namespace bolt::semantic {

bool Analyzer::infer(const std::string& pattern,const std::string& actual,const std::vector<std::string>& parameters,Substitution& map) {
    // A default argument uses a selected parameter's type, but cannot
    // determine a generic type parameter on its own.
    if(actual=="#default" || actual=="#lambda" || actual=="#method")return true;
    const auto p=referred(pattern);
    if(std::find(parameters.begin(),parameters.end(),p)!=parameters.end()) {
        auto it=map.find(p);
        if(it==map.end()) {
            map[p]=actual;
            return true;
        }
        return it->second==actual;
    }
    if(is_optional(p)) {
        if(actual=="null")return true;
        return infer(element_type(p),is_optional(actual)?element_type(actual):actual,parameters,map);
    }
    if(is_pointer(p)&&is_pointer(actual))return infer(element_type(p),element_type(actual),parameters,map);
    if(is_array(p)&&is_array(actual))return array_rank(p)==array_rank(actual)&&infer(element_type(p),element_type(actual),parameters,map);
    auto pa=generic_args(p),aa=generic_args(actual);
    if(!pa.empty()&&pa.back().ends_with("...")&&aa.size()+1>=pa.size()) {
        auto pattern=pa.back().substr(0,pa.back().size()-3);
        for(const auto& name:parameters)if(mentions_type(pattern,name)) {
            for(std::size_t i=0;i+1<pa.size();++i)if(!infer(pa[i],aa[i],parameters,map))return false;
            std::vector<std::string> pack;
            for(std::size_t i=pa.size()-1;i<aa.size();++i) {
                Substitution element;if(!infer(pattern,aa[i],{name},element)||!element.contains(name))return false;
                pack.push_back(element.at(name));
            }
            if(auto prior=map.type_packs.find(name);prior!=map.type_packs.end()&&prior->second!=pack)return false;
            map.type_packs[name]=std::move(pack);return true;
        }
    }
    if(!pa.empty()&&pa.size()==aa.size()) {
        for(std::size_t i=0;i<pa.size();++i)if(!infer(pa[i],aa[i],parameters,map))return false;
    }
    return true;
}

Method& Analyzer::instantiate(Class& owner,Method& original,const Substitution& map) {
    std::vector<std::string> args;
    for(const auto& p:original.generic_parameters)args.push_back(map.at(p));
    if(!original.pack_parameter.empty()) {
        if(auto pack=map.type_packs.find(original.pack_parameter);pack!=map.type_packs.end())args.insert(args.end(),pack->second.begin(),pack->second.end());
        else args.push_back(map.at(original.pack_parameter));
    }
    const auto key=signature(original)+(args.empty()?"<>":generic_name("",args));
    for(auto& m:owner.methods)if(m.is_specialization&&m.id==key)return m;
    if(++instantiations>1024)throw Error(original.token.at,"generic instantiation limit exceeded (1024)", "BLT2000");
    auto m=clone(original);
    substitute(m,map);
    m.id=key;
    m.generic_parameters.clear();
    m.pack_parameter.clear();
    m.constraints.clear();
    m.is_specialization=true;
    m.is_virtual=false;
    m.is_override=false;
    m.specialization_types=args;
    owner.methods.push_back(std::move(m));
    auto& result=owner.methods.back();
    prepare_method(owner,result);
    return result;
}

Method& Analyzer::instantiate_native_pack(Class& owner,Method& original,const Substitution& map,const std::vector<std::string>& types) {
    const auto key=signature(original)+"#nativepack"+generic_name("",types);
    for(auto& m:owner.methods)if(m.is_specialization&&m.id==key)return m;
    if(++instantiations>1024)throw Error(original.token.at,"generic instantiation limit exceeded (1024)","BLT2000");
    auto m=clone(original);substitute(m,map);m.id=key;m.generic_parameters.clear();m.pack_parameter.clear();m.constraints.clear();m.is_specialization=true;m.specialization_types=types;m.is_virtual=false;m.is_override=false;
    owner.methods.push_back(std::move(m));auto& result=owner.methods.back();prepare_method(owner,result);return result;
}

std::size_t Analyzer::required_arguments(const Method& m) {
    std::size_t count=0;for(const auto& p:m.parameters)if(!p.default_value&&!p.variadic&&!p.pack)++count;return count;
}

bool Analyzer::variadic(const Method& m) {return !m.parameters.empty()&&m.parameters.back().variadic;}

bool Analyzer::value_pack(const Method& m) {return !m.parameters.empty()&&m.parameters.back().pack;}

bool Analyzer::heterogeneous_pack(const Method& m) {
    if(value_pack(m))return true;
    if(m.pack_parameter.empty()||m.native_pack)return false;
    if(m.type.find("...")!=std::string::npos&&mentions_type(m.type,m.pack_parameter))return true;
    for(const auto& p:m.parameters)if(p.type.find("...")!=std::string::npos&&mentions_type(p.type,m.pack_parameter))return true;
    if(m.body&&expands_pack(*m.body,m.pack_parameter))return true;
    // Retain scalar-type fan-out only when the pack is actually used
    // as one type. Unused packs still instantiate one ordinary call.
    if(mentions_type(m.type,m.pack_parameter))return false;
    for(const auto& p:m.parameters)if(mentions_type(p.type,m.pack_parameter)||(p.default_value&&mentions_pack(*p.default_value,m.pack_parameter)))return false;
    return !m.body||!mentions_pack(*m.body,m.pack_parameter);
}

std::pair<Class*,Method*> Analyzer::select(std::vector<std::pair<Class*,Method*>> candidates,const std::vector<std::string>& explicit_types,std::vector<Expr*>& arguments,const Token& at,bool allow_pack,bool allow_no_match) {
    auto infer_argument = [&](const std::string& pattern, const Expr& argument,
                              const std::vector<std::string>& parameters, Substitution& map, Class& owner) {
        if (!argument.lambda) return infer(pattern,argument.type,parameters,map);
        const auto contract=resolve_name(generic_base(pattern),&owner);
        if (contract!="System.Action" && contract!="System.Func") return true;
        auto inputs=generic_args(pattern);
        if (contract=="System.Func" && !inputs.empty()) inputs.pop_back();
        if (inputs.size()!=argument.lambda->parameters.size()) return false;
        for (std::size_t i=0;i<inputs.size();++i) {
            const auto& param=argument.lambda->parameters[i];
            if (!param.type.empty() && !infer(inputs[i],resolve(param.type,current_class,param.name),parameters,map))
                return false;
        }
        return true;
    };
    struct Match {Class* c;Method* m;Substitution map;int score;};
    std::vector<Match> viable;std::string detail;
    for(auto [owner,m]:candidates) {
        if(heterogeneous_pack(*m)) {
            const bool values=value_pack(*m);
            const auto fixed=m->parameters.size()-(values?1:0);
            if(arguments.size()<required_arguments(*m))continue;
            if(!values&&arguments.size()>fixed)continue;
            const auto supplied_fixed=std::min(fixed,arguments.size());
            const auto pack_size=arguments.size()-supplied_fixed;
            Substitution map;
            for(std::size_t i=0;i<std::min(explicit_types.size(),m->generic_parameters.size());++i)map[m->generic_parameters[i]]=explicit_types[i];
            bool valid=true;
            std::vector<std::string> inferred_parameters;
            for(const auto& name:m->generic_parameters)if(!map.contains(name))inferred_parameters.push_back(name);
            inferred_parameters.push_back(m->pack_parameter);
            for(std::size_t i=0;i<supplied_fixed;++i)valid&=infer_argument(m->parameters[i].type,*arguments[i],inferred_parameters,map,*owner);
            for(const auto& name:m->generic_parameters)valid&=map.contains(name);
            auto& pack=map.type_packs[m->pack_parameter];
            if(explicit_types.size()>m->generic_parameters.size()) {
                std::vector<std::string> explicit_pack(explicit_types.begin()+static_cast<std::ptrdiff_t>(m->generic_parameters.size()),explicit_types.end());
                if(!pack.empty()&&pack!=explicit_pack)valid=false;
                pack=std::move(explicit_pack);
                if(values)valid&=pack.size()==pack_size;
            } else if(values&&!pack.empty())valid&=pack.size()==pack_size;
            else if(values) {
                std::vector<std::string> deduced;
                for(std::size_t i=fixed;i<arguments.size();++i) {
                    Substitution element;
                    valid&=infer(m->parameters.back().type,arguments[i]->type,{m->pack_parameter},element);
                    if(!element.contains(m->pack_parameter)){valid=false;break;}
                    deduced.push_back(element.at(m->pack_parameter));
                }
                if(!pack.empty()&&pack!=deduced)valid=false;
                pack=std::move(deduced);
            }
            if(!valid)continue;
            try {
                for(const auto& constraint:m->constraints) {
                    Method checked;checked.name=m->name;checked.generic_parameters=m->generic_parameters;
                    checked.constraints.push_back(constraint);
                    bool uses_pack=constraint.parameter==m->pack_parameter;
                    for(const auto& name:constraint.concepts)uses_pack|=mentions_type(name,m->pack_parameter)||(constraint.parameter.empty()&&generic_args(name).empty());
                    if(uses_pack)for(const auto& type:pack){checked.pack_parameter=m->pack_parameter;auto element=map;element[m->pack_parameter]=type;constraints(checked,element,*owner,at);}
                    else constraints(checked,map,*owner,at);
                }
            }catch(const Error& error){detail=error.diagnostic.message;continue;}
            auto expanded=clone(*m);substitute(expanded,map);
            std::vector<std::string> types(pack);
            for(const auto& binding:map)types.push_back(binding.second);
            SignatureArguments bindings(*this,types);
            int score=2+static_cast<int>(fixed-supplied_fixed);
            for(std::size_t i=0;i<arguments.size();++i) {
                const auto& parameter=expanded.parameters[i];
                auto type=resolve(parameter.type,owner,parameter.token);
                if(!can_convert(type,*arguments[i],parameter.constant)){valid=false;break;}
                if(!standard_convert(type,*arguments[i],parameter.constant))score+=100;
                else if(referred(type)!=arguments[i]->type)score+=2;
            }
            if(valid)viable.push_back({owner,m,std::move(map),score});
            continue;
        }
        if(arguments.size()<required_arguments(*m)||(!variadic(*m)&&arguments.size()>m->parameters.size()))continue;
        if(!m->pack_parameter.empty()&&!allow_pack&&!m->native_pack)continue;
        Substitution map;const bool generic=!m->generic_parameters.empty()||!m->pack_parameter.empty();
        if(!generic&&!explicit_types.empty())continue;
        if(generic) {
            if(m->native_pack) {
                if(!m->is_extern||explicit_types.size()<m->generic_parameters.size()+m->native_pack_min)continue;
                for(std::size_t i=0;i<m->generic_parameters.size();++i)map[m->generic_parameters[i]]=explicit_types[i];
                bool valid=true;for(std::size_t i=m->generic_parameters.size();i<explicit_types.size();++i){if(explicit_types[i]=="void"||is_reference(explicit_types[i]))valid=false;}
                if(!valid)continue;
            } else if(!explicit_types.empty()) {
                if(explicit_types.size()!=m->generic_parameters.size()+(m->pack_parameter.empty()?0:1))continue;
                for(std::size_t i=0;i<m->generic_parameters.size();++i)map[m->generic_parameters[i]]=explicit_types[i];
                if(!m->pack_parameter.empty())map[m->pack_parameter]=explicit_types.back();
            } else {
                bool ok=true;
                for(std::size_t i=0;i<arguments.size();++i) {
                    const auto& parameter=m->parameters[std::min(i,m->parameters.size()-1)];
                    auto pattern=parameter.type;
                    if(parameter.variadic&&!(arguments.size()==m->parameters.size()&&is_array(arguments[i]->type)))pattern=element_type(pattern);
                    ok&=infer_argument(pattern,*arguments[i],m->generic_parameters,map,*owner);
                }
                if(!ok||map.size()!=m->generic_parameters.size())continue;
            }
            try{
                if(m->native_pack){for(std::size_t i=m->generic_parameters.size();i<explicit_types.size();++i){auto current=map;current[m->pack_parameter]=explicit_types[i];constraints(*m,current,*owner,at);}}
                else constraints(*m,map,*owner,at);
            }catch(const Error& e){detail=e.diagnostic.message;continue;}
        }
        if(!generic&&!m->constraints.empty()) {
            try{constraints(*m,{},*owner,at);}catch(const Error& e){detail=e.diagnostic.message;continue;}
        }
        int score=(generic?1:0)+(variadic(*m)?4:0);
        if(arguments.size()<m->parameters.size())score+=static_cast<int>(m->parameters.size()-arguments.size());
        bool ok=true;
        std::vector<std::string> types;
        for(const auto& binding:map)types.push_back(binding.second);
        SignatureArguments bindings(*this,types);
        for(std::size_t i=0;i<arguments.size();++i) {
            const auto& parameter=m->parameters[std::min(i,m->parameters.size()-1)];
            auto t=generic?resolve(substitute(parameter.type,map),owner,parameter.token):parameter.type;
            const bool array_form=parameter.variadic&&arguments.size()==m->parameters.size()&&arguments[i]->type==t;
            if(parameter.variadic&&!array_form)t=element_type(t);
            if(!can_convert(t,*arguments[i],parameter.constant)){ok=false;break;}
            if(!standard_convert(t,*arguments[i],parameter.constant))score+=100;
            else if(referred(t)!=arguments[i]->type)score+=2;
        }
        if(ok)viable.push_back({owner,m,std::move(map),score});
    }
    if(viable.empty()) {
        if(allow_no_match)return {nullptr,nullptr};
        if(!detail.empty())throw Error(at.at,detail,"BLT2101");
        throw Error(at.at,"no matching overload: check argument count/types, required parameters, and variadic element types","BLT2006");
    }
    std::sort(viable.begin(),viable.end(),[](const auto& a,const auto& b){return a.score<b.score;});
    if(viable.size()>1&&viable[0].score==viable[1].score) {
        for(const auto& candidate:viable)if(candidate.score==viable.front().score) {
            const auto& method=*candidate.m;const auto& owner=*candidate.c;
            const auto id=method.id.empty()?"template:"+owner.name+":"+method.name+":"+std::to_string(method.token.at.offset):"method:"+owner.name+":"+method.id;
            program.semantic_references.push_back({at,id,method.name,"method","",owner.name,method.token.at,true});
        }
        throw Error(at.at,"ambiguous method overload","BLT2006");
    }
    auto best=std::move(viable.front());if(best.m->native_pack)best.m=&instantiate_native_pack(*best.c,*best.m,best.map,explicit_types);else if(!best.map.empty()||!best.map.type_packs.empty())best.m=&instantiate(*best.c,*best.m,best.map);
    best.m->referenced=true;mark_type(best.c->name);mark_type(best.m->type);for(const auto& p:best.m->parameters)mark_type(p.type);
    return {best.c,best.m};
}

std::vector<std::pair<Class*,Method*>> Analyzer::extension_methods(const std::string& name,const Token& at) {
    std::vector<std::pair<Class*,Method*>> result;
    std::vector<Class*> owners;
    const auto* context=member_context();
    for(auto& owner:program.classes) {
        if(owner.is_template_member||!owner.generic_parameters.empty()||!owner.pack_parameter.empty())continue;
        const bool visible=(context&&owner.namespace_name==context->namespace_name)||
            imported(current_class,owner.name)||(!owner.namespace_name.empty()&&imported(current_class,owner.namespace_name))||
            std::find(program.implicit_static_imports.begin(),program.implicit_static_imports.end(),owner.name)!=program.implicit_static_imports.end();
        if(!visible)continue;
        if(!owner.is_global&&!type_access_error(program,owner.name,at.at.file,context?context->name:std::string{}).empty())continue;
        bool found=false;
        for(const auto& method:owner.methods)if(method.name==name&&method.is_extension&&!method.is_specialization&&
            (method.is_public||(current_class&&current_class->name==owner.name))){found=true;break;}
        if(found)owners.push_back(&owner);
    }
    // Preparing parameter types may append generic specializations to classes.
    for(auto* owner:owners) {
        prepare(*owner);
        for(auto& method:owner->methods)if(method.name==name&&method.is_extension&&!method.is_specialization&&
            (method.is_public||(current_class&&current_class->name==owner->name)))result.emplace_back(owner,&method);
    }
    return result;
}

bool Analyzer::bind_extension_member(Expr& target) {
    if(extension_methods(target.text,target.token).empty())return false;
    target.binding=Expr::Binding::None;target.type="#extension";target.is_extension=true;
    return true;
}

bool Analyzer::builtin_instance_member(const std::string& type,const std::string& name) const {
    if((is_primitive(type)||find_enum(type)||is_collection(type))&&name=="ToString")return true;
    if(type=="string") {
        static const std::unordered_set<std::string> names={"Length","Count","ByteLength","Contains","StartsWith","EndsWith","Equals","IndexOf","LastIndexOf","Substring","Remove","Insert","Replace","Trim","TrimStart","TrimEnd","ToUpper","ToLower","PadLeft","PadRight","Split","ToCharArray","ToUtf8","CompareTo"};
        return names.contains(name);
    }
    if(is_array(type))return name=="Length"||name=="Count"||name=="Rank"||name=="GetLength";
    if(is_collection(type)) {
        static const std::unordered_set<std::string> names={"Length","Count","Capacity","Add","Clear","Contains","Remove","Insert","RemoveAt","Sort","Reverse","IndexOf","ToArray","Enqueue","Dequeue","Peek","Push","Pop","ContainsKey","TryGetValue"};
        return names.contains(name);
    }
    if(type=="Arena")return true;
    return false;
}

bool Analyzer::lower_extension_call(Expr& call,const std::vector<std::string>& types,bool allow_no_match) {
    using K=Expr::Kind;using B=Expr::Binding;
    auto& target=*call.args[0];auto& base=*target.args[0];
    // A pointer member call applies the extension to the pointed-to value.
    Expr dereference(K::Unary,target.token,"*");
    Expr* receiver=&base;
    if(target.arrow){dereference.type=element_type(base.type);dereference.lvalue=true;receiver=&dereference;}
    std::vector<Expr*> arguments{receiver};
    for(std::size_t i=1;i<call.args.size();++i)arguments.push_back(call.args[i].get());
    auto [owner,method]=select(extension_methods(target.text,target.token),types,arguments,target.token,false,allow_no_match);
    if(!method)return false;
    if(is_reference(method->type)&&!receiver->lvalue)
        throw Error(target.token.at,"cannot obtain a reference from a temporary extension receiver", "BLT2011");
    auto value=std::move(target.args[0]);
    if(target.arrow){dereference.args.push_back(std::move(value));value=std::make_unique<Expr>(std::move(dereference));}
    const auto token=target.token;const auto name=target.text;const auto type_args=target.type_args;
    target=Expr(K::Name,token,name);target.type_args=type_args;target.binding=B::Method;
    target.type="#method";target.owner=owner->name;target.resolved=method->id;target.is_static=true;target.is_extension=true;
    call.args.insert(call.args.begin()+1,std::move(value));
    normalize_arguments(*owner,*method,call.args,1);
    call.binding=B::Method;call.type=referred(method->type);call.declared_type=method->type;
    call.lvalue=is_reference(method->type);call.owner=owner->name;call.resolved=method->id;call.is_extension=true;
    if(method->is_extern)program.require_cpp("native standard library / interop");
    return true;
}

void Analyzer::check_default(Class& owner,Parameter& parameter) {
    if(!parameter.default_value)return;
    auto* saved_class=current_class;auto* saved_method=current_method;const auto saved_static=static_context;
    auto saved_scopes=std::move(scopes);auto saved_flow=flow;flow={};current_class=&owner;current_method=nullptr;static_context=true;scopes.clear();
    auto restore=[&]{current_class=saved_class;current_method=saved_method;static_context=saved_static;scopes=std::move(saved_scopes);flow=std::move(saved_flow);};
    try {
        expression(*parameter.default_value,false,parameter.type);convertible(parameter.type,*parameter.default_value,parameter.constant);
        if(is_reference(parameter.type)||!pure(*parameter.default_value))throw Error(parameter.token.at,"default arguments must be pure value expressions; references cannot have defaults","BLT2010");
    }catch(...){restore();throw;}
    restore();
}

void Analyzer::normalize_arguments(Class& owner,Method& method,std::vector<ExprPtr>& arguments,std::size_t start) {
    const auto fixed=method.parameters.size()-(variadic(method)?1:0);
    while(arguments.size()-start<fixed) {
        auto& parameter=method.parameters[arguments.size()-start];
        if(!parameter.default_value)throw Error(method.token.at,"missing required parameter '"+parameter.name+"'","BLT2006");
        check_default(owner,parameter);arguments.push_back(clone(*parameter.default_value));
    }
    if(variadic(method)) {
        const auto& parameter=method.parameters.back();
        const bool array_form=arguments.size()-start==method.parameters.size()&&arguments.back()->type==parameter.type;
        if(!array_form) {
            auto pack=std::make_unique<Expr>(Expr::Kind::Array,method.token);pack->type=parameter.type;
            while(arguments.size()>start+fixed) {pack->args.push_back(std::move(arguments[start+fixed]));arguments.erase(arguments.begin()+static_cast<std::ptrdiff_t>(start+fixed));}
            for(auto& item:pack->args)convertible(element_type(parameter.type),*item,parameter.constant);
            pack->dimensions={pack->args.size()};arguments.push_back(std::move(pack));
        }
    }
    for(std::size_t i=0;i<method.parameters.size();++i) {
        const auto& p=method.parameters[i];auto& argument=arguments[start+i];
        convertible(p.type,*argument,p.constant);
        if(p.constant&&is_reference(p.type)&&is_interface(program,referred(p.type))&&argument->type!=referred(p.type)&&!derives(argument->type,referred(p.type))) {
            auto cast=std::make_unique<Expr>(Expr::Kind::Call,argument->token,"@explicit-cast");
            cast->type=referred(p.type);cast->binding=Expr::Binding::Cast;
            auto target=std::make_unique<Expr>(Expr::Kind::Name,argument->token,cast->type);target->binding=Expr::Binding::Cast;target->owner=cast->type;target->type="#cast";
            cast->args.push_back(std::move(target));cast->args.push_back(std::move(argument));argument=std::move(cast);
        }
    }
    for(std::size_t i=0;i<method.parameters.size();++i) {
        const auto& p=method.parameters[i];
        if(is_reference(p.type)&&!p.constant&&is_optional(referred(p.type)))if(auto id=optional_root(*arguments[start+i])){flow.escaped.insert(id);flow.present.erase(id);}
    }
    kill_escaped();
}

std::pair<Class*,Method*> Analyzer::constructor(const std::string& t,std::vector<Expr*>& args,const Token& at,bool base_subobject) {
    auto* c=find_class(t);
    if(!c)return {
        nullptr,nullptr
    };
    prepare(*c);
    if(c->is_abstract&&!base_subobject)throw Error(at.at,"abstract class '"+t+"' cannot be instantiated","BLT2220");
    if(c->is_interface)throw Error(at.at,"interface '"+t+"' cannot be constructed; initialize it from an implementing value", "BLT2202");
    auto candidates=methods(*c,c->short_name,true);
    if(candidates.empty()) {
        if(!args.empty())throw Error(at.at,"class '"+t+"' has only an implicit parameterless constructor", "BLT2000");
        if(!c->base.empty()) {
            std::vector<Expr*> empty;
            (void)constructor(c->base,empty,at,true);
        }
        return {
            c,nullptr
        };
    }
    auto result=select(std::move(candidates), {
    },args,at);
    Expr dummy(Expr::Kind::Name,at,c->short_name);
    const bool metadata_record=generated_metadata(at) && current_method->name=="GetType" &&
        c->token.at.file=="<bolt-stdlib>/System/Reflection.bolt" &&
        (c->short_name=="FieldInfo" || c->short_name=="MethodInfo" || c->short_name=="TypeInfo");
    if(!metadata_record)accessible(*result.first,result.second->is_public,result.second->is_protected,dummy);
    return result;
}

void Analyzer::default_constructible(const std::string& t,const Token& at) {
    mark_type(t);
    std::vector<Expr*> args;
    if(find_class(t))(void)constructor(t,args,at);
}

} // namespace bolt::semantic
