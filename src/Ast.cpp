#include <bolt/Ast.hpp>
#include <bolt/Types.hpp>
#include <unordered_set>
#include <functional>
namespace bolt {
std::string attribute_text(const std::vector<Attribute>& attributes) {
    std::string result;
    for(const auto& a:attributes){if(!result.empty())result+="\n";result+=a.name;if(!a.arguments.empty()){result+="(";for(std::size_t i=0;i<a.arguments.size();++i){if(i)result+=",";result+=a.arguments[i];}result+=")";}}
    return result;
}

    bool derives_from(const Program& p,const std::string& from,const std::string& to) {
        std::unordered_set<std::string> seen;
        std::function<bool(const std::string&)> test=[&](const std::string& t) {
            if(t==to)return true;
            if(!seen.insert(t).second)return false;
            const auto* c=p.find_class(t); if(!c)return false;
            if(!c->base.empty()&&test(c->base))return true;
            for(const auto& i:c->interfaces)if(test(i))return true;
            return false;
        };
        return test(from);
    }
    bool stringable_type(const Program& p,const std::string& type) {
        if(is_reference(type))return stringable_type(p,referred(type));
        if(is_pointer(type))return true;
        if(is_optional(type))return stringable_type(p,element_type(type));
        if(is_collection(type)) {
            for(const auto& argument:generic_args(type))if(!stringable_type(p,argument))return false;
            return true;
        }
        if(const auto* c=p.find_class(type);c)for(const auto& a:c->attributes)if(a.name=="StringableElements")
            for(const auto& argument:generic_args(type))if(!stringable_type(p,argument))return false;
        if(const auto* c=p.find_class(type);c&&c->is_native){
            for(const auto& m:c->methods)if(m.name=="ToString"&&m.type=="string"&&m.is_public&&!m.is_static&&m.parameters.empty())return true;
        }
        return is_primitive(type)||p.find_enum(type)||derives_from(p,type,"System.IStringable");
    }

    ExprPtr clone(const Expr& e) {
        auto r=std::make_unique<Expr>(e.kind,e.token,e.text);
        r->binding=e.binding;
        r->type_args=e.type_args;r->callback_owner=e.callback_owner;r->callback_method=e.callback_method;
        r->dimensions=e.dimensions;
        r->type=e.type;
        r->owner=e.owner;
        r->resolved=e.resolved;
        r->declared_type=e.declared_type;
        r->constant_owner=e.constant_owner;
        r->constant_token=e.constant_token;
        r->local_id=e.local_id;
        r->lvalue=e.lvalue;
        r->constant=e.constant;
        r->is_static=e.is_static;
        r->is_extension=e.is_extension;
        r->arrow=e.arrow;r->authorized_event_access=e.authorized_event_access;
        if(e.arena)r->arena=clone(*e.arena);
        if(e.lambda) {
            r->lambda=std::make_unique<LambdaSyntax>();
            r->lambda->parameters=e.lambda->parameters;
            r->lambda->body=clone(*e.lambda->body);
            r->lambda->expression_body=e.lambda->expression_body;
            r->lambda->captures=e.lambda->captures;
            r->lambda->free_names=e.lambda->free_names;
        }
        for(const auto& a:e.args) r->args.push_back(clone(*a));
        return r;
    }
    StmtPtr clone(const Stmt& s) {
        auto r=std::make_unique<Stmt>(s.kind,s.token);
        r->type=s.type;
        r->name=s.name;
        r->name_token=s.name_token;
        r->constant=s.constant;
        if(s.expression) r->expression=clone(*s.expression);
        if(s.condition) r->condition=clone(*s.condition);
        if(s.increment) r->increment=clone(*s.increment);
        if(s.init) r->init=clone(*s.init);
        if(s.body) r->body=clone(*s.body);
        if(s.alternative) r->alternative=clone(*s.alternative);
        for(const auto& a:s.statements) r->statements.push_back(clone(*a));
        return r;
    }
    Method clone(const Method& m) {
        Method r;
        r.type=m.type;
        r.name=m.name;
        r.token=m.token;
        r.is_public=m.is_public;
        r.is_protected=m.is_protected;
        r.is_static=m.is_static;
        r.is_constructor=m.is_constructor;
        r.is_sealed=m.is_sealed;
        r.is_override=m.is_override;
        r.is_virtual=m.is_virtual;
        r.is_abstract=m.is_abstract;
        r.is_constant_template=m.is_constant_template;
        r.operator_symbol=m.operator_symbol;
        r.is_destructor=m.is_destructor;r.is_async=m.is_async;r.is_extern=m.is_extern;
        r.is_extension=m.is_extension;
        r.is_conversion=m.is_conversion;r.is_implicit=m.is_implicit;
        r.is_default_implementation=m.is_default_implementation;r.default_owner=m.default_owner;
        r.property_name=m.property_name;r.is_getter=m.is_getter;r.is_setter=m.is_setter;r.readonly_receiver=m.readonly_receiver;
        r.native_name=m.native_name;r.native_abi=m.native_abi;r.native_pack=m.native_pack;r.native_pack_min=m.native_pack_min;
        r.specialization_types=m.specialization_types;r.attributes=m.attributes;
        r.generic_parameters=m.generic_parameters;
        r.pack_parameter=m.pack_parameter;
        r.constraints=m.constraints;
        r.parameters=m.parameters;
        for(auto& p:r.parameters)if(p.default_value)p.default_value=clone(*p.default_value);
        r.explicit_base=m.explicit_base;
        for(const auto& a:m.base_arguments) r.base_arguments.push_back(clone(*a));
        for(const auto& a:m.constructor_initializers) r.constructor_initializers.push_back(clone(*a));
        for(const auto& [name,a]:m.field_initializers) r.field_initializers.emplace_back(name,clone(*a));
        if(m.body) r.body=clone(*m.body);
        return r;
    }
    Class clone(const Class& c) {
        Class r;
        r.name=c.name;
        r.short_name=c.short_name;
        r.namespace_name=c.namespace_name;
        r.declaring_type=c.declaring_type;r.is_public=c.is_public;r.is_template_member=c.is_template_member;
        r.base=c.base;
        r.interfaces=c.interfaces;
        r.is_interface=c.is_interface;
        r.reachable=c.reachable;r.is_struct=c.is_struct;r.is_abstract=c.is_abstract;r.is_reflected=c.is_reflected;
        r.is_native=c.is_native;r.native_cpp=c.native_cpp;r.attributes=c.attributes;
        r.delegate_owner=c.delegate_owner;r.delegate_method=c.delegate_method;
        r.token=c.token;
        r.is_static=c.is_static;
        r.is_sealed=c.is_sealed;
        r.is_global=c.is_global;
        r.imports=c.imports;
        r.generic_parameters=c.generic_parameters;
        r.generic_defaults=c.generic_defaults;
        r.pack_parameter=c.pack_parameter;
        r.constraints=c.constraints;
        r.properties=c.properties;
        for(const auto& f:c.fields) {
            Field v;
            v.type=f.type;
            v.name=f.name;
            v.token=f.token;
            v.is_public=f.is_public;
            v.is_protected=f.is_protected;
            v.is_static=f.is_static;
            v.constant=f.constant;
            v.native_getter=f.native_getter;v.attributes=f.attributes;v.is_event=f.is_event;v.event_type=f.event_type;
            if(f.initializer) v.initializer=clone(*f.initializer);
            r.fields.push_back(std::move(v));
        }
        for(const auto& m:c.methods) r.methods.push_back(clone(m));
        return r;
    }
}
// namespace bolt
