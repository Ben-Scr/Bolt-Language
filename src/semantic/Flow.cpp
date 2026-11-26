#include "Analyzer.hpp"
#include <bolt/Value.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <iterator>

namespace bolt::semantic {

Analyzer::Facts Analyzer::intersect(const Facts& a,const Facts& b) {
    Facts result;std::set_intersection(a.begin(),a.end(),b.begin(),b.end(),std::inserter(result,result.end()));return result;
}

std::size_t Analyzer::optional_root(const Expr& e) const {
    if(!is_optional(e.type)||e.binding!=Expr::Binding::Local)return 0;
    auto id=e.local_id;
    if(auto it=flow.aliases.find(id);it!=flow.aliases.end())id=it->second;
    return id;
}

bool Analyzer::is_present(const Expr& e) const {auto id=optional_root(e);return id&&flow.present.contains(id);}

void Analyzer::extract_optional(Expr& e) {
    auto original=std::make_unique<Expr>(std::move(e));
    e=Expr(Expr::Kind::OptionalValue,original->token,"Value");
    e.type=element_type(original->type);e.lvalue=original->lvalue;e.constant=original->constant;
    e.declared_type=e.lvalue?e.type+"&":e.type;e.args.push_back(std::move(original));
}

void Analyzer::narrow(Expr& e) {
    if(!is_optional(e.type))return;
    if(!is_present(e))throw Error(e.token.at,"optional '"+e.type+"' may be empty; check != null / HasValue, use ??, or access the checked .Value", "BLT2402");
    extract_optional(e);
}

void Analyzer::kill_escaped() {for(auto id:flow.escaped)flow.present.erase(id);}

void Analyzer::optional_assignment(const Expr& dest,const Expr& source) {
    const auto id=optional_root(dest);
    if(!id){if(is_optional(dest.type))kill_escaped();return;}
    const bool present=source.type!="null"&&source.type!="#default"&&source.kind!=Expr::Kind::Default&&(!is_optional(source.type)||is_present(source));
    flow.present.erase(id);if(present)flow.present.insert(id);
}

std::pair<Analyzer::Facts,Analyzer::Facts> Analyzer::condition_facts(const Expr& e) const {
    if(auto it=flow.conditions.find(&e);it!=flow.conditions.end())return it->second;
    auto yes=flow.present,no=flow.present;
    const Expr* value=nullptr;bool positive=true;
    if(e.kind==Expr::Kind::OptionalHas)value=e.args[0].get();
    if(e.kind==Expr::Kind::Binary&&(e.text=="=="||e.text=="!=")) {
        if(e.args[1]->type=="null")value=e.args[0].get();
        else if(e.args[0]->type=="null")value=e.args[1].get();
        positive=e.text=="!=";
    }
    if(value)if(const auto id=optional_root(*value)) {
        (positive?yes:no).insert(id);(positive?no:yes).erase(id);
    }
    return {std::move(yes),std::move(no)};
}

void Analyzer::remember_condition(const Expr& e) {
    flow.conditions.erase(&e);
    if(e.type=="bool") {
        if(e.kind==Expr::Kind::Unary&&e.text=="!") {auto f=condition_facts(*e.args[0]);flow.conditions[&e]={f.second,f.first};}
        else flow.conditions[&e]=condition_facts(e);
    }
}

const Analyzer::Symbol* Analyzer::symbol(const std::string& n) const {
    for(auto it=scopes.rbegin();it!=scopes.rend();++it) {
        auto f=it->find(n);
        if(f!=it->end())return &f->second;
    }
    return nullptr;
}

bool Analyzer::local_arena_alias(const Expr& e,bool storage) const {
    if(e.kind==Expr::Kind::New&&e.arena)return e.arena->binding==Expr::Binding::Local&&e.arena->declared_type=="Arena";
    if(e.binding==Expr::Binding::Local) {
        const auto* s=symbol(e.text);
        return s&&s->id==e.local_id&&s->local_arena_alias&&(!storage||is_reference(s->type));
    }
    // A pointer field is a borrowed alias: its value need not point into
    // the arena that holds the field. Track its storage separately.
    if(e.kind==Expr::Kind::Member&&!e.is_static)return storage&&local_arena_alias(*e.args[0],!e.arrow);
    if(e.kind==Expr::Kind::Index)return storage&&local_arena_alias(*e.args[0],true);
    if(e.kind==Expr::Kind::Unary&&e.text=="&")return local_arena_alias(*e.args[0],true);
    if(e.kind==Expr::Kind::Unary&&e.text=="*")return storage&&local_arena_alias(*e.args[0]);
    if(e.kind==Expr::Kind::Assign||(e.kind==Expr::Kind::Binary&&e.text==","))return local_arena_alias(*e.args[1],storage);
    if(e.binding==Expr::Binding::Cast&&e.args.size()==2)return local_arena_alias(*e.args[1]);
    return false;
}

void Analyzer::set_local_arena_alias(std::size_t id,bool alias) {
    for(auto& scope:scopes)for(auto& [name,s]:scope) {
        (void)name;
        if(s.id==id){s.local_arena_alias=alias;return;}
    }
}

std::size_t Analyzer::add_symbol(const std::string& n,const std::string& t,bool constant,const Token& at,bool parameter) {
    if(scopes.back().contains(n))throw Error(at.at,"duplicate local or parameter '"+n+"'", "BLT2000");
    auto id=++next_local;
    scopes.back().emplace(n,Symbol {
        t,constant,id,parameter
    });
    return id;
}

} // namespace bolt::semantic
