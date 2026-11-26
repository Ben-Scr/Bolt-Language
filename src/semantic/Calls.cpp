#include "Analyzer.hpp"
#include <bolt/Types.hpp>
#include <algorithm>

namespace bolt::semantic {

void Analyzer::call_expression(Expr& e, bool callee, const std::string& expected, bool assignment_target) {
    using K = Expr::Kind;
    using B = Expr::Binding;
    auto& target=*e.args[0];
    if(e.text=="@explicit-cast") {
        target.binding=B::Cast;
        target.owner=resolve(target.text,current_class,target.token);
        if(target.owner=="Arena")throw Error(target.token.at,"Arena owners cannot be cast or copied", "BLT2300");
        if(is_reference(target.owner))throw Error(target.token.at,"reference casts are not supported; bind a T& variable to an existing value instead", "BLT2111");
        target.type="#cast";
    } else expression(target,true);
    if(target.binding==B::Event) {
        require_event_owner(target);lower_event_receiver(target);
        auto method=std::make_unique<Expr>(K::Member,target.token,"Raise");method->args.push_back(std::move(e.args[0]));e.args[0]=std::move(method);
        expression(e);return;
    }
    if(action_arity(target.type)>=0||function_type(target.type)) {
        auto method=std::make_unique<Expr>(K::Member,target.token,"Invoke");method->args.push_back(std::move(e.args[0]));e.args[0]=std::move(method);
        expression(e);return;
    }
    for(std::size_t i=1;i<e.args.size();++i) {
        expression(*e.args[i],false,(target.binding==B::Cast||target.binding==B::Enum)?target.owner:"");
        if(!e.args[i]->lambda&&e.args[i]->type!="#default"&&e.args[i]->type!="#method")value(*e.args[i]);
    }
    std::vector<Expr*> args;
    for(std::size_t i=1;i<e.args.size();++i)args.push_back(e.args[i].get());
    if(target.type=="#builtin" && (target.owner=="Math" || is_numeric(target.owner))) {
        // Check the surface signature before binding the specialized Bolt method.
        const auto library=target.owner=="Math"?"Bolt.Runtime.ScalarMath":"Bolt.Runtime.NumericParsing";
        builtin_call(e,target);
        auto* implementation=find_class(library);
        if(!implementation)throw Error(e.token.at,std::string("missing standard library class ")+library);
        mark_type(implementation->name);
        target.type_args=e.type_args;
        bind_member(target,*implementation,true,false,false);
    }
    if(target.type=="#extension") {
        std::vector<std::string> types;
        for(const auto& type:target.type_args)types.push_back(resolve(type,current_class,target.token,true));
        lower_extension_call(e,types);
    }else if(target.binding==B::Cast||target.binding==B::Enum) {
        if(args.size()!=1)throw Error(e.token.at,"type conversions require one argument", "BLT2000");
        e.binding=B::Cast;
        e.type=target.owner;
        // Boxing preserves the implementing value, including enums and
        // optionals. A string conversion on that value must not run first.
        if(is_interface(program,e.type) && standard_convert(e.type,*args[0])){
            convertible(e.type,*args[0]);
            finish_expression(e,callee,expected,assignment_target);
            return;
        }
        if(!is_optional(e.type)&&is_optional(args[0]->type)&&e.type!="string")extract_optional(*args[0]);
        if(args[0]->type!=e.type&&!is_optional(args[0]->type))apply_user_conversion(e.type,*args[0],true);
        if(e.type=="string") {
            if(!stringable(args[0]->type))throw Error(e.token.at,"string conversion requires a primitive, enum, or IStringable value", "BLT2204");
        } else if(auto* en=find_enum(e.type)) {
            if(!is_integer(args[0]->type)&&args[0]->type!=e.type)throw Error(e.token.at,"enum conversion requires an integer", "BLT2000");
            e.declared_type=en->underlying;
        } else if(find_enum(args[0]->type)) {
            if(!is_integer(e.type))throw Error(e.token.at,"enum conversion requires an integer type", "BLT2000");
        } else if((e.type=="char"&&is_integer(args[0]->type))||(args[0]->type=="char"&&is_integer(e.type))) {
            program.require_cpp("Unicode char conversions");
        } else if(is_pointer(e.type)&&is_pointer(args[0]->type)) {
            // Explicit casts may reinterpret an OS buffer's
            // pointee type. Implicit pointer conversions retain
            // the narrower rules in standard_convert().
        } else convertible(e.type,*args[0]);
    }else if(target.binding==B::Class) {
        e.binding=B::Construct;
        e.type=target.owner;
        if(const auto* c=find_class(e.type);c&&c->is_native)program.require_cpp("native standard library / interop");
        if(is_collection(e.type)) {
            if(!args.empty())throw Error(e.token.at,"collection construction expects no arguments", "BLT2000");
        } else {
            auto [owner,m]=constructor(e.type,args,e.token);
            if(m){e.resolved=m->id;normalize_arguments(*owner,*m,e.args,1);}
        }
    }else if(target.type=="#builtin"&&target.owner=="Primitive") {
        if(!args.empty()||!target.type_args.empty()) {
            if(!extension_methods(target.text,target.token).empty()) {
                std::vector<std::string> types;for(const auto& type:target.type_args)types.push_back(resolve(type,current_class,target.token,true));
                lower_extension_call(e,types);remember_condition(e);return;
            }
            throw Error(e.token.at,"ToString() takes no arguments or type parameters", "BLT2204");
        }
        auto receiver=std::move(target.args[0]);
        if(target.arrow) {
            auto deref=std::make_unique<Expr>(K::Unary,target.token,"*");
            deref->type=element_type(receiver->type);deref->lvalue=true;
            deref->args.push_back(std::move(receiver));receiver=std::move(deref);
        }
        e.args.clear();e.args.push_back(std::make_unique<Expr>(K::Name,e.token,"string"));
        e.args.push_back(std::move(receiver));e.binding=B::Cast;e.type="string";
    }else if(target.type=="#builtin")builtin_call(e,target);
    else if(target.type=="#collection")collection_call(e,target);
    else if(target.binding==B::Method) {
        auto* c=find_class(target.owner);
        auto candidates=methods(*c,target.text);
        if(!target.callback_method.empty() && generated_metadata(target.token)) {
            candidates.clear();
            for(auto& method:c->methods)if(method.id==target.callback_method)
                candidates.emplace_back(c,&method);
        }
        std::vector<std::string> type_args;
        for(const auto& t:target.type_args)type_args.push_back(resolve(t,current_class,target.token,true));
        if(c->name=="Ecs.Registry"&&target.text=="Add"&&type_args.size()==1&&e.args.size()>2&&
           (e.args.size()!=3||e.args[2]->type!=type_args[0])){
            auto construct=std::make_unique<Expr>(K::Call,e.token);
            auto name=std::make_unique<Expr>(K::Name,e.token,generic_base(type_args[0]));name->type_args=generic_args(type_args[0]);construct->args.push_back(std::move(name));
            for(std::size_t i=2;i<e.args.size();++i)construct->args.push_back(std::move(e.args[i]));
            e.args.resize(2);expression(*construct);e.args.push_back(std::move(construct));
            args.clear();args.push_back(e.args[1].get());args.push_back(e.args[2].get());
        }
        Method* pack=nullptr;
        Class* pack_owner=nullptr;
        for(auto [owner,m]:candidates)if(!m->pack_parameter.empty()&&!m->native_pack&&!heterogeneous_pack(*m)&&type_args.size()>=m->generic_parameters.size()&&m->parameters.size()==args.size()) {
            if(pack)throw Error(e.token.at,"ambiguous variadic method", "BLT2000");
            pack=m;
            pack_owner=owner;
        }
        if(pack) {
            Expr replacement(K::Pack,e.token);
            replacement.type="void";
            for(std::size_t i=pack->generic_parameters.size();i<type_args.size();++i) {
                std::vector<std::string> current(type_args.begin(),type_args.begin()+static_cast<std::ptrdiff_t>(pack->generic_parameters.size()));
                current.push_back(type_args[i]);
                auto [owner,m]=select( {
                    {
                        pack_owner,pack
                    }
                },current,args,e.token,true);
                accessible(*owner,m->is_public,m->is_protected,target);
                if(target.is_static!=m->is_static)throw Error(e.token.at,"incorrect static/instance receiver", "BLT2000");
                auto call=clone(e);
                call->binding=B::Method;
                call->type="void";
                call->owner=owner->name;
                call->resolved=m->id;
                call->args[0]->owner=owner->name;
                call->args[0]->resolved=m->id;
                normalize_arguments(*owner,*m,call->args,1);
                replacement.args.push_back(std::move(call));
                for(auto* argument:args)if(argument->kind==K::Default&&argument->constant_owner.empty())argument->type="#default";
            }
            e=std::move(replacement);
            return;
        }
        const bool extension_fallback=target.kind==K::Member&&!target.is_static&&!extension_methods(target.text,target.token).empty();
        if(extension_fallback)std::erase_if(candidates,[&](const auto& candidate){return candidate.second->is_static||!member_accessible(*candidate.first,candidate.second->is_public,candidate.second->is_protected);});
        auto [owner,m]=select(std::move(candidates),type_args,args,e.token,false,extension_fallback);
        if(!m){lower_extension_call(e,type_args);remember_condition(e);return;}
        accessible(*owner,m->is_public,m->is_protected,target);
        if(target.is_static!=m->is_static)throw Error(e.token.at,m->is_static?"access static methods through their class name":"instance method requires an object", "BLT2000");
        if(!m->is_static&&target.constant&&!m->is_extern&&!m->readonly_receiver) {
            const bool snapshot=std::any_of(m->attributes.begin(),m->attributes.end(),[](const auto& a){return a.name=="SnapshotReceiver";});
            if(!snapshot||target.kind!=K::Member||target.arrow||is_reference(m->type))throw Error(e.token.at,"cannot call a non-const instance method on a const value", "BLT2000");
            const auto name="@snapshot_receiver"+std::to_string(++property_temporary);
            auto receiver=std::move(target.args[0]);const auto type=receiver->type;
            target.args[0]=temporary_name(target.token,name);
            auto invocation=std::make_unique<Expr>(std::move(e));
            auto let=std::make_unique<Expr>(K::Let,invocation->token,name);let->declared_type=type;
            let->args.push_back(std::move(receiver));let->args.push_back(std::move(invocation));
            e=std::move(*let);expression(e);return;
        }
        normalize_arguments(*owner,*m,e.args,1);
        if(m->is_extern)program.require_cpp("native standard library / interop");
        if(owner->name=="Bolt.Runtime.ThreadCallback" && owner->native_cpp=="bolt::platform::ThreadCallback" && m->name=="Capture") {
            auto* handler=find_class(referred(args[0]->type));
            if(!handler||handler->is_native||handler->is_interface||handler->is_abstract)throw Error(e.token.at,"Thread.Start needs a concrete value job with public void Run(). Interfaces, abstract jobs and raw pointers cannot be worker captures.","BLT2401");
            std::vector<Expr*> actual;auto [co,cm]=select(methods(*handler,"Run"),{},actual,e.token);
            if(!cm->is_public||cm->is_static||cm->is_abstract||cm->type!="void"||!cm->parameters.empty())throw Error(e.token.at,"Worker Run must be concrete, public, non-static, parameterless and return void.","BLT2401");
            e.callback_owner=co->name;e.callback_method=cm->id;
        }
        if(generic_base(owner->name)=="Ecs.ComponentSignal"&&(m->name=="Connect"||m->name=="ConnectRef")) {
            auto* handler=find_class(args[0]->type);if(!handler)throw Error(e.token.at,"A component callback needs an object with public void OnAdd(Registry&, Entity, T&)","BLT2401");
            Expr registry(K::Name,e.token),entity(K::Name,e.token),component(K::Name,e.token);
            registry.type="Ecs.Registry";registry.lvalue=true;entity.type="Ecs.Entity";entity.lvalue=true;component.type=generic_args(owner->name).front();component.lvalue=true;
            std::vector<Expr*> actual{&registry,&entity,&component};auto [callback_owner,callback]=select(methods(*handler,"OnAdd"),{},actual,e.token);
            if(!callback->is_public||callback->is_static||callback->type!="void")throw Error(e.token.at,"OnAdd callback must be public, non-static and return void","BLT2401");
            e.callback_owner=callback_owner->name;e.callback_method=callback->id;
        }
        e.binding=B::Method;
        e.type=referred(m->type);
        e.declared_type=m->type;
        e.lvalue=is_reference(m->type);
        if(e.lvalue&&!m->is_static&&target.kind==K::Member&&!target.arrow&&!target.args[0]->lvalue)
            throw Error(e.token.at,"cannot obtain a reference from a temporary receiver", "BLT2000");
        e.owner=owner->name;
        e.resolved=m->id;
        target.owner=owner->name;
        target.resolved=m->id;
    }else throw Error(e.token.at,"expression is not callable", "BLT2000");
    // Primitive ToString and Converter.ToString both reach this string cast.
    if(e.binding==B::Cast && e.type=="string")require_primitive_formatter(*e.args[1]);
    finish_expression(e, callee, expected, assignment_target);
}

} // namespace bolt::semantic
