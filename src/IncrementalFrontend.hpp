#pragma once
#include "ParseCache.hpp"
#include <bolt/Frontend.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <charconv>
#include <optional>

// The host keeps one fully analyzed generation. Ordinary static methods can be
// rebound against its declarations. Native code generation always gets a new
// image. Other language features use the complete frontend.
namespace bolt {
void recheck_host_method(Program&,Class&,Method&);
void recheck_host_classes(Program&,const std::vector<Class*>&);

class IncrementalFrontend {
    struct Body {
        std::string owner;
        std::size_t name_offset, parameters, begin, end;
    };
    using Bodies=std::unordered_map<std::string,std::vector<Body>>;
    std::optional<Program> previous;
    Bodies bodies;
    std::unordered_map<std::string,std::string> configurations;
    std::unordered_set<std::string> owners;

    static bool scalar(std::string_view t) { return t=="bool" || is_numeric(t); }
    static bool plain_value(std::string_view t) {
        return scalar(t) || (is_array(t) && scalar(element_type(t)));
    }
    static bool plain_method(const Method& m) {
        return m.is_static && !m.is_extern && !m.is_async && !m.is_constructor &&
            !m.is_specialization && !m.is_constant_template && !m.is_virtual &&
            !m.is_getter && !m.is_setter && !m.is_extension && !m.is_conversion &&
            m.generic_parameters.empty() && m.pack_parameter.empty() && m.attributes.empty() &&
            m.constraints.empty() && m.native_abi.empty() && m.native_name.empty() &&
            m.operator_symbol.empty() && (m.type=="void" || scalar(m.type)) &&
            std::all_of(m.parameters.begin(),m.parameters.end(),[](const auto& p) {
                return plain_value(referred(p.type)) && !p.default_value && !p.pack && !p.variadic && !p.is_this;
            });
    }
    // Rechecking a body runs overload resolution again. Library declarations
    // stay bound, so both generations must select the same external methods.
    using Calls=std::vector<const Method*>;
    bool plain_expression(const Expr& e,Calls* calls) const {
        using K=Expr::Kind;
        const bool typed=calls!=nullptr;
        if(!e.type_args.empty() || e.arena || e.lambda || (e.arrow && e.kind!=K::New)) return false;
        switch(e.kind) {
        case K::Call: {
            if(e.args.empty()) return false;
            bool cast=e.args.front()->kind==K::Name && scalar(e.args.front()->text);
            const auto& target=*e.args.front();
            if(typed && !(cast && e.binding==Expr::Binding::Cast)) {
                if(e.binding!=Expr::Binding::Method || e.is_extension || !e.callback_owner.empty()) return false;
                if(!owners.contains(e.owner)) {
                    const auto* owner=previous->find_class(e.owner);
                    const auto* callee=owner?method_by_id(*owner,e.resolved):nullptr;
                    if(!callee || !owner->token.at.file.starts_with("<bolt-stdlib>/") ||
                        !owner->is_static || !callee->is_static || !callee->analyzed || callee->is_specialization ||
                        !callee->generic_parameters.empty() || !callee->pack_parameter.empty()) return false;
                    calls->push_back(callee);
                }
            }
            if(!target.type_args.empty() || (target.kind!=K::Name && target.kind!=K::Member)) return false;
            if(target.kind==K::Member && (target.arrow || target.args.size()!=1 ||
                target.args.front()->kind!=K::Name || !target.args.front()->type_args.empty())) return false;
            return std::all_of(e.args.begin()+1,e.args.end(),[&](const auto& a){
                return (a->kind==K::Literal && a->token.kind==Token::Kind::String) || plain_expression(*a,calls);
            }) && (!typed || plain_value(e.type) || e.type=="void");
        }
        case K::Literal:
            return e.token.kind==Token::Kind::Number ||
                (e.token.kind==Token::Kind::Identifier && (e.text=="true" || e.text=="false"));
        case K::Name:
            return !typed || (e.binding==Expr::Binding::Local && plain_value(e.type));
        case K::Member:
            if(e.args.size()!=1 || (e.text!="Length" && e.text!="Count" && e.text!="Rank") ||
                (typed && (e.binding!=Expr::Binding::Property || !is_array(e.owner)))) return false;
            break;
        case K::New:
            if(!e.arrow || !scalar(e.text) || (typed && !is_array(e.type))) return false;
            break;
        case K::Index:
            if(typed && (e.binding!=Expr::Binding::None || !is_array(e.owner) || !scalar(e.type))) return false;
            break;
        case K::Unary: case K::Postfix: case K::Binary: case K::Assign: case K::Conditional:
            if(typed && (!scalar(e.type) || e.binding==Expr::Binding::Operator)) return false;
            break;
        default: return false;
        }
        return std::all_of(e.args.begin(),e.args.end(),[&](const auto& a){return plain_expression(*a,calls);});
    }
    bool plain_body(const Stmt& s,Calls* calls) const {
        using K=Stmt::Kind;
        switch(s.kind) {
        case K::Block: case K::Declarations: case K::Expression: case K::If: case K::While:
        case K::For: case K::Return: case K::Break: case K::Continue: case K::Empty: break;
        case K::Variable: if(!plain_value(s.type) && (calls || s.type!="var")) return false; break;
        default: return false;
        }
        for(const auto* e:{s.expression.get(),s.condition.get(),s.increment.get()})
            if(e&&!plain_expression(*e,calls)) return false;
        for(const auto* child:{s.init.get(),s.body.get(),s.alternative.get()})
            if(child&&!plain_body(*child,calls)) return false;
        return std::all_of(s.statements.begin(),s.statements.end(),[&](const auto& child){return plain_body(*child,calls);});
    }
    // A changed decimal int stored into an ordinary int cannot introduce a
    // new overload, specialization or reachable type. This also holds inside
    // methods that use libraries, arrays or other non-scalar code elsewhere.
    static bool changed_initializer(const Stmt& s, std::string_view old_body,
                                    std::string_view new_body, const Location& body) {
        const Expr* value = nullptr;
        if (s.kind == Stmt::Kind::Variable && !s.constant && s.type == "int")
            value = s.expression.get();
        if (s.kind == Stmt::Kind::Expression && s.expression &&
            s.expression->kind == Expr::Kind::Assign && s.expression->text == "=" &&
            s.expression->args.size() == 2) {
            const auto& dest = *s.expression->args.front();
            if (dest.type == "int" && (dest.binding == Expr::Binding::Local ||
                (dest.kind == Expr::Kind::Index && dest.binding == Expr::Binding::None && is_array(dest.owner))))
                value = s.expression->args.back().get();
        }
        if (value && value->kind == Expr::Kind::Literal && value->type == "int" &&
            value->token.kind == Token::Kind::Number && value->token.at.file == body.file &&
            value->token.at.offset >= body.offset) {
            const auto offset = value->token.at.offset - body.offset;
            const auto original = std::string_view(value->token.text);
            if (offset <= old_body.size() && old_body.substr(offset).starts_with(original) &&
                new_body.starts_with(old_body.substr(0, offset))) {
                const auto tail = old_body.substr(offset + original.size());
                if (new_body.size() > offset + tail.size() && new_body.ends_with(tail)) {
                    auto literal = new_body.substr(offset, new_body.size() - offset - tail.size());
                    int number;
                    auto [end, ec] = std::from_chars(literal.data(), literal.data() + literal.size(), number);
                    if (literal.front() >= '1' && literal.front() <= '9' && ec == std::errc{} &&
                        end == literal.data() + literal.size()) return true;
                }
            }
        }
        for (auto* child : {s.init.get(), s.body.get(), s.alternative.get()})
            if (child && changed_initializer(*child,old_body,new_body,body)) return true;
        for (const auto& child : s.statements)
            if (changed_initializer(*child,old_body,new_body,body)) return true;
        return false;
    }
    static Bodies locate(const Program& p) {
        Bodies result;
        if(!p.enums.empty() || !p.concepts.empty()) return result;
        for(const auto& c:p.classes) {
            if(!c.is_static || c.is_template_member || !c.generic_parameters.empty() || !c.pack_parameter.empty() ||
               !c.base.empty() || !c.interfaces.empty() || !c.fields.empty() || !c.properties.empty() ||
               !c.declaring_type.empty() || !c.attributes.empty()) return {};
            for(const auto& m:c.methods) {
                if(m.is_extern) {
                    if(m.native_abi!="C" || !m.is_static || !m.generic_parameters.empty() || !m.pack_parameter.empty() ||
                       !(m.type=="void" || scalar(m.type)) || std::any_of(m.parameters.begin(),m.parameters.end(),
                           [](const auto& a){return !scalar(a.type)||a.default_value||a.pack||a.variadic;})) return {};
                    continue;
                }
                if(!plain_method(m)) return {};
                if(!m.body || m.body->token.text!="{") continue;
                const auto& file=m.token.at.file;
                // Directive-controlled edits use normal preprocessing and full analysis.
                auto raw=p.sources.find(file);
                auto active=p.active_sources.find(file);
                if(raw==p.sources.end() || active==p.active_sources.end() ||
                   raw->second!=active->second || raw->second.find('#')!=std::string::npos) continue;
                auto tokens=navigation_tokens(file,p.active_sources.at(file));
                auto i=std::lower_bound(tokens->begin(),tokens->end(),m.body->token.at.offset,
                    [](const auto& t,std::size_t off){return t.at.offset<off;});
                if(i==tokens->end() || i->text!="{") continue;
                auto name=std::lower_bound(tokens->begin(),i,m.token.at.offset,
                    [](const auto& t,std::size_t off){return t.at.offset<off;});
                // Preserve Main and global signatures. For a normal static method
                // only its primitive parameters and its body may be rebound here.
                auto parameters=i->at.offset;
                if(m.name!="Main" && !c.is_global && name!=i && name->text==m.name &&
                   name+1!=i && (name+1)->text=="(" &&
                   std::all_of(m.parameters.begin(),m.parameters.end(),[](const auto& p){return scalar(p.type);}))
                    parameters=(name+1)->at.offset;
                unsigned depth=0;
                const auto begin=i->at.offset;
                for(;i!=tokens->end();++i) if(i->kind==Token::Kind::Symbol) {
                    if(i->text=="{") ++depth;
                    if(i->text=="}" && --depth==0) {
                        result[file].push_back({c.name,m.token.at.offset,parameters,begin,i->at.offset+1});
                        break;
                    }
                }
            }
        }
        for(auto& [file,spans]:result)
            std::sort(spans.begin(),spans.end(),[](const auto& a,const auto& b){return a.begin<b.begin;});
        return result;
    }
    static Method* method(Class& owner,const Body& b,const std::string& file) {
        for(auto& m:owner.methods) if(m.token.at.file==file && m.token.at.offset==b.name_offset && m.body) return &m;
        return nullptr;
    }
    // Compare all bytes outside the parameters and bodies of existing ordinary
    // methods. Names, visibility, class declarations, imports and entry point
    // stay part of the key. Each caller is then rebound against the new overloads.
    bool rebind_declarations(Program& syntax,const Bodies& next) {
        auto& p=*previous;
        if(next.empty()) return false;
        for(const auto& [file,after]:syntax.sources) {
            const auto& before=p.sources.at(file);
            if(before==after) continue;
            auto old=bodies.find(file);
            auto fresh=next.find(file);
            if(old==bodies.end() || fresh==next.end() || old->second.size()!=fresh->second.size()) return false;
            std::size_t a=0,b=0;
            for(std::size_t j=0;j<old->second.size();++j) {
                const auto& x=old->second[j];
                const auto& y=fresh->second[j];
                if(x.owner!=y.owner || before.compare(a,x.parameters-a,after,b,y.parameters-b)!=0) return false;
                if(before.compare(x.parameters,x.begin-x.parameters,after,y.parameters,y.begin-y.parameters)!=0) {
                    // Library methods remain bound. A library source mentioning
                    // this declaring type could also be a caller.
                    const auto* c=p.find_class(x.owner);
                    if(!c || c->is_global) return false;
                    for(const auto& [name,source]:p.sources)
                        if(name.starts_with("<bolt-stdlib>/") && source.find(c->short_name)!=source.npos) return false;
                }
                a=x.end; b=y.end;
            }
            if(before.compare(a,std::string::npos,after,b,std::string::npos)!=0) return false;
        }
        std::vector<Class*> classes;
        Calls before_calls,after_calls;
        for(const auto& parsed:syntax.classes) {
            auto* prior=p.find_class(parsed.name);
            if(!prior || !owners.contains(parsed.name)) return false;
            for(const auto& m:prior->methods)
                if(m.body && !plain_body(*m.body,&before_calls)) return false;
            for(const auto& m:parsed.methods)
                if(m.body && !plain_body(*m.body,nullptr)) return false;
            classes.push_back(prior);
        }
        for(std::size_t i=0;i<classes.size();++i) {
            *classes[i]=clone(syntax.classes[i]);
            classes[i]->reachable=true;
        }
        recheck_host_classes(p,classes);
        for(const auto* c:classes) for(const auto& m:c->methods)
            if(m.body && !plain_body(*m.body,&after_calls)) return false;
        return before_calls==after_calls;
    }
    bool reuse(Program& syntax,const Bodies& next) {
        if(!previous || bodies.empty() || configurations.size()!=syntax.sources.size()) return false;
        auto& p=*previous;
        if(p.dependencies!=syntax.dependencies || p.selected_entry!=syntax.selected_entry ||
            p.project_identity!=syntax.project_identity || p.prelude_enabled!=syntax.prelude_enabled ||
            p.library_mode!=syntax.library_mode || p.external_sources!=syntax.external_sources ||
            preprocessor_identity(p.preprocessor)!=preprocessor_identity(syntax.preprocessor)) return false;
        struct Edit {Class* owner; Method* method; Method* syntax; bool plain; Calls calls;};
        std::vector<Edit> edits;
        bool rebuild_callers=false;
        for(const auto& [file,after]:syntax.sources) {
            auto config=configurations.find(file);
            if(config==configurations.end() || config->second!=preprocessor_identity(syntax.source_preprocessors.at(file))) return false;
            const auto& before=p.sources.at(file);
            if(before==after) continue;
            auto old=bodies.find(file);
            auto fresh=next.find(file);
            if(old==bodies.end() || fresh==next.end()) return false;
            bool matched=false;
            for(const auto& prior:old->second) {
                for(const auto& b:fresh->second) {
                    if(prior.owner!=b.owner || prior.name_offset!=b.name_offset || prior.begin!=b.begin ||
                       before.compare(0,prior.begin,after,0,b.begin)!=0 ||
                       before.compare(prior.end,std::string::npos,after,b.end,std::string::npos)!=0) continue;
                    // When later declarations exist, changes to their offsets or
                    // line numbers require rebinding the following declarations.
                    auto tail=std::string_view(after).substr(b.end);
                    if(tail.find_first_not_of("} \t\r\n")!=tail.npos &&
                       (prior.end!=b.end || before.substr(0,prior.end).find_last_of('\n')!=after.substr(0,b.end).find_last_of('\n') ||
                        std::count(before.begin()+prior.begin,before.begin()+prior.end,'\n')!=std::count(after.begin()+b.begin,after.begin()+b.end,'\n'))) continue;
                    auto* c=p.find_class(b.owner);
                    auto* m=c?method(*c,prior,file):nullptr;
                    auto* parsed=method(*syntax.find_class(b.owner),b,file);
                    if(!m || !parsed) continue;
                    const bool initializer=changed_initializer(*m->body,
                        std::string_view(before).substr(prior.begin, prior.end - prior.begin),
                        std::string_view(after).substr(b.begin, b.end - b.begin), m->body->token.at);
                    Calls calls;
                    if(!initializer && (!plain_body(*m->body,&calls) || !plain_body(*parsed->body,nullptr))) continue;
                    edits.push_back({c,m,parsed,!initializer,std::move(calls)});
                    matched=true;
                    break;
                }
                if(matched) break;
            }
            if(!matched) rebuild_callers=true;
        }
        // All candidate files have matching declarations. Recheck full bodies
        // including local scopes, overload selection and numeric conversions.
        // A change in library reachability requires a full analysis.
        if(rebuild_callers) {
            if(!rebind_declarations(syntax,next)) return false;
            edits.clear();
        }
        for(auto& edit:edits) {
            edit.method->body=clone(*edit.syntax->body);
            edit.method->analyzed=false;
            recheck_host_method(p,*edit.owner,*edit.method);
            Calls calls;
            if(edit.plain && (!plain_body(*edit.method->body,&calls) || edit.calls!=calls)) return false;
        }
        for(const auto& [file,raw]:syntax.sources) {
            p.sources[file]=raw;
            p.active_sources[file]=syntax.active_sources.at(file);
        }
        p.semantic_references.clear();
        return true;
    }
public:
    std::optional<Program> take_checked() {auto owned=std::move(previous);previous.reset();return owned;}
    Program& check(Program syntax, ParseCache& cache) {
        auto next=locate(syntax);
        try {
            if(reuse(syntax,next)) {
                bodies=std::move(next);
                cache.reuse_standard_library();
                return *previous;
            }
        } catch(const Error&) { /* Rebuild to obtain the complete source diagnostic. */ }
        catch(...) { previous.reset(); throw; }
        previous.reset();
        bodies.clear();
        owners.clear();
        configurations.clear();
        for(const auto& c:syntax.classes) owners.insert(c.name);
        for(const auto& [file,config]:syntax.source_preprocessors)
            configurations.emplace(file,preprocessor_identity(config));
        analyze_for_execution(syntax,!syntax.library_mode);
        bodies=std::move(next);
        // Native host generations expose executable modules and diagnostics.
        // Editor navigation metadata belongs to the tooling frontend and would
        // carry obsolete declaration offsets after a successful body replacement.
        syntax.source_types.clear();
        syntax.semantic_references.clear();
        previous.emplace(std::move(syntax));
        return *previous;
    }
};
}
