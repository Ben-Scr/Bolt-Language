#include <bolt/Frontend.hpp>
#include "ParseCache.hpp"
#include "ProjectInputs.hpp"
#include <bolt/Tooling.hpp>
#include <bolt/Project.hpp>
#include <bolt/Path.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <cctype>
#include <functional>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
namespace bolt {
    namespace {
        class Parser {
            std::vector<Token> tokens;
            std::size_t pos=0, depth=0;
            std::vector<SourceType> source_types;
            std::string source_owner, source_namespace;
            std::vector<std::string> source_imports;
            struct TokenEdit {
                std::size_t index;
                Token original, following;
                bool inserted;
            };
            std::vector<TokenEdit> token_edits;
            struct Checkpoint { std::size_t position, edits, types; };
            Checkpoint checkpoint() const { return {pos,token_edits.size(),source_types.size()}; }
            void restore(Checkpoint saved) {
                while(token_edits.size()>saved.edits) {
                    auto edit=std::move(token_edits.back());
                    token_edits.pop_back();
                    tokens[edit.index]=std::move(edit.original);
                    if(edit.inserted)tokens.erase(tokens.begin()+static_cast<std::ptrdiff_t>(edit.index+1));
                    else tokens[edit.index+1]=std::move(edit.following);
                }
                pos=saved.position;source_types.resize(saved.types);
            }
            bool concept_expression=false, requirement_expression=false;
            std::string class_pack;
            struct Guard {
                Parser& p;
                explicit Guard(Parser& x):p(x) {
                    if(++p.depth>256) throw Error(p.peek().at,"syntax nesting limit exceeded", "BLT1100");
                }
                ~Guard() {
                    --p.depth;
                }
            };
            const Token& peek(std::size_t n=0) const {
                return tokens[std::min(pos+n,tokens.size()-1)];
            }
            bool is(std::string_view s) const {
                return peek().kind!=Token::Kind::String && peek().kind!=Token::Kind::Character && peek().text==s;
            }
            bool match(std::string_view s) {
                if(!is(s))return false;
                ++pos;
                return true;
            }
            Token take() {
                return tokens[pos++];
            }
            Token expect(const std::string& s) {
                if(s==">"&&(is(">>")||is(">>="))) {
                    auto original=peek();auto remainder=original;
                    remainder.text=original.text.substr(1);++remainder.at.offset;++remainder.at.column;--remainder.at.length;
                    token_edits.push_back({pos,original,peek(1),true});
                    tokens[pos].text=">";tokens[pos].at.length=1;
                    tokens.insert(tokens.begin()+static_cast<std::ptrdiff_t>(pos+1),std::move(remainder));
                }
                if(s==">"&&is(">=")) {
                    // Close a generic list without requiring whitespace before '='
                    // or '=='. Speculative parsing restores these edits on fallback.
                    auto original=peek();
                    auto remainder=original;
                    remainder.text="=";
                    ++remainder.at.offset;
                    ++remainder.at.column;
                    remainder.at.length=1;
                    const bool equality=peek(1).kind==Token::Kind::Symbol&&peek(1).text=="="&&
                        peek(1).at.offset==original.at.offset+2;
                    token_edits.push_back({pos,original,peek(1),!equality});
                    tokens[pos].text=">";
                    tokens[pos].at.length=1;
                    if(equality) {
                        remainder.text="==";
                        remainder.at.length=2;
                        tokens[pos+1]=std::move(remainder);
                    }else tokens.insert(tokens.begin()+static_cast<std::ptrdiff_t>(pos+1),std::move(remainder));
                }
                if(!is(s))throw Error(peek().at,"expected '"+s+"', got '"+peek().text+"'", "BLT1101");
                return take();
            }
            Token identifier() {
                static const std::unordered_set<std::string> reserved= {
                    "event","implicit","explicit","class","struct","abstract","async","await","try","catch","finally","throw","require","extern","char","interface","operator","enum","concept","namespace","import","public","private","protected","static","const","var","sealed","virtual","override","if","else","elif","while","for","return","break","continue","true","false","this","null","default","sizeof","new","delete","void","byte","short","ushort","int","uint","long","ulong","float","double","bool","string"
                };
                if(peek().kind!=Token::Kind::Identifier||reserved.contains(peek().text)||is("switch")||is("case"))throw Error(peek().at,"expected an identifier", "BLT1100");
                return take();
            }
            std::string qualified() {
                auto s=identifier().text;
                while(match("."))s+="."+identifier().text;
                return s;
            }
            // In expressions a '<' can be either comparison or generic arguments.
            // Reject impossible short prefixes without constructing a diagnostic.
            // Long or nested candidates still go through the full parser.
            bool possible_type_arguments() const {
                const auto limit=std::min(tokens.size(),pos+17);
                for(auto i=pos+1;i<limit;++i) {
                    const auto& token=tokens[i];
                    if(token.kind==Token::Kind::Identifier)continue;
                    if(token.kind!=Token::Kind::Symbol)return false;
                    const auto& symbol=token.text;
                    if(symbol.starts_with(">"))return true;
                    if(symbol!="<"&&symbol!="."&&symbol!=","&&symbol!="..."&&symbol!="*"&&symbol!="&"&&symbol!="?"&&symbol!="["&&symbol!="]")return false;
                }
                return true;
            }
            std::vector<std::string> type_arguments() {
                expect("<");
                std::vector<std::string> a;
                if(match(">"))return a;
                do {
                    a.push_back(type());
                    if(match("..."))a.back()+="...";
                }
                while(match(","));
                expect(">");
                return a;
            }
            std::string type() {
                Guard guard(*this);
                if(is("var"))throw Error(peek().at,"'var' is only valid for local variable declarations with an initializer", "BLT1100");
                if(peek().kind!=Token::Kind::Identifier || is("delete") || is("new") || is("null") || is("this") || is("default") || is("true") || is("false"))throw Error(peek().at,"expected a type", "BLT1100");
                auto name_token=take();auto t=name_token.text;
                source_types.push_back({name_token,t,source_owner,source_namespace,source_imports});
                if(is("<"))t=generic_name(t,type_arguments());
                while(match(".")){
                    name_token=identifier();t+="."+name_token.text;
                    source_types.push_back({name_token,t,source_owner,source_namespace,source_imports});
                    if(is("<"))t=generic_name(t,type_arguments());
                }
                for(;;) {
                    if(is("[")&&(peek(1).text=="]"||peek(1).text==",")) {
                        take();
                        std::size_t rank=1;
                        while(match(","))++rank;
                        expect("]");
                        t+=array_suffix(rank);
                    }else if(match("*"))t+="*";
                    else if(match("&"))t+="&";
                    else if(match("?"))t+="?";
                    else break;
                }
                return t;
            }
            void generic_parameters(std::vector<std::string>& fixed,std::string& pack,std::vector<std::string>* defaults=nullptr) {
                if(!match("<"))return;
                do {
                    auto t=identifier();
                    if(match("...")) {
                        pack=t.text;
                        if(!is(">"))throw Error(peek().at,"a generic type pack must be last", "BLT1100");
                    }else {
                        fixed.push_back(t.text);
                        if(defaults){
                            if(match("="))defaults->push_back(type());
                            else if(!defaults->empty())throw Error(t.at,"required type parameter after a default", "BLT1100");
                        }
                    }
                }
                while(match(","));
                expect(">");
                std::unordered_set<std::string> seen;
                for(const auto& p:fixed)if(!seen.insert(p).second)throw Error(peek().at,"duplicate generic parameter", "BLT1100");
                if(!pack.empty()&&!seen.insert(pack).second)throw Error(peek().at,"duplicate generic parameter", "BLT1100");
            }
            std::vector<Constraint> constraints() {
                std::vector<Constraint> r;
                if(!match(":"))return r;
                Constraint current;
                current.token=peek();
                do {
                    if(peek().kind==Token::Kind::Identifier&&peek(1).text==":") {
                        if(!current.concepts.empty())r.push_back(std::move(current));
                        current= {
                        };
                        current.token=peek();
                        current.parameter=take().text;
                        take();
                    }
                    auto constraint=qualified();
                    source_types.push_back({tokens[pos-1],constraint,source_owner,source_namespace,source_imports,true});
                    if(is("<"))constraint=generic_name(constraint,type_arguments());
                    current.concepts.push_back(std::move(constraint));
                }
                while(match(","));
                r.push_back(std::move(current));
                return r;
            }
            static int precedence(const std::string& s) {
                if(s==",")return 1;
                if(s=="="||s=="+="||s=="-="||s=="*="||s=="/="||s=="%="||s=="&="||s=="|="||s=="^="||s=="<<="||s==">>="||s=="?" "?=")return 2;
                if(s=="?")return 3;
                if(s=="??")return 4;
                if(s=="||")return 5;
                if(s=="&&")return 6;
                if(s=="|")return 7;
                if(s=="^")return 8;
                if(s=="&")return 9;
                if(s=="=="||s=="!=")return 10;
                if(s=="<"||s==">"||s=="<="||s==">=")return 11;
                if(s=="<<"||s==">>")return 12;
                if(s=="+"||s=="-")return 13;
                if(s=="*"||s=="/"||s=="%")return 14;
                return 0;
            }
            ExprPtr expression(int minimum=1) {
                Guard guard(*this);
                auto e=unary();
                while(peek().kind==Token::Kind::Symbol&&precedence(peek().text)>=minimum) {
                    auto op=take();
                    if(op.text=="?") {
                        auto n=std::make_unique<Expr>(Expr::Kind::Conditional,op,"?:");
                        n->args.push_back(std::move(e));
                        n->args.push_back(expression(2));expect(":");
                        n->args.push_back(expression(3));e=std::move(n);continue;
                    }
                    auto n=std::make_unique<Expr>(precedence(op.text)==2?Expr::Kind::Assign:Expr::Kind::Binary,op,op.text);
                    n->args.push_back(std::move(e));
                    const bool right_associative=precedence(op.text)==2||op.text=="??";
                    n->args.push_back(expression(precedence(op.text)+(right_associative?0:1)));
                    e=std::move(n);
                }
                return e;
            }
            void call_arguments(Expr& e) {
                expect("(");
                if(!is(")"))do {
                    e.args.push_back(expression(2));
                }
                while(match(","));
                expect(")");
            }
            bool generic_value_follows() const {
                // A primary expression after '>' belongs to a comparison such as
                // a < b > c. Delimiters, member access and operators can follow
                // a generic constant (including Constants<T>.Value<U>).
                if(peek().kind==Token::Kind::End)return true;
                if(peek().kind!=Token::Kind::Symbol)return false;
                const auto& next=peek().text;
                return next=="("||next==")"||next=="]"||next=="}"||next==";"||next==","||
                    next=="."||next=="->"||next=="["||next=="++"||next=="--"||precedence(next)>0;
            }
            ExprPtr unary() {
                Guard guard(*this);
                if(lambda_ahead())return lambda_expression();
                if(is("sizeof")) {
                    auto token=take();expect("(");
                    auto result=std::make_unique<Expr>(Expr::Kind::SizeOf,token,type());
                    expect(")");return result;
                }
                // Parentheses are a cast only if a type and a following operand can
                // be recognized. Ambiguous (name)-x remains subtraction; write
                // (Name)(-x) when casting a negative operand to a named/generic type.
                if(is("(")&&peek(1).kind==Token::Kind::Identifier) {
                    const auto saved=checkpoint();
                    auto open=take();
                    std::string cast_type;
                    bool cast=false;
                    try {
                        cast_type=type();
                        const bool closing=match(")");
                        const bool definite=is_primitive(cast_type)||is_pointer(cast_type)||is_reference(cast_type)||is_array(cast_type);
                        const auto& next=peek();
                        const bool value_start=next.kind==Token::Kind::Identifier||next.kind==Token::Kind::Number||next.kind==Token::Kind::String||next.kind==Token::Kind::Character||is("(")||is("!");
                        const bool prefix=is("+")||is("-")||is("*")||is("&")||is("++")||is("--");
                        cast=closing&&(value_start||(definite&&prefix));
                    } catch(const Error&) { }
                    if(cast) {
                        auto result=std::make_unique<Expr>(Expr::Kind::Call,open);
                        result->text="@explicit-cast";
                        result->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,open,cast_type));
                        result->args.push_back(unary());
                        return result;
                    }
                    restore(saved);
                }
                if(is("!")||is("~")||is("await")||is("+")||is("-")||is("++")||is("--")||is("&")||is("*")) {
                    auto t=take();
                    if(t.text=="-"&&peek().kind==Token::Kind::Number&&peek(1).text!="."&&peek(1).text!="++"&&peek(1).text!="--") {
                        auto n=take();
                        n.text="-"+n.text;
                        n.at=t.at;
                        return std::make_unique<Expr>(Expr::Kind::Literal,n,n.text);
                    }
                    auto e=std::make_unique<Expr>(Expr::Kind::Unary,t,t.text);
                    e->args.push_back(unary());
                    return e;
                }
                if(is("new")) {
                    auto t=take();
                    ExprPtr arena;
                    if(match("(")) {
                        arena=expression(2);
                        expect(")");
                    }
                    auto e=std::make_unique<Expr>(Expr::Kind::New,t,type());
                    e->arena=std::move(arena);
                    if(match("[")) {
                        e->arrow=true;
                        do {e->args.push_back(expression(2));}while(match(","));
                        expect("]");
                    } else if(is("("))call_arguments(*e);
                    return e;
                }
                if(is("delete")) {
                    auto e=std::make_unique<Expr>(Expr::Kind::Delete,take());
                    e->args.push_back(unary());
                    return e;
                }
                auto e=primary();
                for(;;) {
                    if(is(".")||(is("->")&&!requirement_expression)) {
                        bool arrow=take().text=="->";
                        auto name=identifier();
                        auto n=std::make_unique<Expr>(Expr::Kind::Member,name,name.text);
                        n->arrow=arrow;
                        n->args.push_back(std::move(e));
                        e=std::move(n);
                    } else if(is("<")&&possible_type_arguments()&&e->type_args.empty()&&(e->kind==Expr::Kind::Name||e->kind==Expr::Kind::Member)) {
                        auto save=checkpoint();
                        try {
                            auto a=type_arguments();
                            if(generic_value_follows()||concept_expression) {
                                e->type_args=std::move(a);
                                continue;
                            }
                        }catch(const Error&) {
                        }
                        restore(save);
                        return e;
                    } else if(is("(")) {
                        auto n=std::make_unique<Expr>(Expr::Kind::Call,e->token);
                        n->args.push_back(std::move(e));
                        call_arguments(*n);
                        e=std::move(n);
                    } else if(match("[")) {
                        auto n=std::make_unique<Expr>(Expr::Kind::Index,peek());
                        n->args.push_back(std::move(e));
                        do {n->args.push_back(expression(2));}while(match(","));
                        expect("]");
                        e=std::move(n);
                    } else if(is("...")) {
                        auto n=std::make_unique<Expr>(Expr::Kind::Unary,take(),"...");
                        n->args.push_back(std::move(e));e=std::move(n);
                    } else if(is("++")||is("--")) {
                        auto t=take();
                        auto n=std::make_unique<Expr>(Expr::Kind::Postfix,t,t.text);
                        n->args.push_back(std::move(e));
                        e=std::move(n);
                    } else return e;
                }
            }
            ExprPtr primary() {
                if(is("default"))return std::make_unique<Expr>(Expr::Kind::Default,take(),"default");
                if(match("(")) {
                    auto e=expression();
                    expect(")");
                    return e;
                }
                if(is("{")||is("[")) {
                    auto t=take();
                    auto e=std::make_unique<Expr>(Expr::Kind::Array,t);
                    auto end=t.text=="{"?"}":"]";
                    if(!is(end))do {
                        e->args.push_back(expression(2));
                    }
                    while(match(",")&&!is(end));
                    expect(end);
                    return e;
                }
                if(peek().kind==Token::Kind::Number||peek().kind==Token::Kind::String||peek().kind==Token::Kind::Character||is("true")||is("false")||is("null")) {
                    auto t=take();
                    return std::make_unique<Expr>(Expr::Kind::Literal,t,t.text);
                }
                if(peek().kind==Token::Kind::Identifier) {
                    auto t=take();
                    return std::make_unique<Expr>(Expr::Kind::Name,t,t.text);
                }
                throw Error(peek().at,"expected an expression, got '"+peek().text+"'", "BLT1100");
            }
            bool lambda_ahead() const {
                if(peek().kind==Token::Kind::Identifier)return peek(1).kind==Token::Kind::Symbol&&peek(1).text=="=>";
                if(!is("("))return false;
                // Parameter types have no nested parentheses. Stop on expression
                // syntax so ordinary grouped expressions stay cheap to parse.
                for(std::size_t i=pos+1;i<tokens.size();++i) {
                    const auto& t=tokens[i];
                    if(t.kind==Token::Kind::Identifier)continue;
                    if(t.kind!=Token::Kind::Symbol)return false;
                    if(t.text==")")return i+1<tokens.size()&&tokens[i+1].kind==Token::Kind::Symbol&&tokens[i+1].text=="=>";
                    if(t.text!=","&&t.text!="."&&t.text!="<"&&t.text!=">"&&t.text!=">>"&&t.text!="["&&t.text!="]"&&t.text!="?"&&t.text!="*"&&t.text!="&")return false;
                }
                return false;
            }
            ExprPtr lambda_expression() {
                auto e=std::make_unique<Expr>(Expr::Kind::Lambda,peek());
                auto& lambda=*(e->lambda=std::make_unique<LambdaSyntax>());
                if(match("(")) {
                    if(!is(")"))do {
                        std::string annotation;
                        if(peek(1).text!=","&&peek(1).text!=")")annotation=type();
                        lambda.parameters.push_back({identifier(),annotation});
                    }while(match(","));
                    expect(")");
                }else lambda.parameters.push_back({identifier(),{}});
                expect("=>");
                if(is("{"))lambda.body=statement();
                else {
                    lambda.expression_body=true;
                    lambda.body=std::make_unique<Stmt>(Stmt::Kind::Return,peek());
                    lambda.body->expression=expression(2);
                }
                return e;
            }
            bool variable_ahead() {
                if(peek().kind!=Token::Kind::Identifier||is("await")||is("delete")||is("new"))return false;
                if(is("const")||is("var"))return true;
                const auto& next=peek(1);
                if(next.kind!=Token::Kind::Identifier && next.text!="." && next.text!="<" &&
                   next.text!="*" && next.text!="&" && next.text!="?" && next.text!="[")return false;
                auto save=checkpoint();
                bool result=false;
                try {
                    type();
                    if(peek().kind==Token::Kind::Identifier) {
                        identifier();
                        result=is(";")||is("=")||is("(")||is(",");
                    }
                }catch(const Error&) {
                }
                restore(save);
                return result;
            }
            StmtPtr variable() {
                const auto start=peek();
                const bool constant=match("const");
                const bool inferred=match("var");
                if(inferred&&(is("<")||is("[")||is("*")||is("&")||is(".")))
                    throw Error(peek().at,"'var' cannot have type suffixes", "BLT1100");
                const auto declared=inferred?std::string("var"):type();
                auto group=std::make_unique<Stmt>(Stmt::Kind::Declarations,start);
                do {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::Variable,start);
                    s->constant=constant;s->type=declared;s->name_token=identifier();s->name=s->name_token.text;
                    if(match("="))s->expression=expression(2);
                    else if(inferred)throw Error(s->name_token.at,"'var' declarations require an '=' initializer", "BLT1100");
                    else if(is("(")) {
                        auto e=std::make_unique<Expr>(Expr::Kind::Call,peek());
                        e->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,s->token,s->type));
                        call_arguments(*e);s->expression=std::move(e);
                    }
                    group->statements.push_back(std::move(s));
                    if(inferred&&is(","))throw Error(peek().at,"use an explicit type for comma-separated declarations", "BLT1100");
                }while(match(","));
                expect(";");
                if(group->statements.size()==1)return std::move(group->statements.front());
                return group;
            }
            StmtPtr if_statement(Token t) {
                Guard g(*this);
                auto s=std::make_unique<Stmt>(Stmt::Kind::If,t);
                expect("(");
                s->condition=expression();
                expect(")");
                s->body=statement();
                if(is("elif"))s->alternative=if_statement(take());
                else if(match("else"))s->alternative=statement();
                return s;
            }
            StmtPtr statement() {
                Guard g(*this);
                auto t=peek();
                if(match(";"))return std::make_unique<Stmt>(Stmt::Kind::Empty,t);
                if(match("{")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::Block,t);
                    while(!is("}")) {
                        if(peek().kind==Token::Kind::End)throw Error(peek().at,"expected '}'", "BLT1101");
                        s->statements.push_back(statement());
                    }
                    expect("}");
                    return s;
                }
                if(match("if"))return if_statement(t);
                if(match("switch")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::Switch,t);
                    expect("(");s->expression=expression();expect(")");expect("{");
                    bool has_default=false;
                    while(!match("}")) {
                        auto arm=std::make_unique<Stmt>(Stmt::Kind::Case,peek());
                        arm->body=std::make_unique<Stmt>(Stmt::Kind::Block,peek());
                        do {
                            if(match("case")) {
                                auto label=std::make_unique<Stmt>(Stmt::Kind::Expression,peek());
                                label->expression=expression(2);arm->statements.push_back(std::move(label));
                            }else if(match("default")) {
                                if(has_default)throw Error(arm->token.at,"duplicate default label","BLT1100");
                                has_default=true;arm->name="default";
                            }else throw Error(peek().at,"expected case or default label","BLT1100");
                            expect(":");
                        }while(is("case")||(is("default")&&peek(1).text==":"));
                        while(!is("}")&&!is("case")&&!(is("default")&&peek(1).text==":"))
                            arm->body->statements.push_back(statement());
                        s->statements.push_back(std::move(arm));
                    }
                    return s;
                }
                if(match("while")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::While,t);
                    expect("(");
                    s->condition=expression();
                    expect(")");
                    s->body=statement();
                    return s;
                }
                if(is("for")) {
                    auto saved=checkpoint();take();expect("(");
                    auto range=std::make_unique<Stmt>(Stmt::Kind::ForEach,t);
                    bool is_range=false;
                    try {
                        range->constant=match("const");
                        if(peek().kind==Token::Kind::Identifier&&peek(1).text==":") {
                            range->type="var";range->name_token=identifier();
                        } else {
                            range->type=match("var")?"var":type();range->name_token=identifier();
                        }
                        range->name=range->name_token.text;is_range=match(":");
                    }catch(const Error&){}
                    if(is_range) {
                        range->expression=expression();expect(")");range->body=statement();return range;
                    }
                    restore(saved);
                }
                if(match("try")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::Try,t);
                    if(!is("{"))throw Error(peek().at,"try requires a braced block","BLT1100");
                    s->body=statement();bool catch_all=false;
                    while(match("catch")) {
                        if(catch_all)throw Error(peek().at,"a catch-all must be the last catch clause","BLT1100");
                        auto handler=std::make_unique<Stmt>(Stmt::Kind::Catch,peek());
                        if(match("(")) {handler->type=type();handler->name_token=identifier();handler->name=handler->name_token.text;expect(")");}
                        else catch_all=true;
                        if(!is("{"))throw Error(peek().at,"catch requires a braced block","BLT1100");
                        handler->body=statement();s->statements.push_back(std::move(handler));
                    }
                    if(match("finally")) {if(!is("{"))throw Error(peek().at,"finally requires a braced block","BLT1100");s->alternative=statement();}
                    if(s->statements.empty()&&!s->alternative)throw Error(t.at,"try needs catch or finally","BLT1100");
                    return s;
                }
                if(match("throw")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::Throw,t);
                    if(!is(";"))s->expression=expression();expect(";");return s;
                }
                if(match("for")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::For,t);
                    expect("(");
                    if(variable_ahead())s->init=variable();
                    else if(!match(";")) {
                        s->init=std::make_unique<Stmt>(Stmt::Kind::Expression,peek());
                        s->init->expression=expression();
                        expect(";");
                    }
                    if(!is(";"))s->condition=expression();
                    expect(";");
                    if(!is(")"))s->increment=expression();
                    expect(")");
                    s->body=statement();
                    return s;
                }
                if(match("return")) {
                    auto s=std::make_unique<Stmt>(Stmt::Kind::Return,t);
                    if(!is(";"))s->expression=expression();
                    expect(";");
                    return s;
                }
                if(is("break")||is("continue")) {
                    auto s=std::make_unique<Stmt>(take().text=="break"?Stmt::Kind::Break:Stmt::Kind::Continue,t);
                    expect(";");
                    return s;
                }
                if(variable_ahead())return variable();
                auto s=std::make_unique<Stmt>(Stmt::Kind::Expression,t);
                s->expression=expression();
                expect(";");
                return s;
            }
            struct Modifiers {
                bool access=false,protected_access=false,stat=false,constant=false,sealed=false,virt=false,over=false,abstract=false,async=false,conversion=false,implicit=false,event=false;
                std::vector<Attribute> attributes;
            };
            Modifiers modifiers(bool access=false,bool stat=false,bool protected_access=false) {
                Modifiers m;
                while(match("[")) {
                    do {Attribute a;a.token=peek();a.name=qualified();
                        if(match("(")){if(!is(")"))do {
                            if(peek().kind!=Token::Kind::String&&peek().kind!=Token::Kind::Number&&peek().kind!=Token::Kind::Identifier)throw Error(peek().at,"attribute arguments must be literal values","BLT1100");
                            if(is("typeof") && peek(1).text=="(") {
                                take(); expect("(");
                                auto argument=type(); expect(")");
                                a.arguments.push_back(std::move(argument));
                            } else if(peek().kind==Token::Kind::Identifier && peek(1).text==".") a.arguments.push_back(qualified());
                            else a.arguments.push_back(take().text);
                        }while(match(","));expect(")");}
                        m.attributes.push_back(std::move(a));
                    }while(match(","));expect("]");
                }
                m.access=access;
                m.protected_access=protected_access;
                m.stat=stat;
                std::unordered_set<std::string> seen;
                std::string access_modifier;
                while(is("event")||is("implicit")||is("explicit")||is("public")||is("private")||is("protected")||is("static")||is("const")||is("sealed")||is("virtual")||is("override")||is("abstract")||is("async")) {
                    auto t=take();
                    if(!seen.insert(t.text).second)throw Error(t.at,"duplicate modifier '"+t.text+"'", "BLT1100");
                    if(t.text=="public"||t.text=="private"||t.text=="protected") {
                        if(!access_modifier.empty())throw Error(t.at,"a declaration can have only one access modifier", "BLT1100");
                        access_modifier=t.text;
                    }
                    if(t.text=="event")m.event=true;
                    else if(t.text=="implicit"||t.text=="explicit") {
                        if(m.conversion)throw Error(t.at,"a conversion can be implicit OR explicit, not both", "BLT2600");
                        m.conversion=true;m.implicit=t.text=="implicit";
                    }
                    else if(t.text=="public"){m.access=true;m.protected_access=false;}
                    else if(t.text=="private"){m.access=false;m.protected_access=false;}
                    else if(t.text=="protected"){m.access=false;m.protected_access=true;}
                    else if(t.text=="static")m.stat=true;
                    else if(t.text=="const")m.constant=true;
                    else if(t.text=="sealed")m.sealed=true;
                    else if(t.text=="virtual")m.virt=true;
                    else if(t.text=="abstract")m.abstract=true;
                    else if(t.text=="async")m.async=true;
                    else m.over=true;
                }
                return m;
            }
            Method method(const std::string& return_type,Token name,Modifiers mod,bool constructor=false,bool declaration=false) {
                Method m;
                m.type=return_type;
                if(mod.event)throw Error(name.at,"event declares a subscription member, not a method", "BLT2700");
                if(mod.conversion&&!constructor&&!name.text.starts_with("@convert."))throw Error(name.at,"implicit/explicit can only modify conversion members or constructors", "BLT2600");
                m.name=name.text;
                m.token=name;
                m.is_public=mod.access;
                m.is_protected=mod.protected_access;
                m.is_static=mod.stat;
                m.is_constructor=constructor;
                m.is_implicit=constructor&&mod.conversion&&mod.implicit;
                for(const auto& a:mod.attributes)if(a.name=="ReadOnly")m.readonly_receiver=true;
                m.is_sealed=mod.sealed;
                m.is_virtual=mod.virt;
                m.is_override=mod.over;
                m.is_abstract=mod.abstract;m.is_async=mod.async;m.attributes=mod.attributes;
                for(const auto& a:m.attributes){if(a.name=="Native"){m.is_extern=true;if(!a.arguments.empty())m.native_name=a.arguments[0];}
                    if(a.name=="NativePack"){m.native_pack=true;if(!a.arguments.empty()){try{m.native_pack_min=std::stoul(a.arguments[0]);}catch(...){throw Error(a.token.at,"NativePack minimum must be a non-negative integer","BLT1100");}}}}
                if(mod.constant)throw Error(name.at,"const applies to values, not method declarations", "BLT1100");
                generic_parameters(m.generic_parameters,m.pack_parameter);
                expect("(");
                if(!is(")"))do {
                    Parameter p;
                    p.token=peek();
                    p.is_this=match("this");
                    if(p.is_this&&(!m.parameters.empty()||is("this")))throw Error(p.token.at,"this is only allowed once, on the first extension receiver parameter", "BLT2011");
                    if(p.is_this&&(!m.is_static||constructor))throw Error(p.token.at,"extension methods must be static or top-level functions", "BLT2011");
                    m.is_extension|=p.is_this;
                    p.constant=match("const");
                    p.type=type();
                    p.variadic=match("...");
                    if(p.variadic) {
                        auto uses=[&](const std::string& name) {
                            if(name.empty())return false;
                            auto identifier=[](unsigned char c){return std::isalnum(c)||c=='_';};
                            for(std::size_t i=0;(i=p.type.find(name,i))!=std::string::npos;++i)
                                if((i==0||!identifier(p.type[i-1]))&&(i+name.size()==p.type.size()||!identifier(p.type[i+name.size()])))return true;
                            return false;
                        };
                        p.pack=uses(m.pack_parameter)||(std::find(m.generic_parameters.begin(),m.generic_parameters.end(),class_pack)==m.generic_parameters.end()&&uses(class_pack));
                        if(p.pack)p.variadic=false;
                    }
                    p.name_token=identifier();
                    p.name=p.name_token.text;
                    if(match("="))p.default_value=expression(2);
                    if(p.is_this&&(p.default_value||p.variadic||p.pack))throw Error(p.token.at,"an extension receiver cannot be optional or variadic", "BLT2011");
                    if(p.pack&&(p.default_value||!is(")")))throw Error(p.token.at,"a value pack must be last and cannot have a default value","BLT1100");
                    if(p.variadic){
                        if(is_reference(p.type))throw Error(p.token.at,"variadic values cannot have a reference element type; use pointer elements for borrowed objects","BLT1100");
                        if(p.default_value)throw Error(p.token.at,"a variadic parameter cannot have a default value","BLT1100");
                        p.type+="[]";
                        if(!is(")"))throw Error(peek().at,"the variadic parameter must be last","BLT1100");
                    }
                    if(!m.parameters.empty()&&m.parameters.back().default_value&&!p.default_value&&!p.variadic&&!p.pack)throw Error(p.token.at,"required parameters must come before default parameters","BLT1100");
                    m.parameters.push_back(std::move(p));
                }
                while(match(","));
                expect(")");
                if(constructor&&m.is_implicit) {
                    if(m.parameters.size()!=1||m.parameters[0].default_value||m.parameters[0].variadic||m.parameters[0].pack)
                        throw Error(name.at,"an implicit constructor must take exactly one required value parameter", "BLT2600");
                }
                if(constructor&&match(":")) {
                    do {
                        auto name=identifier();
                        auto initializer=std::make_unique<Expr>(Expr::Kind::Call,name,name.text);
                        call_arguments(*initializer);
                        m.constructor_initializers.push_back(std::move(initializer));
                    } while(match(","));
                } else if(!constructor)m.constraints=constraints();
                if((declaration&&is(";"))||mod.abstract||m.is_extern) {
                    expect(";");
                    m.is_abstract=(declaration||mod.abstract)&&!m.is_extern;
                    return m;
                }
                if(declaration)m.is_default_implementation=true;
                if(!is("{"))throw Error(peek().at,"expected method body or interface contract ';'", "BLT1100");
                m.body=statement();
                return m;
            }
            void property(Class& c,const std::string& value_type,Token name,Modifiers mod,bool indexer=false) {
                if(mod.event||mod.constant||mod.async)throw Error(name.at,"properties cannot be event, const or async; use a getter or an async method", "BLT2250");
                if(c.is_native)throw Error(name.at,"native declarations use NativeProperty; Bolt accessors require a Bolt-defined type", "BLT2250");
                Property p;p.type=value_type;p.name=indexer?"@indexer":name.text;p.token=name;
                p.is_indexer=indexer;p.is_static=mod.stat;p.is_public=mod.access;p.is_protected=mod.protected_access;p.attributes=mod.attributes;
                if(indexer){
                    if(mod.stat)throw Error(name.at,"indexers are instance properties", "BLT2250");
                    expect("[");
                    do {Parameter a;a.token=peek();a.constant=match("const");a.type=type();a.name_token=identifier();a.name=a.name_token.text;
                        if(a.name=="value")throw Error(a.name_token.at,"'value' is reserved for the indexer setter argument", "BLT2250");
                        p.parameters.push_back(std::move(a));
                    }while(match(","));expect("]");
                }
                std::vector<Method> accessors;
                bool auto_get=false,auto_set=false;
                auto accessor=[&](Token token,bool get,bool public_access,bool protected_access,bool automatic,StmtPtr body){
                    Method m;m.name=(get?"@get.":"@set.")+p.name;m.token=token;
                    m.type=get?value_type:"void";m.property_name=p.name;m.is_getter=get;m.is_setter=!get;
                    m.is_public=public_access;m.is_protected=protected_access;m.is_static=mod.stat;m.is_virtual=mod.virt;m.is_override=mod.over;m.is_sealed=mod.sealed;
                    m.is_abstract=c.is_interface||mod.abstract;m.parameters=p.parameters;m.attributes=mod.attributes;
                    if(!get){Parameter v;v.name="value";v.type=value_type;v.token=token;v.name_token=token;v.name_token.text="value";m.parameters.push_back(std::move(v));}
                    m.body=std::move(body);
                    m.readonly_receiver=get&&!mod.stat&&!is_reference(value_type)&&((automatic&&!c.is_interface&&!mod.abstract)||std::any_of(mod.attributes.begin(),mod.attributes.end(),[](const Attribute& a){return a.name=="ReadOnly";}));
                    if(get){if(!p.getter.empty())throw Error(token.at,"duplicate get accessor", "BLT2250");p.getter=m.name;auto_get=automatic;}
                    else {if(!p.setter.empty())throw Error(token.at,"duplicate set accessor", "BLT2250");p.setter=m.name;auto_set=automatic;}
                    accessors.push_back(std::move(m));
                };
                if(match("=>")) {
                    auto body=std::make_unique<Stmt>(Stmt::Kind::Block,name);
                    auto ret=std::make_unique<Stmt>(Stmt::Kind::Return,name);ret->expression=expression(2);expect(";");body->statements.push_back(std::move(ret));
                    if(c.is_interface||mod.abstract)throw Error(name.at,"abstract properties cannot have an expression body", "BLT2250");
                    accessor(name,true,mod.access,mod.protected_access,false,std::move(body));
                }else {
                    expect("{");
                    while(!is("}")) {
                        bool access=mod.access,protected_access=mod.protected_access;bool explicit_access=false;
                        if(is("private")||is("public")||is("protected")){
                            explicit_access=true;const auto modifier=take().text;
                            access=modifier=="public";protected_access=modifier=="protected";
                            if((c.is_struct||c.is_static)&&protected_access)throw Error(peek().at,c.is_struct?"struct members cannot be protected because structs do not participate in class inheritance":"static classes cannot declare protected accessors", "BLT2000");
                        }
                        auto token=peek();bool get=match("get");
                        if(!get&&!match("set"))throw Error(peek().at,"expected 'get' or 'set' accessor", "BLT2250");
                        const auto rank=[](bool pub,bool prot){return pub?2:prot?1:0;};
                        if(explicit_access&&rank(access,protected_access)>=rank(mod.access,mod.protected_access))throw Error(token.at,"an accessor modifier must be more restrictive than its property", "BLT2250");
                        if(c.is_interface&&explicit_access)throw Error(token.at,"interface accessors must be public", "BLT2250");
                        bool automatic=match(";");StmtPtr body;
                        if(!automatic) {
                            if(c.is_interface||mod.abstract)throw Error(token.at,"abstract property accessors end with ';'", "BLT2250");
                            if(match("=>")){
                                body=std::make_unique<Stmt>(Stmt::Kind::Block,token);
                                auto stmt=std::make_unique<Stmt>(get?Stmt::Kind::Return:Stmt::Kind::Expression,token);stmt->expression=expression(2);expect(";");body->statements.push_back(std::move(stmt));
                            }else {if(!is("{"))throw Error(peek().at,"expected an accessor body or ';'", "BLT2250");body=statement();}
                        }
                        accessor(token,get,access,protected_access,automatic,std::move(body));
                    }expect("}");
                }
                if(accessors.empty())throw Error(name.at,"a property needs at least one accessor", "BLT2250");
                if(is_reference(value_type)&&!p.setter.empty())throw Error(name.at,"a ref-return property cannot also have a setter", "BLT2250");
                const bool automatic=auto_get||auto_set;
                if(automatic&&!c.is_interface&&!mod.abstract) {
                    if(indexer||is_reference(value_type))throw Error(name.at,"indexers and ref-return properties require explicit accessor bodies", "BLT2250");
                    if((!p.getter.empty()&&!auto_get)||(!p.setter.empty()&&!auto_set)||p.getter.empty())
                        throw Error(name.at,"auto-properties require get; and optional set; (do not mix automatic and explicit bodies)", "BLT2250");
                    p.backing_field="@property."+p.name;
                    Field f;f.type=value_type;f.name=p.backing_field;f.token=name;f.is_static=mod.stat;
                    if(match("=")){f.initializer=expression(2);expect(";");}
                    c.fields.push_back(std::move(f));
                    for(auto& m:accessors) {
                        m.body=std::make_unique<Stmt>(Stmt::Kind::Block,name);
                        auto stmt=std::make_unique<Stmt>(m.is_getter?Stmt::Kind::Return:Stmt::Kind::Expression,name);
                        auto storage=std::make_unique<Expr>(Expr::Kind::Name,name,p.backing_field);
                        if(m.is_getter)stmt->expression=std::move(storage);
                        else {auto assignment=std::make_unique<Expr>(Expr::Kind::Assign,name,"=");assignment->args.push_back(std::move(storage));assignment->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,name,"value"));stmt->expression=std::move(assignment);}
                        m.body->statements.push_back(std::move(stmt));
                    }
                }else if(is("="))throw Error(peek().at,"only auto-properties can have a property initializer", "BLT2250");
                for(auto& m:accessors)c.methods.push_back(std::move(m));
                c.properties.push_back(std::move(p));
            }
            Method constant_template(const std::string& value_type,Token name,Modifiers mod) {
                if(!mod.stat)throw Error(name.at,"generic constants must be static", "BLT1100");
                if(mod.sealed||mod.virt||mod.over)throw Error(name.at,"generic constants cannot use method modifiers", "BLT1100");
                Method m;
                m.type=value_type;
                m.name=name.text;
                m.token=name;
                m.is_public=mod.access;
                m.is_protected=mod.protected_access;
                m.is_static=true;
                m.is_constant_template=true;
                generic_parameters(m.generic_parameters,m.pack_parameter);
                if(m.generic_parameters.empty()||!m.pack_parameter.empty())throw Error(name.at,"generic constants require fixed type parameters", "BLT1100");
                m.constraints=constraints();
                expect("=");
                auto value=std::make_unique<Stmt>(Stmt::Kind::Return,name);
                value->expression=expression(2);
                expect(";");
                m.body=std::make_unique<Stmt>(Stmt::Kind::Block,name);
                m.body->statements.push_back(std::move(value));
                return m;
            }
            void enum_declaration(Program& p,const Modifiers& mod,const std::string& ns,const std::vector<std::string>& imports,const std::string& owner,bool dependent=false) {
                if(mod.protected_access)throw Error(peek().at,"protected is only valid on members, not type declarations", "BLT2000");
                Enum e;e.attributes=mod.attributes;
                for(const auto& a:mod.attributes)if(a.name=="Native"){if(a.arguments.size()!=1)throw Error(a.token.at,"Native enum requires a C++ name","BLT1100");e.native_cpp=a.arguments[0];}
                e.token=identifier();
                e.short_name=e.token.text;
                e.name=(owner.empty()?(ns.empty()?"":ns+"."):owner+".")+e.short_name;
                e.declaring_type=owner;e.is_public=mod.access;e.is_template_member=dependent;
                e.namespace_name=ns;e.imports=imports;
                if(match(":"))e.underlying=type();
                expect("{");
                if(!is("}"))do {
                    EnumMember m;
                    m.token=peek();
                    m.name=identifier().text;
                    if(match("="))m.expression=expression(2);
                    e.members.push_back(std::move(m));
                }
                while(match(",")&&!is("}"));
                expect("}");
                match(";");
                p.enums.push_back(std::move(e));
            }
            void type_declaration(Program& p,const Modifiers& mod,const std::string& ns,const std::vector<std::string>& imports,const std::string& owner,bool dependent=false) {
                Guard guard(*this);const auto start=peek();
                if(mod.conversion||mod.event)throw Error(start.at,"event and conversion modifiers apply to members, not types", "BLT2600");
                if(mod.protected_access)throw Error(start.at,"protected is only valid on members, not type declarations", "BLT2000");
                const auto kind=take().text;
                const bool interface=kind=="interface";
                Class c;
                c.is_interface=interface;
                c.is_struct=kind=="struct";c.is_abstract=mod.abstract;c.attributes=mod.attributes;
                for(const auto& a:c.attributes){if(a.name=="Reflect")c.is_reflected=true;
                    if(a.name=="Native"){c.is_native=true;if(a.arguments.size()!=1)throw Error(a.token.at,"Native type requires one C++ type name","BLT1100");c.native_cpp=a.arguments[0];}}
                if(interface&&(mod.stat||mod.sealed||mod.constant||mod.virt||mod.over))throw Error(start.at,"interfaces cannot be static, const, sealed, virtual, or override", "BLT2200");
                c.token=identifier();
                c.short_name=c.token.text;
                c.name=(owner.empty()?(ns.empty()?"":ns+"."):owner+".")+c.short_name;
                c.declaring_type=owner;c.is_public=mod.access;c.is_template_member=dependent;
                const auto previous_owner=source_owner,previous_pack=class_pack;
                source_owner=c.name;
                c.namespace_name=ns;
                c.imports=imports;
                c.is_static=mod.stat;
                c.is_sealed=mod.sealed;
                generic_parameters(c.generic_parameters,c.pack_parameter,&c.generic_defaults);
                class_pack=c.pack_parameter;
                if(match(":")) {
                    do { c.interfaces.push_back(type()); } while(match(","));
                }
                expect("{");
                bool access=interface,protected_access=false;
                while(!is("}")) {
                    if(peek().kind==Token::Kind::End)throw Error(peek().at,"expected '}'", "BLT1101");
                    if((is("public")||is("private")||is("protected"))&&peek(1).text==":") {
                        const auto modifier=take().text;access=modifier=="public";protected_access=modifier=="protected";
                        take();
                        continue;
                    }
                    auto member=modifiers(access,false,protected_access);
                    if(member.protected_access&&(c.is_struct||c.is_static))throw Error(peek().at,c.is_struct?"struct members cannot be protected because structs do not participate in class inheritance":"static classes cannot declare protected members", "BLT2000");
                    if(match("enum")) {enum_declaration(p,member,ns,imports,c.name,c.is_template_member||!c.generic_parameters.empty()||!c.pack_parameter.empty());continue;}
                    if(is("class")||is("struct")||is("interface")) {
                        type_declaration(p,member,ns,imports,c.name,c.is_template_member||!c.generic_parameters.empty()||!c.pack_parameter.empty());continue;
                    }
                    member.stat=member.stat||c.is_static;
                    if(c.is_native)member.attributes.push_back(Attribute{"Native",{},peek()});
                    if(match("~")) {
                        auto name=identifier();if(name.text!=c.short_name)throw Error(name.at,"destructor name must match its type","BLT1100");
                        if(interface||c.is_static||member.stat)throw Error(name.at,"destructors need an instance class or struct","BLT1100");
                        name.text="~"+name.text;auto m=method("void",name,member);m.is_destructor=true;
                        if(!m.parameters.empty()||!m.generic_parameters.empty()||!m.pack_parameter.empty())throw Error(name.at,"destructors take no parameters","BLT1100");
                        c.methods.push_back(std::move(m));continue;
                    }
                    if(interface&&(!member.access||member.protected_access||member.stat||member.constant||member.sealed||member.over))throw Error(peek().at,"interface members must be public instance method signatures", "BLT2200");
                    if(peek().text==c.short_name&&peek(1).text=="(") {
                        auto name=take();
                        if(interface)throw Error(name.at,"an interface cannot declare a constructor", "BLT2200");
                        if(member.stat||member.sealed||member.virt||member.over)throw Error(name.at,"constructor cannot be static, sealed, virtual, or override", "BLT1100");
                        c.methods.push_back(method("void",name,member,true));
                        continue;
                    }
                    auto conversion_token=peek();
                    auto t=type();
                    if(is("(")) {
                        if(c.is_static||member.stat||member.constant||member.async)
                            throw Error(conversion_token.at,"conversions must be non-static instance members", "BLT2600");
                        if(t==c.short_name||t==c.name)throw Error(conversion_token.at,"a conversion cannot target its own type", "BLT2600");
                        auto name=conversion_token;name.text="@convert."+t;
                        auto m=method(t,name,member,false,interface);
                        m.is_conversion=true;m.is_implicit=member.conversion&&member.implicit;
                        if(!m.parameters.empty()||!m.generic_parameters.empty()||!m.pack_parameter.empty())
                            throw Error(conversion_token.at,"conversion declarations have no parameters or method type parameters", "BLT2600");
                        c.methods.push_back(std::move(m));continue;
                    }
                    if(member.conversion)throw Error(conversion_token.at,"implicit/explicit must precede a conversion such as 'explicit string()'", "BLT2600");
                    if(match("operator")) {
                        if(interface)throw Error(peek().at,"operators are declared by concrete classes, not interfaces", "BLT2210");
                        auto symbol=take();
                        static const std::unordered_set<std::string> operators={"+","-","*","/","%","==","!=","<",">","<=",">=","!","~","&","|","^","<<",">>","++","--"};
                        if(symbol.kind!=Token::Kind::Symbol||!operators.contains(symbol.text))throw Error(symbol.at,"unsupported overloadable operator", "BLT2210");
                        auto name=symbol; name.text="operator"+symbol.text;
                        member.stat=true;
                        auto m=method(t,name,member); m.operator_symbol=symbol.text;
                        c.methods.push_back(std::move(m));
                        continue;
                    }
                    if(member.event&&(is("this")||peek(1).text=="{"||peek(1).text=="=>"))throw Error(peek().at,"event declares subscriber storage, not a property or indexer", "BLT2700");
                    if(is("this")){auto name=take();if(!is("["))throw Error(name.at,"expected an identifier; this is reserved for the current instance and cannot name a field", "BLT1100");property(c,t,name,member,true);continue;}
                    auto name=identifier();
                    if(is("{")||is("=>")){property(c,t,name,member);continue;}
                    if(member.constant&&is("<"))c.methods.push_back(constant_template(t,name,member));
                    else if(is("(")||is("<"))c.methods.push_back(method(t,name,member,false,interface));
                    else {
                        if(interface)throw Error(name.at,"interfaces contain method signatures, not fields", "BLT2200");
                        if(member.sealed||member.virt||member.over)throw Error(name.at,"method modifier on a field", "BLT1100");
                        do {
                        Field f;
                        f.type=t;
                        f.name=name.text;
                        f.token=name;
                        f.is_public=member.access;
                        f.is_protected=member.protected_access;
                        f.is_static=member.stat;
                        f.constant=member.constant;f.attributes=member.attributes;f.is_event=member.event;
                        if(f.is_event&&(member.constant||c.is_native))throw Error(name.at,"events cannot be const or native fields", "BLT2700");
                        if(f.is_event&&is("="))throw Error(name.at,"initialize an event by subscribing with +=, not a field initializer", "BLT2700");
                        for(const auto& a:f.attributes)if(a.name=="NativeProperty")f.native_getter=a.arguments.empty()?f.name:a.arguments[0];
                        if(match("="))f.initializer=expression(2);
                        c.fields.push_back(std::move(f));
                        if(!match(","))break;
                        name=identifier();
                        }while(true);
                        expect(";");
                    }
                }
                expect("}");
                match(";");
                if(c.is_template_member||!c.generic_parameters.empty()||!c.pack_parameter.empty())
                    for(const auto& m:c.methods)if(m.is_extension)throw Error(m.token.at,"extension methods require a non-generic declaring type; use method type parameters instead", "BLT2011");
                p.classes.push_back(std::move(c));
                source_owner=previous_owner;class_pack=previous_pack;
            }
            public: explicit Parser(std::vector<Token> t):tokens(std::move(t)) {
            }
            Program program() {
                Program p;
                std::vector<std::string> imports;
                std::string ns;
                while(match("import")) {
                    auto t=peek();
                    bool path=t.kind==Token::Kind::String;
                    auto name=path?take().text:qualified();
                    p.imports.push_back( {
                        name,t,path
                    });
                    imports.push_back(name);
                    match(";");
                }
                if(match("namespace")) {
                    ns=qualified();
                    expect(";");
                }
                source_namespace=ns;source_imports=imports;
                auto full=[&](const std::string& n) {
                    return ns.empty()?n:ns+"."+n;
                };
                while(peek().kind!=Token::Kind::End) {
                    source_owner.clear();
                    auto start=peek();
                    auto mod=modifiers(true);
                    if(mod.protected_access)throw Error(start.at,"protected is only valid on members of a class", "BLT2000");
                    if(match("enum")) {enum_declaration(p,mod,ns,imports,{});continue;}
                    if(match("concept")) {
                        Concept c;
                        c.token=identifier();
                        c.name=full(c.token.text);
                        c.namespace_name=ns;
                        c.imports=imports;
                        expect("<");
                        do {c.parameters.push_back(identifier().text);}while(match(","));expect(">");
                        c.parameter=c.parameters.front();
                        if(match("=")) {
                            concept_expression=true;c.expression=expression(2);concept_expression=false;expect(";");
                        } else {
                            expect("{");
                            while(!is("}")) {
                                expect("require");expect("(");std::vector<Parameter> params;
                                if(!is(")"))do {Parameter p;p.token=peek();p.constant=match("const");p.type=type();p.name_token=identifier();p.name=p.name_token.text;params.push_back(std::move(p));}while(match(","));
                                expect(")");expect("{");
                                while(!is("}")) {Requirement r;r.token=peek();r.parameters=params;
                                    requirement_expression=true;r.expression=expression(2);requirement_expression=false;
                                    if(match("->"))r.result_type=type();expect(";");c.requirements.push_back(std::move(r));
                                }expect("}");
                            }expect("}");match(";");
                            if(c.requirements.empty())throw Error(c.token.at,"a structural concept must contain at least one requirement","BLT1100");
                        }
                        p.concepts.push_back(std::move(c));
                        continue;
                    }
                    if(is("class")||is("interface")||is("struct")) {type_declaration(p,mod,ns,imports,{});continue;}
                    if(is("namespace")||is("import"))throw Error(peek().at,"imports must precede the single file-scoped namespace and declarations", "BLT1100");
                    std::string abi;
                    if(match("extern")) {
                        if(peek().kind!=Token::Kind::String)throw Error(peek().at,"extern requires \"C\" or \"C++\"","BLT1100");
                        abi=take().text;if(abi!="C"&&abi!="C++")throw Error(start.at,"unsupported native linkage","BLT1100");
                        mod.attributes.push_back(Attribute{"Native",{},start});
                    }
                    const auto t=type();
                    auto name=identifier();
                    mod.stat=true;
                    mod.access=true;
                    auto global_name="@Global"+(ns.empty()?"":"."+ns);
                    auto* global=p.find_class(global_name);
                    if(!global) {
                        Class c;
                        c.name=global_name;
                        c.short_name="@Global";
                        c.namespace_name=ns;
                        c.imports=imports;
                        c.token=start;
                        c.is_static=true;
                        c.is_global=true;
                        p.classes.push_back(std::move(c));
                        global=&p.classes.back();
                    }
                    if(mod.constant&&is("<"))global->methods.push_back(constant_template(t,name,mod));
                    else {auto m=method(t,name,mod);m.native_abi=abi;global->methods.push_back(std::move(m));}
                }
                p.source_types=std::move(source_types);
                return p;
            }
        };
        void merge(Program& out,Program p) {
            for(auto& type:p.source_types)out.source_types.push_back(std::move(type));
            for(auto& [name,source]:p.sources)out.sources.emplace(name,std::move(source));
            for(auto& [name,source]:p.active_sources)out.active_sources.emplace(name,std::move(source));
            for(auto& [name,options]:p.source_preprocessors)out.source_preprocessors.emplace(name,std::move(options));
            for(auto& c:p.classes) {
                if(c.is_global) {
                    if(auto* old=out.find_class(c.name)) {
                        for(auto& m:c.methods)old->methods.push_back(std::move(m));
                        for(const auto& import:c.imports)if(std::find(old->imports.begin(),old->imports.end(),import)==old->imports.end())old->imports.push_back(import);
                        continue;
                    }
                }
                out.classes.push_back(std::move(c));
            }
            for(auto& e:p.enums)out.enums.push_back(std::move(e));
            for(auto& c:p.concepts)out.concepts.push_back(std::move(c));
            for(auto& i:p.imports)out.imports.push_back(std::move(i));
        }
    }
    // namespace
    static Program parse_source(std::string_view source,const std::string& filename,
                                const PreprocessorOptions& preprocessing,bool validation_only) {
        try {
            auto* cache=current_parse_cache();
            std::string configuration;
            if(cache) {
                configuration=preprocessor_identity(preprocessing);
                if(const auto* tree=cache->lookup(filename,source,configuration))
                    return validation_only?Program{}:copy_syntax_tree(*tree);
            }
            auto processed = preprocess(source,filename,preprocessing);
            auto result = Parser(tokenize(processed.text,filename)).program();
            result.preprocessor = normalized_preprocessor_options(preprocessing);
            result.source_preprocessors.emplace(filename,result.preprocessor);
            result.active_sources.emplace(filename,std::move(processed.text));
            result.sources.emplace(filename, source);
            if(cache){
                const auto& stored=cache->save(filename,std::move(configuration),std::move(result));
                return validation_only?Program{}:copy_syntax_tree(stored);
            }
            return result;
        } catch(Error& error) { attach_source(error.diagnostic,source); throw; }
    }
    Program parse(std::string_view source,const std::string& filename,const PreprocessorOptions& preprocessing) {
        return parse_source(source,filename,preprocessing,false);
    }
    void validate_syntax(std::string_view source,const std::string& filename,const PreprocessorOptions& preprocessing) {
        (void)parse_source(source,filename,preprocessing,true);
    }
    static std::vector<std::filesystem::path> loose_sources(const std::filesystem::path& root) {
        namespace fs=std::filesystem;
        if(!fs::is_directory(root))return {fs::weakly_canonical(fs::absolute(root))};
    std::vector<fs::path> files;
    static const std::unordered_set<std::string> excluded= {
        "bin","build","obj","out","vendor",".git",".vs",".idea",".bolt"
    };
    for(fs::recursive_directory_iterator it(root),end;it!=end;++it) {
        auto name=path_to_utf8(it->path().filename());
        std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if(it->is_directory()&&(excluded.contains(name)||it->is_symlink()||fs::exists(it->path()/"bolt.proj"))) {
            it.disable_recursion_pending();
            continue;
        }
        auto ext=path_to_utf8(it->path().extension());
        std::transform(ext.begin(),ext.end(),ext.begin(),[](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if(it->is_regular_file()&&ext==".bolt")files.push_back(fs::weakly_canonical(fs::absolute(it->path())));
    }
    std::sort(files.begin(),files.end());
    return files;
    }
    std::vector<std::filesystem::path> discover_sources(const std::filesystem::path& root) {
        if(auto model=project::resolve(root))return std::move(model->sources);
        return loose_sources(root);
    }
    ProjectInputs project_inputs(const std::filesystem::path& root,const SourceOverrides& overrides) {
        namespace fs=std::filesystem;
        ProjectInputs inputs;
        inputs.graph=project::resolve(root,overrides);
        if(inputs.graph){
            // The resolver has already included unsaved sources in every project.
            inputs.files=inputs.graph->sources;
        }else{
            inputs.files=loose_sources(root);
            auto base=fs::weakly_canonical(fs::absolute(root));
            if(!fs::is_directory(base))base=base.parent_path();
            for(const auto& [name,text]:overrides){
                auto path=path_from_utf8(name);
                const auto relative=path.lexically_relative(base);
                if(!relative.empty()&&*relative.begin()!=".."&&path.extension()==".bolt"&&
                   std::find(inputs.files.begin(),inputs.files.end(),path)==inputs.files.end())
                    inputs.files.push_back(std::move(path));
            }
            std::sort(inputs.files.begin(),inputs.files.end());
        }
        return inputs;
    }
    Program load(const std::filesystem::path& root,bool require_entry,const PreprocessorOptions& preprocessing) {
        return load_project(root,{},require_entry,true,preprocessing);
    }
    Program load_project(const std::filesystem::path& root,const SourceOverrides& overrides,bool require_entry,bool check_semantics,const PreprocessorOptions& preprocessing) {
        return load_project(project_inputs(root,overrides),root,overrides,require_entry,check_semantics,preprocessing);
    }
    Program load_project(const ProjectInputs& inputs,const std::filesystem::path& root,const SourceOverrides& overrides,
                         bool require_entry,bool check_semantics,const PreprocessorOptions& preprocessing) {
        namespace fs=std::filesystem;
        Program result;
        const auto& project_model=inputs.graph;
        result.preprocessor=source_preprocessor_options(root,project_model?&*project_model:nullptr,preprocessing);
        if(project_model) {
            result.selected_entry=project_model->main.entry;
            result.prelude_enabled=project_model->main.prelude;
            result.library_mode=project_model->main.kind!="executable";
            result.project_identity=project_model->identity;
            for(const auto& d:project_model->dependencies)for(const auto& f:d.sources)result.external_sources.push_back(path_to_utf8(f));
            if(!project_model->dependencies.empty())result.require_cpp("local separately linked project libraries");
        }
        std::unordered_map<std::string,int> state;
        // Imports and discovered files usually share a long directory prefix.
        // Check each exact spelling once per load, including on Windows.
        std::unordered_set<fs::path::string_type> verified_spellings;
        std::function<void(const fs::path&,const Location&,std::size_t)> visit;
        visit=[&](const fs::path& path,const Location& from,std::size_t depth) {
            if(depth>128)throw Error(from,"import nesting limit exceeded", "BLT4002");
            std::error_code ec;
            auto absolute=fs::weakly_canonical(fs::absolute(path),ec);
            if(ec)throw Error(from,"cannot resolve import '"+path_to_utf8(path)+"'", "BLT1100");
            auto key=path_to_utf8(absolute);
            if(project_model){
                bool belongs=project::contains_source(project_model->main,absolute);
                for(const auto& dep:project_model->dependencies)belongs|=project::contains_source(dep,absolute);
                if(!belongs)throw Error(from,"imported file is outside project sources/dependencies: '"+key+"'","BLT4102");
                auto spelled=fs::absolute(path).lexically_normal(),prefix=spelled.root_path();
                for(const auto& part:spelled.relative_path()){
                    auto parent=prefix;
                    prefix/=part;
                    if(verified_spellings.contains(prefix.native()))continue;
                    bool exact=false,folded=false;
                    auto lower=[](std::string text){for(char& c:text)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return text;};
                    const auto wanted=path_to_utf8(part), folded_part=lower(wanted);
                    if(fs::is_directory(parent))for(const auto& child:fs::directory_iterator(parent)){
                        const auto name=path_to_utf8(child.path().filename());
                        if(name==wanted){exact=true;break;}
                        if(lower(name)==folded_part)folded=true;
                    }
                    if(!exact&&folded)throw Error(from,"source/import path has incorrect letter case: '"+path_to_utf8(path)+"'","BLT4102");
                    if(exact)verified_spellings.insert(prefix.native());
                }
            }
            if(state[key]==1)throw Error(from,"cyclic import of '"+key+"'", "BLT4002");
            if(state[key]==2)return;
            std::string source;
            if(auto it=overrides.find(key);it!=overrides.end())source=it->second;
            else {
                std::ifstream in(absolute,std::ios::binary);
                if(!in)throw Error(from,"cannot open '"+key+"'", "BLT4001");
                source.assign(std::istreambuf_iterator<char>(in),{});
            }
            state[key]=1;
            auto p=parse(source,key,source_preprocessor_options(absolute,project_model?&*project_model:nullptr,preprocessing));
            for(const auto& imp:p.imports) {
                if(is_builtin_import(imp.name))continue;
                auto target=path_from_utf8(imp.name);
                if(target.extension().empty())target+=".bolt";
                if(target.is_relative())target=absolute.parent_path()/target;
                if(fs::is_regular_file(target)||overrides.contains(path_to_utf8(fs::weakly_canonical(fs::absolute(target)))))visit(target,imp.token.at,depth+1);
                else if(imp.is_path)throw Error(imp.token.at,"cannot open imported source '"+imp.name+"'", "BLT4001");
                // Non-path imports can name a namespace supplied by another project file.
            }
            merge(result,std::move(p));
            state[key]=2;
            result.dependencies.push_back(key);
        };
        if(project_model||fs::is_directory(root)) {
            const auto& files=inputs.files;
            if(files.empty())throw Error( {
                path_to_utf8(root),1,1
            },"project folder contains no .bolt files", "BLT1100");
            for(const auto& f:files)visit(f, {
                path_to_utf8(f),1,1
            },0);
        }else visit(root, {
            path_to_utf8(root),1,1
        },0);
        try { if(check_semantics)analyze(result,require_entry&&!result.library_mode); }
        catch(Error& error) {
            if(auto it=result.sources.find(error.location.file);it!=result.sources.end())attach_source(error.diagnostic,it->second);
            throw;
        }
        return result;
    }
    // The C++ API also accepts a program previously analyzed for direct codegen.
    // Reuse its in-memory source snapshot, including project-local defines.
    Program prepare_cpp_emission(const Program& original) {
        Program result;
        result.preprocessor=original.preprocessor;
        result.preprocessor.defines["BoltCppCompatibility"]=true;
        result.library_mode=original.library_mode;
        result.prelude_enabled=original.prelude_enabled;
        result.selected_entry=original.selected_entry;
        result.project_identity=original.project_identity;
        result.external_sources=original.external_sources;
        result.dependencies=original.dependencies;
        auto append=[&](const std::string& name) {
            const auto it=original.sources.find(name);
            if(it==original.sources.end() || name.starts_with("<bolt-stdlib>/") || result.sources.contains(name))return;
            auto options=result.preprocessor;
            if(auto pp=original.source_preprocessors.find(name);pp!=original.source_preprocessors.end())
                options=pp->second;
            options.defines["BoltCppCompatibility"]=true;
            merge(result,parse(it->second,name,options));
        };
        for(const auto& name:original.dependencies)append(name);
        for(const auto& c:original.classes)append(c.token.at.file);
        for(const auto& [name,source]:original.sources)append(name);
        analyze_for_execution(result,!result.library_mode);
        return result;
    }
    Program parse_memory_source(std::string_view source,const std::string& filename,const PreprocessorOptions& preprocessing) {
        auto p=parse(source,filename,preprocessing);
        for(const auto& i:p.imports)if(i.is_path)throw Error(i.token.at,"in-memory code does not permit filesystem imports", "BLT1100");
        return p;
    }
    void append_memory_source(Program& program,std::string_view source,const std::string& filename) {
        if (program.sources.contains(filename)) throw std::invalid_argument("Duplicate source name: "+filename);
        merge(program,parse_memory_source(source,filename,program.preprocessor));
    }
    Program compile_source(std::string_view source,const std::string& filename,bool require_entry,const PreprocessorOptions& preprocessing) {
        auto p=parse_memory_source(source,filename,preprocessing);
        try { analyze(p,require_entry); }
        catch(Error& error) { attach_source(error.diagnostic,source); throw; }
        return p;
    }
}
// namespace bolt
