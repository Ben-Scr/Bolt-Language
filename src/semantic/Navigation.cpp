#include "Analyzer.hpp"
#include "../ParseCache.hpp"
#include <bolt/Types.hpp>
#include <algorithm>
#include <functional>
#include <limits>

namespace bolt::semantic {

// Bind parser-retained physical type tokens using the compiler namespace and import rules.
void Analyzer::navigation_bindings(bool collect_references) {
    program.semantic_references.clear();
    dependent_source_types.clear();
    std::unordered_map<std::string,std::shared_ptr<const std::vector<Token>>> tokens_by_file;
    auto tokens_for = [&](const std::string& file) -> const std::vector<Token>* {
        auto [it, inserted] = tokens_by_file.try_emplace(file);
        if (inserted) {
            const auto raw = program.sources.find(file);
            if (raw == program.sources.end()) return nullptr;
            const auto active = program.active_sources.find(file);
            try {
                it->second = navigation_tokens(file, active == program.active_sources.end() ? raw->second : active->second);
            } catch (const Error&) { return nullptr; }
        }
        return it->second.get();
    };
    auto bind=[&](const Token& token,std::string id,std::string name,std::string category,
                  const Location& declaration,std::string detail={},std::string owner={}) {
        if(token.at.file.empty()||declaration.file.empty())return;
        program.semantic_references.push_back({token,std::move(id),std::move(name),std::move(category),std::move(detail),std::move(owner),declaration});
    };
    struct GenericScope {std::size_t begin,end;std::map<std::string,Token> parameters;};
    std::unordered_map<std::string,std::vector<GenericScope>> scopes_by_file;
    auto generics=[&](const Token& name,std::vector<std::string> parameters,const std::string& pack,bool method) {
        if(!pack.empty())parameters.push_back(pack);
        if(parameters.empty())return;
        const auto* found=tokens_for(name.at.file);if(!found)return;
        const auto& ts=*found;
        auto it=std::lower_bound(ts.begin(),ts.end(),name.at.offset,[](const Token& t,std::size_t offset){return t.at.offset<offset;});
        if(it==ts.end()||it->at.offset!=name.at.offset)return;
        const auto at=static_cast<std::size_t>(it-ts.begin());
        if(at+1>=ts.size()||ts[at+1].text!="<")return;
        GenericScope scope{name.at.offset,name.at.offset,{}};
        auto close=at+2;
        for(;close<ts.size()&&ts[close].text!=">";++close)
            if(std::find(parameters.begin(),parameters.end(),ts[close].text)!=parameters.end())scope.parameters.emplace(ts[close].text,ts[close]);
        if(close==ts.size())return;
        auto begin=at;
        if(method)while(begin&&ts[begin-1].text!=";"&&ts[begin-1].text!="{"&&ts[begin-1].text!="}")--begin;
        scope.begin=ts[begin].at.offset;
        auto end=close+1;
        while(end<ts.size()&&ts[end].text!="{"&&ts[end].text!=";")++end;
        if(end<ts.size()&&ts[end].text=="{") {
            int braces=1;++end;
            while(end<ts.size()&&braces){if(ts[end].text=="{")++braces;else if(ts[end].text=="}")--braces;++end;}
        }
        scope.end=end<ts.size()?ts[end].at.offset:std::numeric_limits<std::size_t>::max();
        if(collect_references)for(const auto& [parameter,token]:scope.parameters)
            bind(token,"type-parameter:"+token.at.file+":"+std::to_string(token.at.offset),parameter,"typeParameter",token.at,"type parameter "+parameter,name.text);
        scopes_by_file[name.at.file].push_back(std::move(scope));
    };
    for(const auto& c:program.classes) {
        generics(c.token,c.generic_parameters,c.pack_parameter,false);
        for(const auto& m:c.methods)if(!m.is_specialization)generics(m.token,m.generic_parameters,m.pack_parameter,true);
    }
    for(const auto& c:program.concepts)generics(c.token,c.parameters,"",false);
    auto parameter=[&](const SourceType& source,bool root=false)->const Token* {
        const auto found=scopes_by_file.find(source.token.at.file);
        if(found==scopes_by_file.end())return nullptr;
        const auto name=root?source.name.substr(0,source.name.find_first_of(".<")):source.name;
        const GenericScope* best=nullptr;
        for(const auto& scope:found->second)
            if(scope.begin<=source.token.at.offset&&source.token.at.offset<scope.end&&scope.parameters.contains(name))
                if(!best||scope.end-scope.begin<best->end-best->begin)best=&scope;
        return best?&best->parameters.at(name):nullptr;
    };
    auto concept_ref=[&](const std::string& name,const Token& token,Class* context) {
        try {
            if(const auto* c=find_concept(name,context,token)) {
                bind(token,"concept:"+c->name,c->name,"concept",c->token.at,"concept "+c->name,c->namespace_name);return;
            }
            for(const auto builtin:builtin_concept_names())if(name==builtin) {
                program.semantic_references.push_back({token,"",name,"concept","built-in concept "+name,"",{}});return;
            }
            const auto resolved=resolve_name(name,context);
            if(const auto* c=find_class(resolved);c&&c->is_interface)
                bind(token,"class:"+c->name,c->short_name,"interface",c->token.at,c->name,c->declaring_type.empty()?c->namespace_name:c->declaring_type);
        }catch(const Error&) { /* Ambiguous names have no selected declaration. */ }
    };
    for(const auto& source:program.source_types) {
        if(source.name.find('.')!=std::string::npos&&parameter(source,true)) {
            dependent_source_types.emplace(source.token.at.file,source.token.at.offset);
            continue;
        }
        if(const auto* declaration=parameter(source)) {
            dependent_source_types.emplace(source.token.at.file,source.token.at.offset);
            if(collect_references)bind(source.token,"type-parameter:"+declaration->at.file+":"+std::to_string(declaration->at.offset),source.name,"typeParameter",declaration->at,"type parameter "+source.name);
            continue;
        }
        if(!collect_references)continue;
        Class scope;scope.namespace_name=source.namespace_name;scope.imports=source.imports;
        auto* context=find_class(source.owner);if(!context)context=&scope;
        if(source.concept_name){concept_ref(source.name,source.token,context);continue;}
        try {
            auto name=resolve_name(source.name,context);
            if(name.empty())name=resolve_name(generic_declaration_name(source.name),context);
            if(const auto* c=find_class(name))
                bind(source.token,"class:"+c->name,c->short_name,c->is_interface?"interface":c->is_struct?"struct":"class",c->token.at,c->name,c->declaring_type.empty()?c->namespace_name:c->declaring_type);
            else if(const auto* e=find_enum(name))
                bind(source.token,"enum:"+e->name,e->short_name,"enum",e->token.at,e->name,e->declaring_type.empty()?e->namespace_name:e->declaring_type);
        }catch(const Error&) { /* Do not substitute another matching name. */ }
    }
    if(!collect_references)return;
    auto attribute=[&](const Attribute& a,Class* scope) {
        if(a.token.text!=a.name.substr(0,a.name.find('.')))return; // synthetic native marker
        const Class* target=nullptr;
        const bool builtin=std::any_of(builtin_attribute_names().begin(),builtin_attribute_names().end(),[&](auto name){return name==a.name;});
        try {
            if(builtin)target=find_class("System."+a.name+"Attribute");
            else {
                auto name=resolve_name(a.name,scope);
                if(name.empty())name=resolve_name(a.name+"Attribute",scope);
                target=find_class(name);
                std::set<std::string> visited;
                std::function<bool(const Class*)> derives_attribute=[&](const Class* c) {
                    if(!c||!visited.insert(c->name).second)return false;
                    if(c->name=="System.Attribute")return true;
                    auto base=[&](const std::string& n){return find_class(resolve_name(n,c));};
                    if(!c->base.empty()&&derives_attribute(base(c->base)))return true;
                    for(const auto& parent:c->interfaces)if(derives_attribute(base(parent)))return true;
                    return false;
                };
                if(!derives_attribute(target))target=nullptr;
            }
        }catch(const Error&) {return;}
        if(!target)return;
        const auto* found=tokens_for(a.token.at.file);if(!found)return;
        auto at=std::lower_bound(found->begin(),found->end(),a.token.at.offset,[](const Token& t,std::size_t offset){return t.at.offset<offset;});
        // Walk parsed qualified-name segments, allowing arbitrary whitespace.
        auto remaining=a.name;
        while(at!=found->end()) {
            const auto dot=remaining.find('.');const auto segment=remaining.substr(0,dot);
            if(at->text!=segment)break;
            if(dot==std::string::npos) {bind(*at,"class:"+target->name,target->short_name,"decorator",target->token.at,"attribute "+target->name,target->namespace_name);break;}
            ++at;if(at==found->end()||at->text!=".")break;++at;remaining.erase(0,dot+1);
        }
    };
    for(auto& c:program.classes) {
        for(const auto& a:c.attributes)attribute(a,&c);
        for(const auto& f:c.fields)for(const auto& a:f.attributes)attribute(a,&c);
        for(const auto& p:c.properties)for(const auto& a:p.attributes)attribute(a,&c);
        for(const auto& m:c.methods)if(!m.is_specialization)for(const auto& a:m.attributes)attribute(a,&c);
    }
    for(const auto& e:program.enums){Class scope;scope.namespace_name=e.namespace_name;scope.imports=e.imports;auto* owner=find_class(e.declaring_type);for(const auto& a:e.attributes)attribute(a,owner?owner:&scope);}
    std::function<void(const Expr&,Class&)> concept_expression=[&](const Expr& e,Class& scope) {
        if(e.kind==Expr::Kind::Name)concept_ref(e.text,e.token,&scope);
        else if(e.kind==Expr::Kind::Member) {
            std::function<std::string(const Expr&)> path=[&](const Expr& value){return value.kind==Expr::Kind::Name?value.text:value.kind==Expr::Kind::Member&&!value.args.empty()?path(*value.args[0])+"."+value.text:std::string{};};
            concept_ref(path(e),e.token,&scope);
        }else for(const auto& child:e.args)concept_expression(*child,scope);
    };
    for(const auto& c:program.concepts)if(c.expression){Class scope;scope.namespace_name=c.namespace_name;scope.imports=c.imports;concept_expression(*c.expression,scope);}
}

} // namespace bolt::semantic
