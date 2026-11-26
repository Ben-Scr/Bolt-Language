#pragma once
#include <bolt/Ast.hpp>
#include <algorithm>
#include <functional>
#include <unordered_map>

namespace bolt {
// Resolve lexical captures before type checking. Only bindings actually used
// across a lambda boundary get a cell. The ordinary declaration still performs
// type inference and conversions before its initial value is moved into scope.
class ClosureConversion {
    struct Binding {
        std::string name, cell;
        Token token;
        bool constant;
        std::size_t depth;
        Stmt* declaration;
        StmtPtr* entry;
        std::vector<Expr*> uses;
        bool captured=false;
    };
    std::deque<Binding> bindings;
    std::vector<std::unordered_map<std::string,Binding*>> scopes;
    std::vector<LambdaSyntax*> lambdas;

    void define(const std::string& name,const Token& token,bool constant,Stmt* declaration,StmtPtr* entry) {
        auto& b=bindings.emplace_back(Binding{name,"@cell"+std::to_string(bindings.size()),token,constant,lambdas.size(),declaration,entry});
        scopes.back()[name]=&b;
    }
    void expression(Expr& e) {
        if(e.lambda) {
            auto& l=*e.lambda;
            scopes.emplace_back();lambdas.push_back(&l);
            for(const auto& p:l.parameters)define(p.name.text,p.name,false,nullptr,&l.body);
            statement(*l.body);
            lambdas.pop_back();scopes.pop_back();
            return;
        }
        bool bound=false;
        if(e.kind==Expr::Kind::Name)for(auto i=scopes.rbegin();i!=scopes.rend();++i) {
            auto local=i->find(e.text);
            if(local==i->end())continue;
            auto& b=*local->second;
            bound=true;
            b.uses.push_back(&e);
            for(auto level=b.depth;level<lambdas.size();++level) {
                b.captured=true;
                const auto& captured=b.constant?b.name:b.cell;
                auto& names=lambdas[level]->captures;
                if(std::find(names.begin(),names.end(),captured)==names.end())names.push_back(captured);
            }
            break;
        }
        if(e.kind==Expr::Kind::Name&&!bound)for(auto* lambda:lambdas) {
            auto& names=lambda->free_names;
            if(std::find(names.begin(),names.end(),e.text)==names.end())names.push_back(e.text);
        }
        if(e.arena)expression(*e.arena);
        for(auto& a:e.args)expression(*a);
    }
    void scoped(StmtPtr& s) {
        if(s){scopes.emplace_back();statement(*s);scopes.pop_back();}
    }
    void statement(Stmt& s) {
        using K=Stmt::Kind;
        const bool scope=s.kind==K::Block||s.kind==K::For||s.kind==K::ForEach||s.kind==K::Catch;
        if(scope)scopes.emplace_back();
        if(s.init)statement(*s.init);
        if(s.expression)expression(*s.expression);
        if(s.condition)expression(*s.condition);
        if(s.increment)expression(*s.increment);
        if(s.kind==K::Variable)define(s.name,s.name_token,s.constant,&s,nullptr);
        if(s.kind==K::ForEach||(s.kind==K::Catch&&!s.name.empty()))define(s.name,s.name_token,s.constant,nullptr,&s.body);
        for(auto& child:s.statements)statement(*child);
        scoped(s.body);scoped(s.alternative);
        if(scope)scopes.pop_back();
    }
    static StmtPtr make_cell(const Binding& b) {
        auto cell=std::make_unique<Stmt>(Stmt::Kind::Variable,b.token);
        cell->type="var";cell->name=b.cell;cell->name_token=b.token;
        auto call=std::make_unique<Expr>(Expr::Kind::Call,b.token);
        auto create=std::make_unique<Expr>(Expr::Kind::Member,b.token,"Create");
        create->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,b.token,"Bolt.Runtime.ClosureStorage"));
        call->args.push_back(std::move(create));
        call->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,b.token,b.name));
        cell->expression=std::move(call);
        return cell;
    }
    void rewrite(Binding& b) {
        if(!b.captured||b.constant)return;
        for(auto* use:b.uses) {
            const auto token=use->token;
            auto get=std::make_unique<Expr>(Expr::Kind::Member,token,"Get");
            get->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,token,b.cell));
            *use=Expr(Expr::Kind::Call,token);
            use->args.push_back(std::move(get));
        }
        if(b.declaration) {
            auto original=std::make_unique<Stmt>(std::move(*b.declaration));
            *b.declaration=Stmt(Stmt::Kind::Declarations,b.token);
            b.declaration->statements.push_back(std::move(original));
            b.declaration->statements.push_back(make_cell(b));
        }else {
            auto& entry=*b.entry;
            if(entry->kind!=Stmt::Kind::Block) {
                auto block=std::make_unique<Stmt>(Stmt::Kind::Block,entry->token);
                block->statements.push_back(std::move(entry));entry=std::move(block);
            }
            entry->statements.insert(entry->statements.begin(),make_cell(b));
        }
    }
public:
    static bool contains_lambda(const Expr& e) {
        if(e.lambda||(e.arena&&contains_lambda(*e.arena)))return true;
        for(const auto& a:e.args)if(contains_lambda(*a))return true;
        return false;
    }
    static bool contains_lambda(const Stmt& s) {
        for(const auto* e:{s.expression.get(),s.condition.get(),s.increment.get()})
            if(e&&contains_lambda(*e))return true;
        for(const auto* child:{s.init.get(),s.body.get(),s.alternative.get()})
            if(child&&contains_lambda(*child))return true;
        for(const auto& child:s.statements)if(contains_lambda(*child))return true;
        return false;
    }
    void run(Method& method) {
        scopes.emplace_back();
        for(const auto& p:method.parameters)define(p.name,p.name_token,p.constant,nullptr,&method.body);
        statement(*method.body);
        for(auto& b:bindings)rewrite(b);
    }
    void run(Expr& initializer) {
        scopes.emplace_back();expression(initializer);
        for(auto& b:bindings)rewrite(b);
    }
};
}
