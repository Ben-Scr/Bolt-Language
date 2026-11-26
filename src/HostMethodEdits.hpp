#pragma once
#include "ParseCache.hpp"
#include "native/ReadOnlyAnalysis.hpp"
#include <bolt/Frontend.hpp>
#include <bolt/Types.hpp>
#include <bolt/native/IR.hpp>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace bolt {
void recheck_host_classes(Program&,const std::vector<Class*>&);
}

namespace bolt::host {
// Keep fields, class contracts and the externally visible method signatures in
// the key. Private scalar helpers and ordinary method bodies form the edit unit.
struct HostMethodFile {
    std::string owner;
    std::vector<Token> contract;
    std::set<std::string> helpers;
    struct Body {std::string code,header; int line,column;};
    std::unordered_map<std::size_t,Body> bodies;
    std::map<std::string,std::vector<std::string>> private_signatures;
    std::set<std::string> readonly_methods;

    static bool ordinary(const Method& m) {
        return m.body && m.body->token.text=="{" && !m.is_async && !m.is_extern && !m.is_abstract &&
            !m.is_getter && !m.is_setter && !m.is_conversion && !m.is_constructor && !m.is_destructor &&
            !m.is_extension && !m.is_constant_template && !m.is_default_implementation && m.attributes.empty() &&
            m.generic_parameters.empty() && m.pack_parameter.empty() && m.operator_symbol.empty() && m.constraints.empty();
    }
    static bool scalar(std::string_view type) {return is_numeric(type)||type=="bool";}
    static bool helper(const Method& m) {
        return ordinary(m) && m.name!="Main" && !m.is_virtual && !m.is_override && !m.is_public && !m.is_protected &&
            !m.is_sealed && (scalar(m.type)||m.type=="void") &&
            std::all_of(m.parameters.begin(),m.parameters.end(),[](const Parameter& p){
                return scalar(p.type) && !p.default_value && !p.variadic && !p.pack;
            });
    }
    static bool syntax(const Expr& e) {
        using K=Expr::Kind;
        if(e.lambda || e.arena || !e.type_args.empty() || e.arrow) return false;
        switch(e.kind) {
        case K::Literal:case K::Name:case K::Member:case K::Call:case K::Binary:
        case K::Unary:case K::Assign:case K::Postfix:case K::Conditional:break;
        default:return false;
        }
        return std::all_of(e.args.begin(),e.args.end(),[](const auto& a){return syntax(*a);});
    }
    static bool syntax(const Stmt& s) {
        using K=Stmt::Kind;
        switch(s.kind) {
        case K::Block:case K::Declarations:case K::Variable:case K::Expression:case K::Return:case K::If:
        case K::While:case K::For:case K::Break:case K::Continue:case K::Empty:break;
        default:return false;
        }
        for(auto* e:{s.expression.get(),s.condition.get(),s.increment.get()}) if(e && !syntax(*e))return false;
        for(auto* child:{s.init.get(),s.body.get(),s.alternative.get()}) if(child && !syntax(*child))return false;
        return std::all_of(s.statements.begin(),s.statements.end(),[](const auto& n){return syntax(*n);});
    }
    static std::optional<HostMethodFile> describe(const Program& p,const std::string& file) {
        if(p.classes.size()!=1 || !p.enums.empty() || !p.concepts.empty())return {};
        const auto& c=p.classes.front();
        if(c.is_global || c.is_interface || c.is_native || c.is_struct || c.is_template_member ||
           !c.declaring_type.empty() || !c.attributes.empty() || !c.generic_parameters.empty() ||
           !c.pack_parameter.empty() || c.methods.empty() || !c.properties.empty())return {};
        for(const auto& f:c.fields) if(f.initializer || f.is_static || f.constant || !f.attributes.empty())return {};
        auto tokens=navigation_tokens(file,p.active_sources.at(file));
        struct Span {std::size_t first,last;};
        std::vector<Span> spans;
        HostMethodFile result;result.owner=c.name;
        for(const auto& m:c.methods) {
            if(!ordinary(m)||!syntax(*m.body))return {};
            auto index=[&](const Token& t){return static_cast<std::size_t>(
                std::lower_bound(tokens->begin(),tokens->end(),t.at.offset,[](const Token& x,auto n){return x.at.offset<n;})-tokens->begin());};
            auto first=index(m.body->token),end=first;
            if(first>=tokens->size() || (*tokens)[first].text!="{")return {};
            int depth=0;
            do {
                if(end==tokens->size())return {};
                const auto& t=(*tokens)[end++];
                if(t.kind==Token::Kind::Symbol)depth+=int(t.text=="{")-int(t.text=="}");
            } while(depth);
            const auto open=first;
            if(helper(m)) {
                auto j=index(m.token);
                while(j && (*tokens)[j-1].text!="{" && (*tokens)[j-1].text!="}" && (*tokens)[j-1].text!=";")--j;
                // An access section changes the interpretation of subsequent members.
                if(j<first && (*tokens)[j].text=="private" && (*tokens)[j+1].text!=":") {
                    first=j; result.helpers.insert(m.name);
                }
            }
            const auto& text=p.sources.at(file);
            const auto start=(*tokens)[open].at.offset;
            const auto last=(*tokens)[end-1].at.offset+1;
            auto header=text.substr((*tokens)[first].at.offset,start-(*tokens)[first].at.offset);
            if(first!=open)result.private_signatures[m.name].push_back(header);
            result.bodies.emplace(m.token.at.offset,Body{text.substr(start,last-start),std::move(header),
                m.body->token.at.line,m.body->token.at.column});
            spans.push_back({first,end});
        }
        std::sort(spans.begin(),spans.end(),[](auto a,auto b){return a.first<b.first;});
        std::size_t i=0;
        for(auto span:spans) {
            if(span.first<i)return {};
            result.contract.insert(result.contract.end(),tokens->begin()+i,tokens->begin()+span.first);
            i=span.last;
        }
        result.contract.insert(result.contract.end(),tokens->begin()+i,tokens->end());
        return result;
    }
    bool matches(const HostMethodFile& next) const {
        if(owner!=next.owner || contract.size()!=next.contract.size())return false;
        for(std::size_t i=0;i<contract.size();++i) {
            const auto& a=contract[i]; const auto& b=next.contract[i];
            if(a.kind!=b.kind || a.text!=b.text)return false;
            // Metadata and generated initializers can refer to declaration sites.
            if(a.kind!=Token::Kind::End && a.text!="}" && (a.at.line!=b.at.line || a.at.column!=b.at.column))return false;
        }
        return true;
    }
    using References=std::set<std::tuple<int,std::string,std::string,std::string>>;
    static void references(const Expr& e,References& out,const std::string& owner,const std::set<std::string>& helpers) {
        using B=Expr::Binding;
        if(e.binding==B::Cast && e.args.size()==2 && scalar(e.type) && scalar(e.args[1]->type)) {
            references(*e.args[1],out,owner,helpers);
            return;
        }
        if(e.binding==B::Method && e.owner==owner && !e.args.empty() &&
           e.args.front()->kind==Expr::Kind::Name && helpers.contains(e.args.front()->text)) {
            for(std::size_t i=1;i<e.args.size();++i)references(*e.args[i],out,owner,helpers);
            return;
        }
        if(e.binding!=B::None && e.binding!=B::Local && e.binding!=B::This)
            out.emplace(static_cast<int>(e.binding),e.owner,e.resolved,e.text);
        if(!e.type.empty() && e.type!="void" && !scalar(e.type)) out.emplace(-1,e.type,"","");
        for(const auto& a:e.args)references(*a,out,owner,helpers);
    }
    static void references(const Stmt& s,References& out,const std::string& owner,const std::set<std::string>& helpers) {
        if(s.kind==Stmt::Kind::Variable && !scalar(s.type))out.emplace(-1,s.type,"","");
        for(auto* e:{s.expression.get(),s.condition.get(),s.increment.get()})if(e)references(*e,out,owner,helpers);
        for(auto* child:{s.init.get(),s.body.get(),s.alternative.get()})if(child)references(*child,out,owner,helpers);
        for(const auto& child:s.statements)references(*child,out,owner,helpers);
    }

    ir::Module compile(Program& program,const std::string& file,std::string_view text,
                       const ir::MethodEnvironment& environment,std::chrono::steady_clock::duration& frontend_time) const {
        const auto start=std::chrono::steady_clock::now();
        auto parsed=parse_memory_source(text,file,program.source_preprocessors.at(file));
        auto next=describe(parsed,file);
        if(!next || !matches(*next))throw std::runtime_error("Changed class contract");
        auto* target=program.find_class(owner);
        if(!target || target->is_reflected)throw std::runtime_error("Reflected method edit");
        std::set<std::string> names=helpers;
        names.insert(next->helpers.begin(),next->helpers.end());
        for(const auto& name:names) {
            for(const auto& [path,source]:program.active_sources)
                if(path!=file && source.find(name)!=source.npos) throw std::runtime_error("Helper visible outside edit unit");
            for(const auto& t:contract)if(t.text==name)throw std::runtime_error("Helper referenced by class contract");
        }
        for(const auto& c:program.classes) if(c.declaring_type==owner || c.is_reflected && derives_from(program,c.name,owner))
            throw std::runtime_error("Edit requires dependent type metadata");

        std::string fresh(text),active(parsed.active_sources.at(file));
        Class saved=std::move(*target);
        *target=std::move(parsed.classes.front());
        struct Restore {
            Class& target;Class& saved;std::string& source;std::string& fresh;
            std::string& previous_active;std::string& active;
            ~Restore(){target=std::move(saved);source.swap(fresh);previous_active.swap(active);}
        };
        program.sources.at(file).swap(fresh);
        program.active_sources.at(file).swap(active);
        Restore restore{*target,saved,program.sources.at(file),fresh,program.active_sources.at(file),active};
        const auto classes=program.classes.size(), enums=program.enums.size();
        recheck_host_classes(program,{target});
        if(program.classes.size()!=classes || program.enums.size()!=enums)
            throw std::runtime_error("Edit introduced a specialization");
        std::vector<Method*> methods;
        for(auto& m:target->methods) {
            if(!ordinary(m) || !m.analyzed)throw std::runtime_error("Synthesized method");
            References now,prior;
            references(*m.body,now,owner,next->helpers);
            auto* old=method_by_id(saved,m.id);
            if(next->helpers.contains(m.name)) {
                if(!helper(m) || !now.empty()) throw std::runtime_error("Helper has external dependencies");
            } else {
                if(!old || !old->body)throw std::runtime_error("Changed externally visible signature");
                references(*old->body,prior,owner,helpers);
                if(!std::includes(prior.begin(),prior.end(),now.begin(),now.end()))
                    throw std::runtime_error("New method dependency");
            }
            if(old && readonly_methods.contains(old->id) &&
               !ir::ReadOnlyAnalysis(program).reads_only(m))
                throw std::runtime_error("Callers depend on receiver effects");
            if(old) {
                const auto& before=bodies.at(old->token.at.offset);
                const auto& after=next->bodies.at(m.token.at.offset);
                const bool stable=before.code==after.code && before.header==after.header &&
                    before.line==after.line && before.column==after.column && old->type==m.type;
                // A declaration edit may rebind an overload even when the
                // caller's source did not change. Re-emit possible callers.
                if(stable && (private_signatures==next->private_signatures ||
                    std::none_of(names.begin(),names.end(),[&](const auto& name){return after.code.find(name)!=after.code.npos;})))
                    continue;
            }
            methods.push_back(&m);
        }
        if(methods.empty())throw std::runtime_error("Edit has no replaceable method bodies");
        frontend_time=std::chrono::steady_clock::now()-start;
        return ir::lower_methods(program,*target,methods,environment);
    }
};
}
