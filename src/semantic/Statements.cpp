#include "Analyzer.hpp"
#include "../ClosureConversion.hpp"
#include <bolt/Value.hpp>
#include <bolt/Types.hpp>
#include <algorithm>

namespace bolt::semantic {

void Analyzer::scoped(Stmt& s) {
    scopes.emplace_back();
    statement(s);
    scopes.pop_back();
}

void Analyzer::condition(Expr& e) {
    expression(e);narrow(e);
    if(e.type!="bool")throw Error(e.token.at,"conditions must have type bool", "BLT2000");
}

Value Analyzer::case_value(const Expr& e,unsigned depth) const {
    using K=Expr::Kind;
    if(depth>128)throw Error(e.token.at,"cyclic or deeply nested case constant","BLT2000");
    if(e.kind==K::Literal)return literal_value(e);
    if(e.binding==Expr::Binding::EnumValue)return Value(e.resolved);
    const Expr* initializer=nullptr;
    if(e.binding==Expr::Binding::Local) {
        for(auto it=scopes.rbegin();it!=scopes.rend();++it) {
            auto local=it->find(e.text);
            if(local!=it->end()&&local->second.id==e.local_id){initializer=local->second.constant_value;break;}
        }
    }else if(e.binding==Expr::Binding::Field&&e.is_static) {
        if(const auto* owner=find_class(e.owner))if(const auto* f=find_field(*owner,e.text);f&&f->constant)
            initializer=f->initializer.get();
    }
    if(initializer) {
        auto v=case_value(*initializer,depth+1);
        return is_integer(e.type)?convert_value(e.type,v):v;
    }
    if(e.kind==K::SizeOf)return Value(static_cast<std::int32_t>(std::stol(e.resolved)));
    if(e.binding==Expr::Binding::Property&&is_integer(e.owner)&&(e.text=="MinValue"||e.text=="MaxValue"))
        return numeric_limit_value(e.owner,e.text=="MaxValue");
    if(e.kind==K::Binary&&is_integer(e.type))
        return numeric_binary(e.text,e.type,case_value(*e.args[0],depth+1),case_value(*e.args[1],depth+1));
    if(e.kind==K::Unary&&is_integer(e.type)&&(e.text=="+"||e.text=="-")) {
        auto v=case_value(*e.args[0],depth+1);
        return e.text=="+"?v:numeric_binary("-",e.type,default_primitive(e.type),v);
    }
    if(e.kind==K::Call&&e.binding==Expr::Binding::Cast&&is_integer(e.type)&&e.args.size()==2)
        return convert_value(e.type,case_value(*e.args[1],depth+1));
    throw Error(e.token.at,"case label must be a compile-time constant","BLT2000");
}

void Analyzer::switch_statement(Stmt& s) {
    using K=Stmt::Kind;
    scopes.emplace_back();
    auto selector=std::make_unique<Stmt>(K::Variable,s.token);
    selector->name="@switch";selector->name_token=s.token;selector->type="var";selector->constant=true;
    selector->expression=std::move(s.expression);statement(*selector);
    const auto type=selector->type;
    if(!is_integer(type)&&type!="char"&&type!="bool"&&type!="string"&&!find_enum(type))
        throw Error(s.token.at,"switch requires an integer, char, bool, string or enum","BLT2000");
    std::unordered_set<std::string> values;
    auto dispatch=std::make_unique<Stmt>(K::Block,s.token);
    dispatch->statements.push_back(std::move(selector));
    StmtPtr fallback;
    ++switches;
    for(auto& arm:s.statements) {
        ExprPtr test;
        for(auto& label:arm->statements) {
            auto& e=*label->expression;expression(e);
            if(e.type!=type&&!(is_integer(type)&&is_integer(e.type)))
                throw Error(e.token.at,"case label does not match switch type '"+type+"'","BLT2000");
            try {
                auto v=case_value(e);
                if(is_integer(type))v=convert_value(type,v);
                if(!values.insert(v.to_string()).second)throw Error(e.token.at,"duplicate case value","BLT2000");
            }catch(const Error&){throw;}catch(const std::exception& ex){throw Error(e.token.at,ex.what(),"BLT2000");}
            auto equal=std::make_unique<Expr>(Expr::Kind::Binary,e.token,"==");
            equal->args.push_back(std::make_unique<Expr>(Expr::Kind::Name,e.token,"@switch"));
            equal->args.push_back(std::move(label->expression));
            if(test) {
                auto either=std::make_unique<Expr>(Expr::Kind::Binary,arm->token,"||");
                either->args.push_back(std::move(test));either->args.push_back(std::move(equal));test=std::move(either);
            }else test=std::move(equal);
        }
        if(arm->name=="default")fallback=std::move(arm->body);
        else {
            auto branch=std::make_unique<Stmt>(K::If,arm->token);
            branch->condition=std::move(test);branch->body=std::move(arm->body);
            branch->body->statements.push_back(std::make_unique<Stmt>(K::Break,arm->token));
            dispatch->statements.push_back(std::move(branch));
        }
    }
    if(fallback)dispatch->statements.push_back(std::move(fallback));
    const auto entry_facts=flow.present;
    for(std::size_t i=1;i<dispatch->statements.size();++i) {
        flow.present=entry_facts;
        statement(*dispatch->statements[i]);
    }
    --switches;scopes.pop_back();flow.present.clear();
    s.kind=K::SwitchScope;s.statements.clear();
    s.body=std::move(dispatch);
}

void Analyzer::statement(Stmt& s) {
    using K=Stmt::Kind;
    if(s.kind==K::Switch){switch_statement(s);return;}
    if(s.kind==K::Declarations) {
        for(auto& c:s.statements)statement(*c);
    } else if(s.kind==K::Block) {
        scopes.emplace_back();
        for(auto& c:s.statements)statement(*c);
        scopes.pop_back();
    } else if(s.kind==K::Variable) {
        if(s.name.starts_with("@cell")&&s.expression) {
            const auto* captured=symbol(s.expression->args.back()->text);
            if(captured&&(is_pointer(captured->type)||is_reference(captured->type)||captured->type=="Arena"))
                throw Error(s.name_token.at,"a closure cannot capture a borrowed pointer, reference or Arena","BLT2702");
        }
        const auto* previous_initializer=arena_initializer;
        arena_initializer=(s.type=="Arena"||s.type=="var")?s.expression.get():nullptr;
        if(s.type=="var") {
            if(!s.expression)throw Error(s.token.at,"'var' declarations require an initializer", "BLT2000");
            std::string lambda_target;
            if(s.expression->lambda)lambda_target=infer_lambda(*s.expression);
            expression(*s.expression,false,lambda_target);
            value(*s.expression);
            auto element=s.expression->type;
            while(is_array(element))element=element_type(element);
            if(element=="null")throw Error(s.expression->token.at,"cannot infer a 'var' type from null; declare an explicit type", "BLT2000");
            // Expression types are already canonical. Resolving again
            // could bind a global type to a same-named namespace type.
            // Reference expressions yield a value type, so var copies.
            s.type=lambda_target.empty()?s.expression->type:lambda_target;
            convertible(s.type,*s.expression,s.constant);
        }else {
            s.type=resolve(s.type,current_class,s.token);
            if(s.type=="Arena&")throw Error(s.token.at,"Arena& borrows are supported only as method parameters", "BLT2300");
            if(s.type=="Arena"&&!s.expression)throw Error(s.token.at,"Arena owners require direct initialization with Arena.Fixed or Arena.Growable", "BLT2300");
            if((s.constant||is_reference(s.type))&&!s.expression)throw Error(s.token.at,"const locals and references need an initializer", "BLT2000");
            if(s.expression) {
                expression(*s.expression,false,s.type);
                convertible(s.type,*s.expression,s.constant);
            }else default_constructible(s.type,s.token);
        }
        auto native_type=referred(s.type);if(is_array(native_type))native_type=element_type(native_type);
        if(const auto* c=find_class(native_type);c&&c->is_native)program.require_cpp("native standard library / interop");
        if(native_type=="char")program.require_cpp("Unicode char");
        if(is_reference(s.type)&&s.expression&&!s.expression->lvalue)throw Error(s.token.at,"a local reference requires an existing lvalue; temporary binding is only supported for const method parameters","BLT2002");
        const bool arena_alias=s.expression&&(is_pointer(s.type)||is_reference(s.type))&&local_arena_alias(*s.expression,is_reference(s.type));
        s.local_id=add_symbol(s.name,s.type,s.constant,s.token);
        if(s.constant)scopes.back().at(s.name).constant_value=s.expression.get();
        if(is_optional(referred(s.type))) {
            if(is_reference(s.type)&&s.expression){auto root=optional_root(*s.expression);if(root)flow.aliases[s.local_id]=root;}
            else if(s.expression) {
                Expr local(Expr::Kind::Name,s.token,s.name);local.type=s.type;local.binding=Expr::Binding::Local;local.local_id=s.local_id;optional_assignment(local,*s.expression);
            }
        }
        if(arena_alias)set_local_arena_alias(s.local_id,true);
        arena_initializer=previous_initializer;
    }else if(s.kind==K::ForEach) {
        auto range_expression=clone(*s.expression);
        expression(*s.expression);value(*s.expression);narrow(*s.expression);
        const auto source=s.expression->type;
        const auto protocol=iteration_element(source,current_class,s.token,false,s.expression->constant);
        const auto variable_type=s.type=="var"?protocol.type:resolve(s.type,current_class,s.token);
        if(referred(variable_type)!=protocol.type)throw Error(s.token.at,"range element is '"+protocol.type+"', not '"+variable_type+"'","BLT2021");
        if(!is_array(source)&&source!="string"&&!is_collection(source)&&!((source=="Ecs.Query"||source=="Ecs.ComponentQuery")&&program.find_class(source)->is_native)) {
            // Ordinary structural iteration, lowered once in the shared frontend.
            // A borrowed lvalue is not copied; a temporary is owned for the whole loop.
            const auto suffix=std::to_string(++property_temporary);
            const auto source_name="@range_source"+suffix, iterator_name="@range_iterator"+suffix;
            auto name=[&](const std::string& n){return std::make_unique<Expr>(Expr::Kind::Name,s.token,n);};
            auto member=[&](const std::string& base,const std::string& n){auto e=std::make_unique<Expr>(Expr::Kind::Member,s.token,n);e->args.push_back(name(base));return e;};
            auto invoke=[&](const std::string& base,const std::string& n){auto e=std::make_unique<Expr>(Expr::Kind::Call,s.token);e->args.push_back(member(base,n));return e;};
            Stmt block(K::Block,s.token);
            auto source_var=std::make_unique<Stmt>(K::Variable,s.token);source_var->type=source+(s.expression->lvalue?"&":"");source_var->name=source_name;source_var->constant=s.expression->constant;source_var->name_token=s.token;source_var->expression=std::move(range_expression);block.statements.push_back(std::move(source_var));
            auto iterator=std::make_unique<Stmt>(K::Variable,s.token);iterator->type="var";iterator->name=iterator_name;iterator->name_token=s.token;iterator->expression=invoke(source_name,"GetEnumerator");block.statements.push_back(std::move(iterator));
            auto loop=std::make_unique<Stmt>(K::While,s.token);loop->condition=invoke(iterator_name,"MoveNext");loop->body=std::make_unique<Stmt>(K::Block,s.token);
            auto variable=std::make_unique<Stmt>(K::Variable,s.token);variable->type=s.type;variable->name=s.name;variable->name_token=s.name_token;variable->constant=s.constant;variable->expression=member(iterator_name,"Current");
            loop->body->statements.push_back(std::move(variable));loop->body->statements.push_back(std::move(s.body));block.statements.push_back(std::move(loop));
            statement(block);s=std::move(block);return;
        }
        s.type=variable_type;
        if((source=="string"||collection_kind(source)=="Dictionary"||(generic_base(source)=="Ecs.Query"||generic_base(source)=="Ecs.ComponentQuery"))&&is_reference(s.type))throw Error(s.token.at,"this range yields value views; omit & and borrow individual components instead","BLT2021");
        if(collection_kind(source)=="HashSet"&&is_reference(s.type)&&!s.constant)throw Error(s.token.at,"HashSet elements cannot be borrowed mutably; use const T& or a value","BLT2021");
        if(is_reference(s.type)&&protocol.constant&&!s.constant)throw Error(s.token.at,"a const range cannot yield mutable references","BLT2021");
        flow.present.clear();scopes.emplace_back();s.local_id=add_symbol(s.name,s.type,s.constant,s.name_token);++loops;scoped(*s.body);--loops;scopes.pop_back();flow.present.clear();
        program.require_cpp("range-for");
    }else if(s.kind==K::Try) {
        program.require_cpp("exception handling");
        for(auto& type:program.classes)if(type.is_native&&derives(type.name,"System.Exception"))mark_type(type.name);
        flow.present.clear();scoped(*s.body);flow.present.clear();
        std::vector<std::string> caught;
        for(auto& handler:s.statements) {
            flow.present.clear();scopes.emplace_back();
            if(!handler->type.empty()) {
                handler->type=resolve(handler->type,current_class,handler->token);
                const auto type=referred(handler->type);
                if(!derives(type,"System.Exception"))throw Error(handler->token.at,"catch types must derive from Exception","BLT2240");
                for(const auto& previous:caught)if(derives(type,previous))throw Error(handler->token.at,"unreachable catch: '"+type+"' is already handled","BLT2240");
                caught.push_back(type);handler->local_id=add_symbol(handler->name,type+"&",false,handler->name_token);
            }
            ++catch_depth;statement(*handler->body);--catch_depth;scopes.pop_back();
        }
        flow.present.clear();
        if(s.alternative){const auto saved=loops,saved_switches=switches;loops=switches=0;++finally_depth;scoped(*s.alternative);--finally_depth;loops=saved;switches=saved_switches;}
        flow.present.clear();
    }else if(s.kind==K::Throw) {
        program.require_cpp("exception handling");
        if(s.expression){expression(*s.expression);if(!derives(s.expression->type,"System.Exception"))throw Error(s.token.at,"throw expects an Exception value, not a pointer","BLT2240");}
        else if(!catch_depth)throw Error(s.token.at,"bare throw is only valid inside catch","BLT2240");
    }else if(s.kind==K::Expression) {
        expression(*s.expression);
        if(s.expression->type=="Arena")throw Error(s.token.at,"Arena owners cannot be evaluated by value", "BLT2300");
        value(*s.expression,true);
    } else if(s.kind==K::If) {
        condition(*s.condition);const auto branch=condition_facts(*s.condition);
        flow.present=branch.first;scoped(*s.body);const auto yes=flow.present;
        flow.present=branch.second;if(s.alternative)scoped(*s.alternative);const auto no=flow.present;
        if(returns(*s.body))flow.present=no;
        else if(s.alternative&&returns(*s.alternative))flow.present=yes;
        else flow.present=intersect(yes,no);
    } else if(s.kind==K::While||s.kind==K::For) {
        scopes.emplace_back();
        if(s.init)statement(*s.init);
        flow.present.clear();
        if(s.condition){condition(*s.condition);flow.present=condition_facts(*s.condition).first;}
        ++loops;scoped(*s.body);--loops;
        // Increment follows the body. Do not carry body facts into a later iteration.
        if(s.increment) {
            expression(*s.increment);
            if(s.increment->type=="Arena")throw Error(s.increment->token.at,"Arena owners cannot be evaluated by value", "BLT2300");
            value(*s.increment,true);
        }
        flow.present.clear();scopes.pop_back();
    } else if(s.kind==K::Return) {
        if(!current_method)throw Error(s.token.at,"return outside a method", "BLT2000");
        if(finally_depth)throw Error(s.token.at,"return cannot leave a finally block","BLT2240");
        const auto return_type=current_method->is_async?generic_args(current_method->type).front():current_method->type;
        if(return_type=="void") {
            if(s.expression) {
                expression(*s.expression);
                if(s.expression->type!="void")throw Error(s.token.at,"void methods cannot return a value", "BLT2000");
            }
        } else {
            if(!s.expression)throw Error(s.token.at,"return requires a value", "BLT2000");
            expression(*s.expression,false,return_type);
            convertible(return_type,*s.expression);
            if((is_pointer(current_method->type)||is_reference(current_method->type))&&local_arena_alias(*s.expression,is_reference(current_method->type)))
                throw Error(s.token.at,"cannot return a pointer or reference into a local Arena", "BLT2300");
            if(is_reference(current_method->type)) {
                const Expr* root=s.expression.get();
                while((root->kind==Expr::Kind::Member&&!root->arrow&&!root->is_static)||root->kind==Expr::Kind::Index||root->kind==Expr::Kind::OptionalValue)
                    root=root->args[0].get();
                if(root->binding==Expr::Binding::Local){
                    const auto* sym=symbol(root->text);
                    if(sym&&!is_reference(sym->type))throw Error(s.token.at,"cannot return a reference to a local value or by-value parameter", "BLT2000");
                }
            }
        }
    }else if((s.kind==K::Continue||(s.kind==K::Break&&!switches))&&loops==0)throw Error(s.token.at,s.kind==K::Break?"break requires a loop or switch":"continue requires a loop", "BLT2000");
}

bool Analyzer::returns(const Stmt& s) const {
    if(s.kind==Stmt::Kind::SwitchScope)return !has_switch_break(*s.body)&&returns(*s.body);
    if(s.kind==Stmt::Kind::Return||s.kind==Stmt::Kind::Throw)return true;
    if(s.kind==Stmt::Kind::Try){if(!returns(*s.body))return false;for(const auto& c:s.statements)if(!returns(*c->body))return false;return true;}
    if(s.kind==Stmt::Kind::If)return s.alternative&&returns(*s.body)&&returns(*s.alternative);
    if(s.kind==Stmt::Kind::Block)for(const auto& c:s.statements) {
        if(returns(*c))return true;
        if(c->kind==Stmt::Kind::Break||c->kind==Stmt::Kind::Continue)break;
    }
    return false;
}

bool Analyzer::has_switch_break(const Stmt& s) const {
    using K=Stmt::Kind;
    if(s.kind==K::Break)return true;
    if(s.kind==K::SwitchScope||s.kind==K::While||s.kind==K::For||s.kind==K::ForEach)return false;
    if((s.body&&has_switch_break(*s.body))||(s.alternative&&has_switch_break(*s.alternative)))return true;
    for(const auto& child:s.statements){if(has_switch_break(*child))return true;if(returns(*child))break;}
    return false;
}

void Analyzer::analyze_method(Class& c,Method& m) {
    if(m.analyzed||m.analyzing||!m.generic_parameters.empty()||!m.pack_parameter.empty())return;
    if(m.is_abstract||m.is_extern){prepare_method(c,m);m.analyzed=true;return;}
    m.analyzing=true;
    if(m.is_destructor)program.require_cpp("destructors and RAII");
    prepare_method(c,m);mark_type(c.name);mark_type(m.type);for(const auto& p:m.parameters)mark_type(p.type);
    current_class=&c;
    current_method=&m;flow={};
    if(m.is_specialization)specialize_metadata(c,m);
    static_context=m.is_static;
    catch_depth=0;finally_depth=0;
    if(m.is_async) {
        if(generic_base(m.type)!="System.Threading.Tasks.Task")throw Error(m.token.at,"async methods must return Task or Task<T>","BLT2230");
        if(is_pointer(generic_args(m.type).front())||is_reference(generic_args(m.type).front()))throw Error(m.token.at,"async results must own their values, not borrow a worker stack","BLT2230");
        for(const auto& p:m.parameters)if(is_reference(p.type)||is_pointer(p.type))throw Error(p.token.at,"async parameters must own values, not borrow pointers or references","BLT2230");
        program.require_cpp("async/await tasks");
        // Native async entry execution retrieves the result through the
        // source-defined method. Keep it even when only Main awaits it.
        if(auto* task=find_class(m.type))for(auto& member:task->methods)
            if(member.name=="GetResult")member.referenced=true;
    }
    scopes.clear();
    scopes.emplace_back();
    loops=0;
    for(auto& p:m.parameters)check_default(c,p);
    for(auto& p:m.parameters){p.local_id=add_symbol(p.name,p.type,p.constant,p.token,true);if(is_reference(p.type)&&is_optional(referred(p.type)))flow.escaped.insert(p.local_id);}
    if(m.is_constructor) {
        std::unordered_set<std::string> initialized;
        for(auto& init:m.constructor_initializers) {
            if(init->text=="base"||(!c.base.empty()&&(init->text==c.base||init->text==find_class(c.base)->short_name))) {
                if(m.explicit_base)throw Error(init->token.at,"base constructor initialized more than once", "BLT2251");
                m.explicit_base=true;m.base_arguments=std::move(init->args);continue;
            }
            std::string field=init->text;
            if(const auto* prop=find_property(c,field);prop&&!prop->backing_field.empty())field=prop->backing_field;
            auto it=std::find_if(c.fields.begin(),c.fields.end(),[&](const auto& f){return f.name==field&&!f.is_static;});
            if(it==c.fields.end())throw Error(init->token.at,"constructor initializer must name an instance field, auto-property, or base class", "BLT2251");
            if(!initialized.insert(field).second)throw Error(init->token.at,"member initialized more than once", "BLT2251");
            if(init->args.size()!=1)throw Error(init->token.at,"member initializer requires one value expression", "BLT2251");
            auto value=std::move(init->args.front());
            // Initializers may use parameters but not partially initialized this.
            const bool previous=static_context;static_context=true;
            expression(*value,false,it->type);convertible(it->type,*value);static_context=previous;
            m.field_initializers.emplace_back(field,std::move(value));
        }
        for(const auto& field:c.fields)if(!field.is_static&&!field.initializer&&!initialized.contains(field.name))default_constructible(field.type,field.token);
        std::vector<Expr*> args;
        for(auto& a:m.base_arguments) {
            expression(*a);
            args.push_back(a.get());
        }
        if(c.base.empty()) {
            if(m.explicit_base)throw Error(m.token.at,"base constructor initializer on a class without a base", "BLT2000");
        } else {
            auto selected=constructor(c.base,args,m.token,true);
            if(selected.second){m.base_constructor=selected.second->id;normalize_arguments(*selected.first,*selected.second,m.base_arguments,0);}
        }
    }
    if(!m.name.starts_with("@lambda")&&ClosureConversion::contains_lambda(*m.body))ClosureConversion{}.run(m);
    statement(*m.body);
    if(m.is_constant_template&&!constant_initializer(*m.body->statements[0]->expression))throw Error(m.token.at,"generic constant initializers must be side-effect-free literal, builtin cast, arithmetic, or array expressions", "BLT2000");
    if((m.is_async?generic_args(m.type).front():m.type)!="void"&&!returns(*m.body))throw Error(m.token.at,"not all paths explicitly return a value", "BLT2000");
    scopes.clear();
    m.analyzing=false;
    m.analyzed=true;
}

} // namespace bolt::semantic
