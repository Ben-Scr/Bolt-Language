#include "Analyzer.hpp"
#include <bolt/native/Layout.hpp>
#include <bolt/Value.hpp>
#include <bolt/Types.hpp>
#include <functional>
#include <limits>

namespace bolt::semantic {

bool Analyzer::has_operator(const std::string& t,const std::string& op,std::size_t count) {
    auto* c=find_class(t);if(!c||c->is_interface)return false;
    for(auto [owner,m]:methods(*c,"operator"+op)) {
        (void)owner;if(m->parameters.size()!=count||!m->is_public)continue;
        bool compatible=true;for(const auto& p:m->parameters)compatible&=referred(p.type)==t;
        if(compatible)return true;
    }
    return false;
}

bool Analyzer::overload(Expr& e,const std::string& op,bool update) {
    if(op=="&&"||op=="||"||op==",")return false; // Always short-circuit / sequence.
    std::vector<std::pair<Class*,Method*>> candidates;
    std::unordered_set<std::string> seen;
    for(const auto& arg:e.args)if(auto* c=find_class(arg->type);c&&!c->is_interface)
        for(auto [owner,m]:methods(*c,"operator"+op))if(m->parameters.size()==e.args.size()&&seen.insert(owner->name+"::"+m->id).second)candidates.emplace_back(owner,m);
    if(candidates.empty())return false;
    std::vector<Expr*> args;for(auto& a:e.args)args.push_back(a.get());
    // Operator operands are snapshotted before invocation. const-reference
    // overloads can therefore safely bind an expression/temporary as well.
    std::vector<bool> lvalues;for(auto* a:args){lvalues.push_back(a->lvalue);a->lvalue=true;}
    std::pair<Class*,Method*> selected;
    try{selected=select(std::move(candidates),{},args,e.token);}catch(...){for(std::size_t i=0;i<args.size();++i)args[i]->lvalue=lvalues[i];throw;}
    for(std::size_t i=0;i<args.size();++i)args[i]->lvalue=lvalues[i];
    auto [owner,m]=selected;
    if(update){
        if(m->type!=e.args[0]->type)throw Error(e.token.at,"an update operator must return the left operand type", "BLT2211");
        e.binding=Expr::Binding::Operator;e.owner=owner->name;e.resolved=m->id;e.type=m->type;
        return true;
    }
    auto target=std::make_unique<Expr>(Expr::Kind::Name,e.token,m->name);
    target->binding=Expr::Binding::Method;target->owner=owner->name;target->resolved=m->id;target->is_static=true;target->type="#method";
    e.kind=Expr::Kind::Call;e.binding=Expr::Binding::Method;e.owner=owner->name;e.resolved=m->id;e.type=m->type;e.declared_type=m->type;
    e.args.insert(e.args.begin(),std::move(target));
    return true;
}

std::string Analyzer::binary_type(const std::string& op,const Expr& a,const Expr& b,const Token& at) {
    value(a);
    value(b);
    if(op=="&&"||op=="||") {
        if(a.type!="bool"||b.type!="bool")throw Error(at.at,"logical operators require bool operands", "BLT2000");
        return "bool";
    }
    if(op=="+"&&(a.type=="string"||b.type=="string")) {
        if(!stringable(a.type)||!stringable(b.type))throw Error(at.at,"string concatenation requires primitive, enum, or IStringable values", "BLT2204");
        return "string";
    }
    if(op=="??") {
        if(is_optional(a.type)) {
            const auto payload=element_type(a.type);
            if(b.type=="null"||is_optional(b.type)) {
                if(can_convert(a.type,b))return a.type;
            } else if(can_convert(payload,b))return payload;
            throw Error(at.at,"optional fallback must be compatible with '"+payload+"'", "BLT2401");
        }
        if(!is_pointer(a.type))throw Error(at.at,"?? requires an optional value or nullable pointer on the left","BLT2020");
        if(!can_convert(a.type,b))throw Error(at.at,"null-coalescing operands must have compatible pointer types","BLT2020");
        return a.type;
    }
    if(op=="&"||op=="|"||op=="^"||op=="<<"||op==">>") {
        if(find_enum(a.type)&&((a.type==b.type&&(op=="&"||op=="|"||op=="^"))||(is_integer(b.type)&&(op=="<<"||op==">>")))){program.require_cpp("enum bitwise operators");return a.type;}
        if(!is_integer(a.type)||!is_integer(b.type))throw Error(at.at,"bitwise operators require integer operands, or compatible enum values","BLT2020");
        auto type=(op=="<<"||op==">>")?promoted(a.type):common_numeric(a.type,b.type);
        if(type.empty())throw Error(at.at,"mixed signed/ulong bitwise operands require an explicit cast","BLT2020");
        program.require_cpp("bitwise operators");return type;
    }
    bool equality=op=="=="||op=="!=";
    bool comparison=equality||op=="<"||op==">"||op=="<="||op==">=";
    if(equality&&(is_optional(a.type)||is_optional(b.type))) {
        if((is_optional(a.type)&&(b.type=="null"||b.type==a.type))||(is_optional(b.type)&&a.type=="null"))return "bool";
        throw Error(at.at,"compare compatible optionals, or compare an optional with null; use .Value for its payload", "BLT2401");
    }
    if(equality&&((is_pointer(a.type)&&(b.type=="null"||can_convert(a.type,b)))||(a.type=="null"&&(is_pointer(b.type)||b.type=="null"))))return "bool";
    if(equality&&a.type==b.type&&!is_numeric(a.type))return "bool";
    if(comparison&&a.type==b.type&&(a.type=="string"||a.type=="char"||find_enum(a.type)))return "bool";
    auto t=common_numeric(a.type,b.type);
    if(t.empty()) {
        if(is_numeric(a.type)&&is_numeric(b.type))throw Error(at.at,"mixed signed and ulong arithmetic requires an explicit cast", "BLT2000");
        throw Error(at.at,"operator '"+op+"' does not accept '"+a.type+"' and '"+b.type+"'", "BLT2000");
    }
    if(op=="%"&&(!is_integer(a.type)||!is_integer(b.type)))throw Error(at.at,"% requires integer operands", "BLT2000");
    return comparison?"bool":t;
}

void Analyzer::size_of(Expr& e,Class* context) {
    e.text=resolve(e.text,context,e.token);
    std::unordered_set<std::string> visited;
    std::function<void(std::string)> prepare_layout=[&](std::string type) {
        type=referred(type);
        if(!visited.insert(type).second)return;
        if(is_optional(type)){prepare_layout(element_type(type));return;}
        if(auto* c=find_class(type)) {
            prepare(*c);
            if(c->is_native||c->is_interface)return;
            if(!c->base.empty())prepare_layout(c->base);
            for(const auto& field:c->fields)if(!field.is_static)prepare_layout(field.type);
        }
    };
    prepare_layout(e.text);
    try {
        const auto size=native_abi::value_size(program,e.text);
        if(size>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw std::runtime_error("sizeof result exceeds int range");
        e.resolved=std::to_string(size);
    }catch(const std::exception& error){throw Error(e.token.at,error.what(),"BLT2000");}
    e.type="int";e.constant=true;
}

void Analyzer::finish_expression(Expr& e, bool callee, const std::string& expected, bool assignment_target) {
    using K = Expr::Kind;
    using B = Expr::Binding;
    remember_condition(e);
    if(e.binding==B::UserProperty&&!assignment_target){read_property(e);return;}
    if(e.binding==B::Method&&(e.kind==K::Name||e.kind==K::Member))bind_constant_template(e,callee);
    if(!callee&&e.type=="#method"&&(action_arity(referred(expected))>=0||function_type(referred(expected)))){bind_delegate(e,referred(expected));return;}
    if(!callee&&(e.type=="#builtin"||e.type=="#collection"))throw Error(e.token.at,"method must be called with parentheses", "BLT2000");
}

void Analyzer::expression(Expr& e,bool callee,const std::string& expected,bool assignment_target) {
    DepthGuard guard(*this,e.token.at);
    using K=Expr::Kind;
    using B=Expr::Binding;
    if(e.lambda) {
        if(expected.empty()){e.type="#lambda";return;}
        bind_lambda(e,referred(expected));return;
    }
    if(e.kind==K::Name&&e.binding==B::Method&&!e.resolved.empty()&&(e.is_extension||e.text.starts_with("operator")))return;
    flow.conditions.erase(&e);
    if(e.kind==K::Binary&&(e.text=="&&"||e.text=="||")) {
        condition(*e.args[0]);const auto first=condition_facts(*e.args[0]);
        const bool conjunction=e.text=="&&";flow.present=conjunction?first.first:first.second;
        condition(*e.args[1]);const auto second=condition_facts(*e.args[1]);
        auto yes=conjunction?second.first:intersect(first.first,second.first);
        auto no=conjunction?intersect(first.second,second.second):second.second;
        flow.present=intersect(yes,no);flow.conditions[&e]={std::move(yes),std::move(no)};e.type="bool";return;
    }
    if(e.kind==K::Let) {
        expression(*e.args[0],false,e.declared_type);convertible(e.declared_type,*e.args[0]);
        scopes.emplace_back();e.local_id=add_symbol(e.text,e.declared_type,false,e.token);
        expression(*e.args[1],false,expected);e.type=e.args[1]->type;e.constant=e.args[1]->constant;
        // A generated let returns an owned value, never an alias into temporary storage.
        e.lvalue=false;scopes.pop_back();return;
    }
    if(e.kind==K::OptionalHas||e.kind==K::OptionalValue) {
        expression(*e.args[0]);
        if(!is_optional(e.args[0]->type))throw Error(e.token.at,"expected an optional value", "BLT2400");
        e.type=e.kind==K::OptionalHas?"bool":element_type(e.args[0]->type);
        e.lvalue=e.kind==K::OptionalValue&&e.args[0]->lvalue;
        e.constant=e.kind==K::OptionalHas||e.args[0]->constant;
        e.declared_type=e.lvalue?e.type+"&":e.type;
        remember_condition(e);return;
    }
    if(e.kind==K::SizeOf){size_of(e,current_class);return;}
    if(e.kind==K::Default) {
        // Delay call arguments until overload resolution provides
        // their parameter type. Other untyped uses fail in value().
        if(expected.empty()) {e.type="#default";return;}
        if(is_reference(expected))throw Error(e.token.at,"'default' cannot initialize a reference; references require an existing value", "BLT2000");
        if(expected=="Arena")throw Error(e.token.at,"'default' cannot initialize an Arena owner; use Arena.Fixed or Arena.Growable", "BLT2300");
        if(expected=="void"||expected=="null"||expected.starts_with('@')||expected.starts_with('#'))
            throw Error(e.token.at,"'default' requires a constructible value type", "BLT2000");
        default_constructible(expected,e.token);
        e.type=expected;
        return;
    }
    if(e.kind==K::Literal) {
        if(e.token.kind==Token::Kind::Character)program.require_cpp("Unicode char");
        e.type=e.text=="null"&&e.token.kind!=Token::Kind::String?"null":literal_value(e).type;
        return;
    }
    if(e.kind==K::Conditional) {
        condition(*e.args[0]);auto branches=condition_facts(*e.args[0]);
        flow.present=branches.first;expression(*e.args[1],false,expected);auto after_yes=flow.present;
        flow.present=branches.second;expression(*e.args[2],false,expected);flow.present=intersect(after_yes,flow.present);
        auto& a=*e.args[1];auto& b=*e.args[2];value(a);value(b);
        if(a.type==b.type)e.type=a.type;
        else if(!common_numeric(a.type,b.type).empty())e.type=common_numeric(a.type,b.type);
        else if(is_pointer(a.type)&&can_convert(a.type,b))e.type=a.type;
        else if(is_pointer(b.type)&&can_convert(b.type,a))e.type=b.type;
        else if(is_optional(expected)&&can_convert(expected,a)&&can_convert(expected,b))e.type=expected;
        else if(is_optional(a.type)&&can_convert(a.type,b))e.type=a.type;
        else if(is_optional(b.type)&&can_convert(b.type,a))e.type=b.type;
        else if(a.type=="null"&&b.type!="void")e.type=b.type+"?";
        else if(b.type=="null"&&a.type!="void")e.type=a.type+"?";
        else throw Error(e.token.at,"conditional branches must have compatible value types","BLT2020");
        program.require_cpp("conditional operator");return;
    }
    if(e.kind==K::Name) {
        bind_name(e, callee);
    }else if(e.kind==K::Member) {
        expression(*e.args[0]);
        if(e.args[0]->binding==B::Event) {
            if(e.text!="Invoke")throw Error(e.token.at,"events expose only += and -= outside their declaring type", "BLT2701");
            require_event_owner(*e.args[0]);
            lower_event_receiver(*e.args[0]);e.text="Raise";
        }
        if(e.arrow&&is_pointer(e.args[0]->type)&&is_optional(element_type(e.args[0]->type))) {
            auto pointer=std::move(e.args[0]);
            auto deref=std::make_unique<Expr>(K::Unary,e.token,"*");deref->type=element_type(pointer->type);deref->lvalue=true;deref->args.push_back(std::move(pointer));
            e.args[0]=std::move(deref);e.arrow=false;
        }
        if(is_optional(e.args[0]->type)) {
            if(e.arrow)throw Error(e.token.at,"use . to access an optional value; -> requires a pointer", "BLT2400");
            if(e.text=="HasValue"||e.text=="Value") {
                if(!e.type_args.empty())throw Error(e.token.at,"optional properties are not generic", "BLT2400");
                e.kind=e.text=="HasValue"?K::OptionalHas:K::OptionalValue;
                e.type=e.kind==K::OptionalHas?"bool":element_type(e.args[0]->type);
                e.lvalue=e.kind==K::OptionalValue&&e.args[0]->lvalue;
                e.constant=e.kind==K::OptionalHas||e.args[0]->constant;
                e.declared_type=e.lvalue?e.type+"&":e.type;
                remember_condition(e);return;
            }
            narrow(*e.args[0]);
        }
        bind_member_access(e, callee);
    }else if(e.kind==K::Call) {
        call_expression(e, callee, expected, assignment_target);
        return;
    }else if(e.kind==K::Array) {
        std::string element=is_array(expected)?element_type(expected):"";
        const auto rank=array_rank(expected);
        if(rank>1) {
            e.dimensions.assign(rank,0);
            std::vector<bool> seen(rank,false);
            std::vector<ExprPtr> elements;
            std::function<void(Expr&,std::size_t)> flatten=[&](Expr& row,std::size_t depth) {
                if(row.kind!=K::Array)throw Error(row.token.at,"rectangular array initializer does not match the array rank", "BLT2000");
                if(seen[depth]&&e.dimensions[depth]!=row.args.size())throw Error(row.token.at,"rectangular array initializer must have equal lengths in every dimension", "BLT2000");
                seen[depth]=true;
                e.dimensions[depth]=row.args.size();
                for(auto& item:row.args) {
                    if(depth+1<rank)flatten(*item,depth+1);
                    else {
                        if(item->kind==K::Array&&!is_array(element))throw Error(item->token.at,"rectangular array initializer does not match the array rank", "BLT2000");
                        expression(*item,false,element);
                        convertible(element,*item);
                        elements.push_back(std::move(item));
                    }
                }
            };
            flatten(e,0);
            e.args=std::move(elements);
            e.type=expected;
            return;
        }
        for(auto& a:e.args) {
            expression(*a,false,element);
            value(*a);
            if(element.empty())element=a->type;
            convertible(element,*a);
        }
        if(element.empty())throw Error(e.token.at,"empty array literal needs a declared array type", "BLT2000");
        if(element=="Arena")throw Error(e.token.at,"Arena owners cannot be array elements", "BLT2300");
        e.type=element+"[]";
    }else if(e.kind==K::New) {
        auto t=resolve(e.text,current_class,e.token);
        if(const auto* c=find_class(t);c&&c->is_native)program.require_cpp("native standard library / interop");
        if(referred(t)=="Arena")throw Error(e.token.at,"Arena owners must be directly initialized as local variables", "BLT2300");
        e.text=t;
        if(e.arena) {
            expression(*e.arena);
            if(e.arena->type!="Arena"||!e.arena->lvalue||e.arena->constant)throw Error(e.arena->token.at,"placement allocation requires a mutable Arena owner or Arena& parameter", "BLT2300");
            if(e.arrow)throw Error(e.token.at,"placement array allocation is not supported; store arrays in an arena-owned object", "BLT2300");
        }
        std::vector<Expr*> args;
        for(auto& a:e.args) {
            expression(*a,false,!e.arrow&&!find_class(t)?t:"");
            if(a->type!="#default")value(*a);
            args.push_back(a.get());
        }
        if(e.arrow) {
            for(const auto* a:args)if(!is_integer(a->type))throw Error(a->token.at,"array sizes must be integers", "BLT2000");
            default_constructible(t,e.token);
            e.type=t+array_suffix(args.size());
        } else {
            if(is_reference(t))throw Error(e.token.at,"cannot allocate a reference", "BLT2000");
            if(auto* c=find_class(t)) {
                auto [owner,m]=constructor(t,args,e.token);
                (void)c;
                if(m){e.resolved=m->id;normalize_arguments(*owner,*m,e.args,0);}
            }else if(args.size()>1)throw Error(e.token.at,"scalar allocation expects zero or one argument", "BLT2000");
            else if(args.size()==1)convertible(t,*args[0]);
            e.type=t+"*";
        }
    }else if(e.kind==K::Delete) {
        expression(*e.args[0]);narrow(*e.args[0]);
        if(!is_pointer(e.args[0]->type)||element_type(e.args[0]->type)=="void")throw Error(e.token.at,"delete requires a typed pointer", "BLT2000");
        e.type="void";
    } else if(e.kind==K::Index) {
        auto& base=*e.args[0];
        auto& index=*e.args[1];
        expression(base);
        value(base);
        for(std::size_t i=1;i<e.args.size();++i){expression(*e.args[i]);value(*e.args[i]);narrow(*e.args[i]);}
        narrow(base);e.owner=base.type;
        if(auto* c=find_class(base.type);c&&property(*c,"@indexer").second) {
            const auto user_text=e.text;e.text="@indexer";
            bind_member(e,*c,false,base.lvalue,base.constant);
            const auto* p=find_property(*find_class(e.owner),"@indexer");
            if(e.args.size()!=p->parameters.size()+1)throw Error(e.token.at,"incorrect number of indexer arguments", "BLT2250");
            for(std::size_t i=1;i<e.args.size();++i)convertible(p->parameters[i-1].type,*e.args[i],p->parameters[i-1].constant);
            if(!assignment_target)read_property(e);
            return;
        }
        narrow(base);
        if(is_pointer(base.type)) {
            if(element_type(base.type)=="void")throw Error(e.token.at,"pointer indexing requires a typed pointer", "BLT2000");
            if(e.args.size()!=2||!is_integer(index.type))throw Error(e.token.at,"pointer indexing expects one integer index", "BLT2000");
            e.type=element_type(base.type);
            e.lvalue=true;
            // Match dereference: const freezes the pointer binding,
            // not the separate storage it points to.
            e.constant=false;
            return;
        } else if(is_array(base.type)) {
            if(e.args.size()-1!=array_rank(base.type))throw Error(e.token.at,"number of indices must match the array rank", "BLT2000");
            for(std::size_t i=1;i<e.args.size();++i)if(!is_integer(e.args[i]->type))throw Error(e.args[i]->token.at,"array indices must be integers", "BLT2000");
            e.type=element_type(base.type);
        } else if(is_collection(base.type)) {
            if(e.args.size()!=2)throw Error(e.token.at,"collection indexing requires one index", "BLT2000");
            auto kind=collection_kind(base.type);
            auto a=generic_args(base.type);
            if(kind=="List") {
                if(!is_integer(index.type))throw Error(index.token.at,"list index must be an integer", "BLT2000");
                e.type=a[0];
            }else if(kind=="Dictionary") {
                convertible(a[0],index);
                e.type=a[1];
            }else throw Error(e.token.at,"this collection has no indexer", "BLT2000");
        } else if(base.type=="string") {
            if(e.args.size()!=2||!is_integer(index.type))throw Error(e.token.at,"string indexing expects one integer index","BLT2020");
            e.type="char";e.constant=true;e.lvalue=false;program.require_cpp("Unicode string indexing");return;
        } else throw Error(e.token.at,"indexing requires an array, string, List, or Dictionary", "BLT2000");
        e.lvalue=base.lvalue;
        e.constant=base.constant;
    }else if(e.kind==K::Unary||e.kind==K::Postfix) {
        auto& operand=*e.args[0];
        expression(operand,false,"",e.text=="++"||e.text=="--");
        if(operand.binding==B::UserProperty){update_property(e);return;}
        value(operand);
        if(e.text!="&")narrow(operand);
        if(operand.type=="Arena")throw Error(e.token.at,"Arena owners cannot be copied or addressed; borrow through Arena& parameters", "BLT2300");
        if(e.text!="&"&&e.text!="*") {
            const bool update=e.text=="++"||e.text=="--";
            if(update)writable(operand);
            if(overload(e,e.text,update))return;
        }
        if(e.text=="await") {
            if(!current_method||!current_method->is_async)throw Error(e.token.at,"await is only valid inside an async method","BLT2230");
            if(generic_base(operand.type)!="System.Threading.Tasks.Task") {
                auto member=std::make_unique<Expr>(K::Member,e.token,"AsTask");
                member->arrow=is_pointer(operand.type);
                member->args.push_back(std::move(e.args[0]));
                auto conversion=std::make_unique<Expr>(K::Call,e.token);
                conversion->args.push_back(std::move(member));
                expression(*conversion);
                if(generic_base(conversion->type)!="System.Threading.Tasks.Task")
                    throw Error(e.token.at,"await requires Task or an AsTask() returning Task","BLT2230");
                e.args[0]=std::move(conversion);
            }
            program.require_cpp("async/await tasks");
            // Await reads the public Task contract like an ordinary call.
            // It must not name a hidden native Task::GetResult implementation.
            auto target=std::make_unique<Expr>(K::Member,e.token,"GetResult");
            target->args.push_back(std::move(e.args[0]));
            Expr call(K::Call,e.token);call.args.push_back(std::move(target));
            e=std::move(call);expression(e);return;
        }
        if(e.text=="~") {
            if(!is_integer(operand.type)&&!find_enum(operand.type))throw Error(e.token.at,"~ requires an integer or enum operand","BLT2020");
            e.type=find_enum(operand.type)?operand.type:promoted(operand.type);program.require_cpp("bitwise operators");return;
        }
        if(e.text=="&") {
            if(!operand.lvalue||operand.constant)throw Error(e.token.at,"address-of requires a mutable lvalue", "BLT2000");
            e.type=operand.type+"*";
            if(auto id=optional_root(operand))flow.escaped.insert(id);
        } else if(e.text=="*") {
            if(!is_pointer(operand.type)||element_type(operand.type)=="void")throw Error(e.token.at,"dereference requires a typed pointer", "BLT2000");
            e.type=element_type(operand.type);
            e.lvalue=true;
            // const freezes the pointer binding, not its separate pointee.
            e.constant=false;
        } else if(e.text=="!") {
            if(operand.type!="bool")throw Error(e.token.at,"! requires a bool", "BLT2000");
            e.type="bool";
        } else if(e.text=="++"||e.text=="--") {
            writable(operand);
            if(!is_numeric(operand.type))throw Error(e.token.at,"increment/decrement requires a number", "BLT2000");
            e.type=operand.type;
        } else {
            if(!is_numeric(operand.type))throw Error(e.token.at,"unary arithmetic requires a number", "BLT2000");
            e.type=promoted(operand.type);
        }
    }else if(e.kind==K::Binary||e.kind==K::Assign) {
        if(e.text=="??"||e.text=="?" "?=")program.require_cpp("null coalescing");
        auto& a=*e.args[0];
        auto& b=*e.args[1];
        expression(a,false,"",e.kind==K::Assign);
        if(a.binding==B::Event) {
            if(e.kind!=K::Assign||(e.text!="+="&&e.text!="-="))throw Error(e.token.at,"events can only be subscribed to with += or unsubscribed with -=", "BLT2701");
            if(a.constant||!a.lvalue)throw Error(e.token.at,"event subscription requires a mutable, existing receiver", "BLT2701");
            if(auto* event_owner=find_class(a.owner))if(auto* event_field=find_field(*event_owner,a.text)) {
                expression(b,false,event_field->event_type);
                if(b.type=="#method")delegate_method(b,event_field->event_type);
                else if(!can_convert(event_field->event_type,b))throw Error(b.token.at,"event handler must match '"+event_field->event_type+"'", "BLT2702");
            }
            lower_event_receiver(a);
            auto method=std::make_unique<Expr>(K::Member,e.token,e.text=="+="?"Subscribe":"Unsubscribe");method->args.push_back(std::move(e.args[0]));
            Expr call(K::Call,e.token);call.args.push_back(std::move(method));call.args.push_back(std::move(e.args[1]));e=std::move(call);
            expression(e);return;
        }
        if(e.kind==K::Assign&&a.binding==B::UserProperty){update_property(e);return;}
        if(e.kind==K::Binary&&e.text!=","&&a.type=="#default") {
            expression(b);
            value(b);
            expression(a,false,b.type);
        }else expression(b,false,e.text==","?expected:e.text=="??"&&is_optional(a.type)?element_type(a.type):a.type);
        if(e.kind==K::Binary&&e.text!="=="&&e.text!="!="&&e.text!="??"&&e.text!=",") {
            // String formatting of an optional is intentionally not extraction.
            if(!(e.text=="+"&&(a.type=="string"||b.type=="string"))){narrow(a);narrow(b);}
        }
        if(e.kind==K::Binary&&(e.text=="=="||e.text=="!=")&&a.type!="null"&&b.type!="null") {
            if(is_optional(a.type)&&!is_optional(b.type))narrow(a);
            if(is_optional(b.type)&&!is_optional(a.type))narrow(b);
        }
        if(a.type=="Arena"||b.type=="Arena")throw Error(e.token.at,"Arena owners cannot be copied, assigned, or used as operands", "BLT2300");
        if(e.kind==K::Assign) {
            writable(a);
            if(is_interface(program,a.type)&&(is_reference(a.declared_type)||(a.kind==K::Unary&&a.text=="*")))throw Error(e.token.at,"an interface reference cannot replace the referenced value; call its methods instead", "BLT2205");
            if(e.text!="="&&overload(e,e.text.substr(0,e.text.size()-1),true))return;
            if(e.text=="=") {
                convertible(a.type,b);
                if(a.binding==B::Local&&is_pointer(a.type))set_local_arena_alias(a.local_id,local_arena_alias(b));
                if(a.kind==K::Index&&collection_kind(a.owner)=="Dictionary")a.is_static=true;
            }else if(e.text=="?" "?="&&is_optional(a.type))convertible(a.type,b);
            else if(!assignable(a.type,binary_type(e.text.substr(0,e.text.size()-1),a,b,e.token)))throw Error(e.token.at,"compound assignment produces an incompatible type", "BLT2000");
            e.type=a.type;
            if(e.text=="="||e.text=="?" "?=")optional_assignment(a,b);
        } else if(e.text==",") {
            value(a,true);
            value(b,true);
            e.type=b.type;
        } else { if(overload(e,e.text))return; e.type=binary_type(e.text,a,b,e.token); }
    }
    finish_expression(e, callee, expected, assignment_target);
}

bool Analyzer::pure(const Expr& e) const {
    if(e.kind==Expr::Kind::SizeOf)return true;
    if(e.kind==Expr::Kind::Default)return true;
    if(e.kind==Expr::Kind::New&&!e.arena){for(const auto& a:e.args)if(!pure(*a))return false;return true;}
    if(e.kind==Expr::Kind::Literal||e.binding==Expr::Binding::EnumValue||(e.binding==Expr::Binding::Property&&(e.owner=="Math"||is_numeric(e.owner))))return true;
    if(e.kind==Expr::Kind::Unary&&(e.text=="+"||e.text=="-"||e.text=="!"))return pure(*e.args[0]);
    if(e.kind==Expr::Kind::Binary)return pure(*e.args[0])&&pure(*e.args[1]);
    if(e.kind==Expr::Kind::Array) {
        for(const auto& a:e.args)if(!pure(*a))return false;
        return true;
    }
    if(e.kind==Expr::Kind::Call&&(e.binding==Expr::Binding::Cast||e.binding==Expr::Binding::Construct)) {
        for(std::size_t i=1;i<e.args.size();++i)if(!pure(*e.args[i]))return false;
        return true;
    }
    return false;
}

bool Analyzer::constant_initializer(const Expr& e) const {
    if(e.kind==Expr::Kind::SizeOf)return true;
    if(e.kind==Expr::Kind::Default)return !find_class(e.type);
    if(e.kind==Expr::Kind::Literal||e.binding==Expr::Binding::EnumValue||(e.binding==Expr::Binding::Property&&(e.owner=="Math"||is_numeric(e.owner))))return true;
    if(e.kind==Expr::Kind::Unary&&(e.text=="+"||e.text=="-"||e.text=="!"))return constant_initializer(*e.args[0]);
    if(e.kind==Expr::Kind::Binary)return constant_initializer(*e.args[0])&&constant_initializer(*e.args[1]);
    if(e.kind==Expr::Kind::Array) {
        for(const auto& a:e.args)if(!constant_initializer(*a))return false;
        return true;
    }
    if(e.kind==Expr::Kind::Call&&e.binding==Expr::Binding::Cast&&e.args.size()==2) {
        const auto builtin_value=[&](const std::string& type) { return is_primitive(type)||find_enum(type); };
        return builtin_value(e.type)&&builtin_value(e.args[1]->type)&&constant_initializer(*e.args[1]);
    }
    return false;
}

} // namespace bolt::semantic
