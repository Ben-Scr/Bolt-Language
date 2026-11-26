#include "Analyzer.hpp"
#include <bolt/Value.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <functional>

namespace bolt::semantic {

void Analyzer::value(const Expr& e,bool allow_void) const {
    if(e.lambda)throw Error(e.token.at,"lambda needs a target type such as Action<int> or Func<int,int>","BLT2702");
    if(e.type=="#default")throw Error(e.token.at,"'default' requires a target type; declare an explicit type or use a cast", "BLT2000");
    if(e.type.empty()||e.type.starts_with('@')||e.type.starts_with('#')||(!allow_void&&e.type=="void"))throw Error(e.token.at,"expected a value, not a type, method, or void expression", "BLT2000");
}

bool Analyzer::standard_convert(const std::string& target,const Expr& e,bool constant) {
    if(e.type=="#default")return !is_reference(target)&&target!="void"&&target!="Arena";
    if(is_reference(target)) {
        auto t=element_type(target);
        if(is_optional(e.type)&&!is_optional(t)&&is_present(e)) {
            Expr payload(Expr::Kind::Default,e.token);payload.type=element_type(e.type);payload.lvalue=e.lvalue;payload.constant=e.constant;
            return standard_convert(target,payload,constant);
        }
        if(constant&&is_optional(t)&&e.type!=t)return standard_convert(t,e);
        // A const interface borrow can bind an owned, boxed conversion for the call.
        if(constant&&t=="System.IStringable")return stringable(e.type);
        return (e.lvalue||constant)&&(constant||!e.constant)&&(t==e.type||derives(e.type,t));
    }
    if(is_optional(target)) {
        if(e.type=="null")return true;
        Expr payload(Expr::Kind::Default,e.token);payload.type=is_optional(e.type)?element_type(e.type):e.type;
        payload.lvalue=e.lvalue;payload.constant=e.constant;
        return standard_convert(element_type(target),payload,constant);
    }
    if(target=="System.IStringable")return stringable(e.type);
    if(is_optional(e.type)) {
        if(!is_present(e))return false;
        Expr payload(Expr::Kind::Default,e.token);payload.type=element_type(e.type);payload.lvalue=e.lvalue;payload.constant=e.constant;
        return standard_convert(target,payload,constant);
    }
    if(target=="Arena"||e.type=="Arena")return target=="Arena"&&e.type=="Arena"&& &e==arena_initializer&&e.binding==Expr::Binding::Builtin&&e.owner=="Arena";
    if(is_pointer(target)&&is_pointer(e.type)) {
        auto a=element_type(target),b=element_type(e.type);
        return a==b||a=="void"||derives(b,a);
    }
    if(is_interface(program,target)) {
        if(target=="System.IStringable")return stringable(e.type);
        return derives(e.type,target);
    }
    return assignable(target,e.type);
}

std::vector<Analyzer::UserConversion> Analyzer::user_conversions(const std::string& target,const Expr& source,bool explicit_cast) {
    const auto wanted=is_optional(referred(target))?element_type(referred(target)):referred(target);
    if((is_reference(target)&&!source.constant)||is_optional(source.type)) {
        // Writable references cannot refer to a conversion temporary. Optional
        // lifting is handled by normal payload access, never unchecked unwrap.
        if(is_optional(source.type))return {};
    }
    if(source.type==wanted)return {};
    std::vector<UserConversion> result;
    if(auto* type=find_class(source.type)) {
        prepare(*type);
        std::unordered_set<std::string> seen;
        std::function<void(Class&)> visit=[&](Class& c){
            prepare(c);
            for(auto& m:c.methods)if(m.is_conversion&&m.prepared&&seen.insert(m.id).second) {
                if(!m.is_implicit&&!explicit_cast)continue;
                if(!member_accessible(c,m.is_public,m.is_protected))continue;
                if(source.constant&&!m.readonly_receiver&&std::none_of(m.attributes.begin(),m.attributes.end(),[](const auto& a){return a.name=="SnapshotReceiver";}))continue;
                Expr converted(Expr::Kind::Default,source.token);converted.type=m.type;
                if(m.type==wanted||standard_convert(wanted,converted))result.push_back({&c,&m,m.type==wanted?0:1,false});
            }
            if(!c.base.empty())visit(*find_class(c.base));
            for(const auto& i:c.interfaces)visit(*find_class(i));
        };
        visit(*type);
    }
    // A destination constructor is a user-defined conversion only when it
    // opts in with `implicit`. Explicit casts may also select the normal
    // explicit-by-default one-argument constructors. Its parameter may use
    // a standard conversion, but never another user-defined conversion.
    if(auto* destination=find_class(wanted);destination&&!destination->is_interface) {
        prepare(*destination);
        for(auto& m:destination->methods)if(m.is_constructor&&!m.is_specialization&&m.prepared) {
            if(!m.is_implicit&&!explicit_cast)continue;
            if(m.parameters.size()!=1||m.parameters[0].variadic||m.parameters[0].pack)continue;
            if(!member_accessible(*destination,m.is_public,m.is_protected))continue;
            const auto& parameter=m.parameters[0];
            if(!standard_convert(parameter.type,source,parameter.constant))continue;
            result.push_back({destination,&m,referred(parameter.type)==source.type?0:1,true});
        }
    }
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.cost<b.cost;});
    return result;
}

bool Analyzer::can_convert(const std::string& target,const Expr& e,bool constant) {
    if(e.lambda)return (!is_reference(target)||constant)&&lambda_matches(e,referred(target));
    if(e.type=="#method"&&(action_arity(referred(target))>=0||function_type(referred(target)))) {
        if(is_reference(target)&&!constant)return false;
        return delegate_compatible(e,referred(target));
    }
    if(standard_convert(target,e,constant))return true;
    if(is_reference(target)&&!constant)return false;
    return !user_conversions(target,e,false).empty();
}

bool Analyzer::apply_user_conversion(const std::string& target,Expr& e,bool explicit_cast) {
    auto matches=user_conversions(target,e,explicit_cast);
    if(matches.empty())return false;
    if(matches.size()>1&&matches[0].cost==matches[1].cost)
        throw Error(e.token.at,"ambiguous user-defined conversion from '"+e.type+"' to '"+target+"'", "BLT2602");
    auto selected=matches.front();auto& m=*selected.method;
    m.referenced=true;mark_type(selected.owner->name);for(const auto& p:m.parameters)mark_type(p.type);
    if(selected.constructor) {
        const auto token=e.token;
        auto argument=std::make_unique<Expr>(std::move(e));
        auto type=std::make_unique<Expr>(Expr::Kind::Name,token,selected.owner->short_name);
        type->binding=Expr::Binding::Class;type->type="#class";type->owner=selected.owner->name;
        e=Expr(Expr::Kind::Call,token);
        e.binding=Expr::Binding::Construct;e.type=selected.owner->name;e.declared_type=selected.owner->name;e.owner=selected.owner->name;e.resolved=m.id;
        e.args.push_back(std::move(type));e.args.push_back(std::move(argument));
        normalize_arguments(*selected.owner,m,e.args,1);
        return true;
    }
    mark_type(m.type);
    // General receiver policy, not a hard-coded IStringable conversion.
    // The existing stdlib formatter promises a value snapshot. An
    // annotated conversion can therefore be used on const values too.
    if(std::any_of(m.attributes.begin(),m.attributes.end(),[](const auto& a){return a.name=="SnapshotReceiver";})) {
        const auto token=e.token;
        // Snapshot through the declaring interface when this policy comes
        // from an interface default. A static base-value copy would slice
        // the dynamic object and change existing IStringable formatting.
        const auto* declared=find_class(m.default_owner.empty()?selected.owner->name:m.default_owner);
        const auto type=declared&&declared->is_interface?declared->name:e.type;
        const auto name="@conversion_receiver"+std::to_string(++property_temporary);
        auto receiver=std::make_unique<Expr>(std::move(e));
        auto member=std::make_unique<Expr>(Expr::Kind::Member,token,m.name);
        member->args.push_back(temporary_name(token,name));
        auto call=std::make_unique<Expr>(Expr::Kind::Call,token);call->args.push_back(std::move(member));
        e=Expr(Expr::Kind::Let,token,name);e.declared_type=type;
        e.args.push_back(std::move(receiver));e.args.push_back(std::move(call));
        expression(e);return true;
    }
    auto receiver=std::make_unique<Expr>(std::move(e));const auto token=receiver->token;
    auto method=std::make_unique<Expr>(Expr::Kind::Member,token,m.name);
    method->binding=Expr::Binding::Method;method->type="#method";method->owner=selected.owner->name;
    method->resolved=m.id;method->constant=receiver->constant;method->args.push_back(std::move(receiver));
    e=Expr(Expr::Kind::Call,token);e.binding=Expr::Binding::Method;e.type=m.type;e.declared_type=m.type;
    e.owner=selected.owner->name;e.resolved=m.id;e.args.push_back(std::move(method));
    // A conversion is an ordinary call and may invalidate escaped optional facts.
    kill_escaped();
    return true;
}

void Analyzer::validate_conversion_cycles() {
    struct Edge { const Class* to; const Method* method; };
    std::unordered_map<std::string,std::vector<Edge>> edges;
    for(const auto& c:program.classes) {
        if(!c.generic_parameters.empty()||!c.pack_parameter.empty()||c.is_interface)continue;
        for(const auto& m:c.methods)if(m.is_implicit&&m.prepared) {
            if(m.is_conversion) {
                if(auto* to=find_class(m.type);to&&!to->is_interface)edges[c.name].push_back({to,&m});
            } else if(m.is_constructor&&m.parameters.size()==1) {
                auto from=referred(m.parameters[0].type);if(is_optional(from))from=element_type(from);
                if(auto* source=find_class(from);source&&!source->is_interface)edges[source->name].push_back({&c,&m});
            }
        }
    }
    std::unordered_map<std::string,int> state;
    std::function<void(const Class&)> visit=[&](const Class& c){
        if(state[c.name]==2)return;
        state[c.name]=1;
        for(const auto& edge:edges[c.name]) {
            if(state[edge.to->name]==1)throw Error(edge.method->token.at,"cyclic implicit conversion graph involving '"+c.name+"' and '"+edge.to->name+"'", "BLT2603");
            if(!state[edge.to->name])visit(*edge.to);
        }
        state[c.name]=2;
    };
    for(const auto& c:program.classes)if((c.generic_parameters.empty()&&c.pack_parameter.empty())&&!c.is_interface&&!state[c.name])visit(c);
}

void Analyzer::require_primitive_formatter(const Expr& value) {
    const auto formatter=primitive_formatter(is_optional(value.type)?element_type(value.type):value.type);
    if(!formatter.owner)return;
    auto* formatting=find_class(formatter.owner);
    if(!formatting)throw Error(value.token.at,std::string("missing standard library class ")+formatter.owner);
    auto candidates=methods(*formatting,formatter.method);
    if(candidates.empty())throw Error(value.token.at,"missing standard library method "+formatting->name+"."+formatter.method);
    mark_type(formatting->name);
    candidates.front().second->referenced=true;
}

void Analyzer::convertible(const std::string& target,Expr& e,bool constant) {
    if(e.lambda)bind_lambda(e,referred(target));
    if(e.type=="#method"&&(action_arity(referred(target))>=0||function_type(referred(target)))&&(!is_reference(target)||constant))bind_delegate(e,referred(target));
    if(e.type=="#default")expression(e,false,target);
    value(e);
    const auto destination=referred(target);
    if(is_optional(e.type)&&!is_optional(destination)&&destination!="System.IStringable"&&is_present(e))extract_optional(e);
    if(!standard_convert(target,e,constant)&&(!is_reference(target)||constant))apply_user_conversion(target,e,false);
    if(!standard_convert(target,e,constant))throw Error(e.token.at,"cannot assign '"+e.type+"' to '"+target+"'"+(is_reference(target)?"; references require a compatible, non-null lvalue":""), "BLT2002");
    if(destination=="System.IStringable" || destination=="System.IStringable?")require_primitive_formatter(e);
    if(is_reference(target)&&constant&&is_optional(referred(target))&&e.type!=referred(target)) {
        auto value=std::make_unique<Expr>(std::move(e));const auto token=value->token;
        e=Expr(Expr::Kind::Call,token,"@explicit-cast");e.type=referred(target);e.binding=Expr::Binding::Cast;
        auto type=std::make_unique<Expr>(Expr::Kind::Name,token,e.type);type->binding=Expr::Binding::Cast;type->owner=e.type;type->type="#cast";
        e.args.push_back(std::move(type));e.args.push_back(std::move(value));
    }
    const bool numeric_limit=e.binding==Expr::Binding::Property&&is_numeric(e.owner);
    if((e.kind==Expr::Kind::Literal||numeric_limit)&&is_numeric(target)) {
        try {
            (void)convert_value(target,numeric_limit?numeric_limit_value(e.owner,e.text=="MaxValue"):literal_value(e));
        }catch(const Error&) {
            throw;
        }catch(const std::exception& ex) {
            throw Error(e.token.at,ex.what(), "BLT2000");
        }
    }
}

void Analyzer::writable(const Expr& e) const {
    if(!e.lvalue||e.binding==Expr::Binding::This)throw Error(e.token.at,"assignment requires a variable or mutable field", "BLT2000");
    if(e.constant)throw Error(e.token.at,"cannot modify a const value", "BLT2000");
}

} // namespace bolt::semantic
