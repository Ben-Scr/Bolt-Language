#include "Analyzer.hpp"
#include "Substitution.hpp"
#include <bolt/TypeAccess.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <functional>
#include <optional>

namespace bolt::semantic {

Class* Analyzer::find_class(const std::string& name) const {
    const auto found=classes_by_name.find(name);
    return found==classes_by_name.end()?nullptr:found->second;
}

Enum* Analyzer::find_enum(const std::string& name) const {
    const auto found=enums_by_name.find(name);
    return found==enums_by_name.end()?nullptr:found->second;
}

void Analyzer::append_class(Class value) {
    auto& added=program.classes.emplace_back(std::move(value));
    classes_by_name.try_emplace(added.name,&added);
}

std::string Analyzer::fullname(const std::string& ns,const std::string& name) {
    return ns.empty()?name:ns+"."+name;
}

Class* Analyzer::member_context() const {return current_method&&!current_method->default_owner.empty()?find_class(current_method->default_owner):current_class;}

bool Analyzer::imported(const Class* c,const std::string& name) const {
    if(c==current_class)c=member_context();
    return c&&std::find(c->imports.begin(),c->imports.end(),name)!=c->imports.end();
}

std::string Analyzer::resolve_name(const std::string& n,const Class* context) const {
    if(context==current_class)context=member_context();
    auto exists=[&](const std::string& name) {
        return classes_by_name.contains(name)||enums_by_name.contains(name);
    };
    // Search lexical type scopes before namespace/global declarations. A
    // nested type supplies names only; it never supplies an outer receiver.
    std::unordered_set<std::string> visited;
    for(auto* scope=context;scope;) {
        if(!scope->name.empty()) {
            const auto candidate=fullname(scope->name,n);
            if(exists(candidate))return candidate;
            if(n==scope->short_name&&exists(scope->name))return scope->name;
        }
        if(scope->declaring_type.empty() || !visited.insert(scope->name).second) break;
        scope=find_class(scope->declaring_type);
    }
    if(context&&!context->namespace_name.empty()&&exists(fullname(context->namespace_name,n)))return fullname(context->namespace_name,n);
    if(exists(n))return n;
    if(n=="IStringable"&&exists("System.IStringable"))return "System.IStringable";
    if(n.find('.')==std::string::npos) {
        for(const auto& prefix:{"System.","System.IO.","System.Diagnostics.","System.Threading.","System.Threading.Tasks.","System.Reflection.","System.Memory.","System.Text.Json.","System.Collections.Generic.","Ecs."}) {
            auto candidate=std::string(prefix)+n;const auto it=classes_by_name.find(candidate);
            const auto* c=it==classes_by_name.end()?nullptr:it->second;
            if(c&&c->token.at.file.starts_with("<bolt-stdlib>")) {
                const bool implicit=program.prelude_enabled&&(derives_from(program,candidate,"System.Exception")||candidate=="System.Exception"||n=="Task"||n=="Console"||n=="Action"||n=="Func");
                const auto ns=std::string(prefix).substr(0,std::string(prefix).size()-1);
                if(implicit||imported(context,n)||imported(context,ns)||imported(context,"System")||(ns=="Ecs"&&imported(context,"Ecs")))return candidate;
            }
        }
    }
    std::string found;
    if(context)for(const auto& imp:context->imports) {
        const auto candidate=fullname(imp,n);
        if(exists(candidate)) {
            if(!found.empty()&&found!=candidate)throw Error(context->token.at,"ambiguous type '"+n+"'; use a fully qualified name", "BLT2000");
            found=candidate;
        }
    }
    return found;
}

void Analyzer::require_type_access(const std::string& name,const Class* context,const Token& at) const {
    if(generated_metadata(at))return;
    // A synthetic delegate adapter only repeats types from its checked target,
    // captures and arguments. The user's lambda body is checked in its owner.
    if(context && !context->delegate_owner.empty() && context->token.at.file==at.at.file &&
       at.at.file.starts_with("<bolt-generated>/"))return;
    // These library signatures contain application types only through generic
    // arguments. Their names were checked at the caller (or reflected member).
    // Rebinding the substituted signature must not require file-level access
    // to the application type from the embedded library file.
    if(context && context->token.at.file==at.at.file &&
       ((context->name=="System.Reflection.Reflection" && at.at.file=="<bolt-stdlib>/System/Reflection.bolt") ||
        (context->name=="System.Text.Json.Json" && at.at.file=="<bolt-stdlib>/System/Json.bolt") ||
        (context->name=="Bolt.Runtime.ValueTree" && at.at.file=="<bolt-stdlib>/System/PlatformValues.bolt")))return;
    if(const auto* c=find_class(name);c && c->is_public && c->declaring_type.empty())return;
    if(context && at.at.file.starts_with("<bolt-stdlib>/") && context->token.at.file==at.at.file){
        // The caller may use a file-private task result or worker. Rebinding
        // that same argument inside an instantiated library signature/body
        // preserves its already-checked identity across nested generic calls.
        auto substituted=[&](const std::string& type){return mentions_type(type,name);};
        for(const Class* scope=context;scope;scope=find_class(scope->declaring_type)){
            auto arguments=generic_args(scope->name);
            if(std::any_of(arguments.begin(),arguments.end(),substituted))return;
        }
        if(std::any_of(signature_arguments.begin(),signature_arguments.end(),substituted))return;
        if(current_method && current_method->token.at.file==at.at.file &&
           std::any_of(current_method->specialization_types.begin(),current_method->specialization_types.end(),substituted))return;
    }
    if(context==current_class)context=member_context();
    const auto error=type_access_error(program,name,at.at.file,context?context->name:std::string{});
    if(!error.empty())throw Error(at.at,error,"BLT2005");
}

void Analyzer::validate_source_type_access(bool scopes_known) {
    for(const auto& source:program.source_types) {
        if(is_primitive(source.name)||source.name=="void"||source.name=="Arena")continue;
        if(scopes_known && dependent_source_types.contains({source.token.at.file,source.token.at.offset}))continue;
        const auto* context=find_class(source.owner);
        std::optional<Class> scope;
        if(!context) {
            scope.emplace();
            scope->namespace_name=source.namespace_name;
            scope->imports=source.imports;
            context=&*scope;
        }
        // Dependent parameters are resolved when their template is instantiated.
        bool parameter=false;
        for(auto* parent=context;parent;) {
            parameter|=std::find(parent->generic_parameters.begin(),parent->generic_parameters.end(),source.name)!=parent->generic_parameters.end()||parent->pack_parameter==source.name;
            parent=find_class(parent->declaring_type);
        }
        if(parameter)continue;
        try {
            auto name=resolve_name(source.name,context);
            if(name.empty())name=resolve_name(generic_declaration_name(source.name),context);
            if(!name.empty())require_type_access(name,context,source.token);
        } catch(const Error&) {
            // Executable builds only need the lexical parameter scopes when
            // a parameter could shadow an ambiguous or inaccessible type.
            if(scopes_known)throw;
            navigation_bindings(false);
            scopes_known=true;
            if(!dependent_source_types.contains({source.token.at.file,source.token.at.offset}))throw;
        }
    }
    auto attributes=[&](const std::vector<Attribute>& values,const Class* context) {
        for(const auto& attribute:values) {
            if(std::find(builtin_attribute_names().begin(),builtin_attribute_names().end(),attribute.name)!=builtin_attribute_names().end())continue;
            auto name=resolve_name(attribute.name,context);
            if(name.empty())name=resolve_name(attribute.name+"Attribute",context);
            if(!name.empty())require_type_access(name,context,attribute.token);
        }
    };
    for(const auto& c:program.classes) {
        attributes(c.attributes,&c);
        for(const auto& f:c.fields)attributes(f.attributes,&c);
        for(const auto& p:c.properties)attributes(p.attributes,&c);
        for(const auto& m:c.methods)attributes(m.attributes,&c);
    }
    for(const auto& en:program.enums) {
        Class scope;scope.name=en.declaring_type;scope.namespace_name=en.namespace_name;scope.imports=en.imports;
        attributes(en.attributes,&scope);
    }
}

// Template bodies still have non-dependent names. Check visibility now,
// while leaving dependent expression typing to ordinary instantiation.
void Analyzer::validate_template_type_access() {
    for(auto& c:program.classes) {
        const bool class_template=c.is_template_member||!c.generic_parameters.empty()||!c.pack_parameter.empty();
        if (!class_template && std::none_of(c.methods.begin(), c.methods.end(), [](const Method& method) {
            return !method.generic_parameters.empty() || !method.pack_parameter.empty();
        })) continue;
        std::unordered_set<std::string_view> names;
        for(const auto* owner=&c;owner;owner=find_class(owner->declaring_type)) {
            for(const auto& parameter:owner->generic_parameters)names.insert(parameter);
            if(!owner->pack_parameter.empty())names.insert(owner->pack_parameter);
        }
        bool dependent_base=false;
        std::unordered_set<std::string> visited;
        std::function<void(const Class&)> members=[&](const Class& scope) {
            if(!visited.insert(scope.name).second)return;
            for(const auto& field:scope.fields)names.insert(field.name);
            for(const auto& property:scope.properties)names.insert(property.name);
            for(const auto& method:scope.methods)if(!method.is_constructor)names.insert(method.name);
            auto inherit=[&](const std::string& name) {
                const auto resolved=resolve_name(generic_declaration_name(name),&scope);
                if(const auto* base=find_class(resolved))members(*base);
                else dependent_base=true;
            };
            if(!scope.base.empty())inherit(scope.base);
            for(const auto& base:scope.interfaces)inherit(base);
        };
        members(c);
        // Global and implicitly imported functions precede type expressions.
        if(const auto* global=find_class(c.namespace_name.empty()?"@Global":"@Global."+c.namespace_name);global)
            for(const auto& method:global->methods)names.insert(method.name);
        for(const auto& name:program.implicit_static_imports)if(const auto* owner=find_class(name))
            for(const auto& method:owner->methods)if(method.is_public&&method.is_static)names.insert(method.name);
        for(const auto* builtin:{"this","Random","Runtime","Math","Type","Arena"})names.insert(builtin);

        // The syntax tree owns the names throughout this read-only walk. Only
        // newly visible names need to be removed when a lexical scope ends.
        std::vector<std::string_view> introduced;
        auto declare = [&](std::string_view name) {
            if (names.insert(name).second) introduced.push_back(name);
        };
        auto leave_scope = [&](std::size_t mark) {
            while (introduced.size() > mark) {
                names.erase(introduced.back());
                introduced.pop_back();
            }
        };
        std::function<void(const Expr&)> expression_access=[&](const Expr& expression) {
            for(const auto& argument:expression.args)expression_access(*argument);
            if(expression.arena)expression_access(*expression.arena);
            const auto path=qualified_expr(expression);
            if(path.empty()||dependent_base||names.contains(std::string_view(path).substr(0,path.find('.'))))return;
            const auto name=resolve_name(path,&c);
            if(name.empty())return;
            if(const auto* type=find_class(name))
                program.semantic_references.push_back({expression.token,"class:"+type->name,type->short_name,type->is_interface?"interface":type->is_struct?"struct":"class",type->name,type->declaring_type.empty()?type->namespace_name:type->declaring_type,type->token.at});
            else if(const auto* type=find_enum(name))
                program.semantic_references.push_back({expression.token,"enum:"+type->name,type->short_name,"enum",type->name,type->declaring_type.empty()?type->namespace_name:type->declaring_type,type->token.at});
            require_type_access(name,&c,expression.token);
        };
        std::function<void(const Stmt&)> statement_access=[&](const Stmt& statement) {
            using K=Stmt::Kind;
            const auto mark = introduced.size();
            if(statement.kind==K::Block||statement.kind==K::For) {
                if(statement.init)statement_access(*statement.init);
                if(statement.condition)expression_access(*statement.condition);
                if(statement.increment)expression_access(*statement.increment);
                for(const auto& child:statement.statements)statement_access(*child);
                if(statement.body)statement_access(*statement.body);
                leave_scope(mark);
                return;
            }
            if(statement.kind==K::Declarations){for(const auto& child:statement.statements)statement_access(*child);return;}
            if(statement.expression)expression_access(*statement.expression);
            if(statement.condition)expression_access(*statement.condition);
            if(statement.increment)expression_access(*statement.increment);
            if(statement.kind==K::Variable){declare(statement.name);return;}
            if(statement.kind==K::ForEach||statement.kind==K::Catch)declare(statement.name);
            if(statement.init)statement_access(*statement.init);
            if(statement.body)statement_access(*statement.body);
            leave_scope(mark);
            if(statement.alternative){statement_access(*statement.alternative);leave_scope(mark);}
            for(const auto& child:statement.statements){statement_access(*child);leave_scope(mark);}
        };
        if(class_template)for(const auto& field:c.fields)if(field.initializer)expression_access(*field.initializer);
        for(const auto& method:c.methods) {
            if(!class_template&&method.generic_parameters.empty()&&method.pack_parameter.empty())continue;
            for(const auto& parameter:method.generic_parameters)declare(parameter);
            if(!method.pack_parameter.empty())declare(method.pack_parameter);
            for(const auto& parameter:method.parameters)declare(parameter.name);
            for(const auto& parameter:method.parameters)if(parameter.default_value)expression_access(*parameter.default_value);
            for(const auto& expression:method.base_arguments)expression_access(*expression);
            for(const auto& expression:method.constructor_initializers)expression_access(*expression);
            for(const auto& [field,expression]:method.field_initializers)expression_access(*expression);
            if(method.body)statement_access(*method.body);
            leave_scope(0);
        }
    }
}

std::string Analyzer::resolve(std::string t,Class* context,const Token& at,bool allow_void) {
    if(is_reference(t)||is_pointer(t)||is_array(t)||is_optional(t)) {
        const auto suffix=is_array(t)?array_suffix(array_rank(t)):std::string(1,t.back());
        auto inner=resolve(element_type(t),context,at,suffix=="*");
        if(suffix=="?"&&(inner=="void"||inner=="Arena"||is_reference(inner)||is_optional(inner)))throw Error(at.at,"optional payloads must be non-void values, not references, Arena owners, or another optional", "BLT2400");
        if(is_reference(inner))throw Error(at.at,"references cannot be array elements, pointer targets, or nested references", "BLT2000");
        if(inner=="Arena"&&suffix!="&")throw Error(at.at,"Arena owners cannot be pointer targets or array elements", "BLT2300");
        if(inner=="void"&&suffix!="*")throw Error(at.at,"void cannot be stored", "BLT2000");
        return inner+suffix;
    }
    if(is_primitive(t)||t=="Arena"||(allow_void&&t=="void")){
        if(t=="char"&&context&&!context->is_native)program.require_cpp("Unicode char");return t;
    }
    auto args=generic_args(t);
    auto base=generic_base(t);
    int nesting=0;std::size_t dot=std::string::npos;
    for(std::size_t i=0;i<base.size();++i) {
        if(base[i]=='<')++nesting;else if(base[i]=='>')--nesting;
        else if(base[i]=='.'&&nesting==0)dot=i;
    }
    if(dot!=std::string::npos&&base.substr(0,dot).find('<')!=std::string::npos)
        base=resolve(base.substr(0,dot),context,at)+base.substr(dot);
    if((base=="Task"||base=="System.Threading.Tasks.Task")&&args.empty())args.push_back("void");
    if(base=="Buffer"&&resolve_name(base,context)=="Bolt.Runtime.Buffer")base="Bolt.Runtime.Buffer";
    if(!collection_kind(base).empty()) {
        const auto kind=collection_kind(base);
        if(base.find('.')==std::string::npos&&!imported(context,kind=="List"?"Bolt.Runtime":"System.Collections.Generic"))throw Error(at.at,"import System.Collections.Generic or use a fully qualified collection type", "BLT2000");
        const std::size_t count=kind=="Dictionary"?2:1;
        if(args.size()!=count)throw Error(at.at,kind+" requires "+std::to_string(count)+" generic type argument(s)", "BLT2000");
        for(auto& a:args) {
            a=resolve(a,context,at);
            if(referred(a)=="Arena")throw Error(at.at,"Arena owners cannot be collection elements", "BLT2300");
            if(is_reference(a))throw Error(at.at,"collections store values, not reference elements", "BLT2000");
        }
        return generic_name(kind=="List"?"Bolt.Runtime.Buffer":"System.Collections.Generic."+kind,args);
    }
    auto found=resolve_name(base,context);
    if(!args.empty()&&generic_base(found)!=found)found=generic_base(found);
    if(found.empty())throw Error(at.at,"unknown type '"+t+"'", "BLT2001");
    require_type_access(found,context,at);
    if(const auto* en=find_enum(found)) {
        if(en->is_template_member)throw Error(at.at,"nested type requires arguments for its enclosing generic type", "BLT2000");
        if(!args.empty())throw Error(at.at,"an enum cannot have generic arguments", "BLT2000");
        return found;
    }
    auto* c=find_class(found);
    if(c->is_native&&context&&!context->is_native&&!at.at.file.starts_with("<bolt-stdlib>"))program.require_cpp("native standard library / interop");
    if(c->is_template_member)throw Error(at.at,"nested type requires arguments for its enclosing generic type", "BLT2000");
    if(c->generic_parameters.empty()&&c->pack_parameter.empty()) {
        if(!args.empty())throw Error(at.at,"type '"+found+"' is not generic", "BLT2000");
        if(c->is_static)throw Error(at.at,"static class '"+found+"' cannot be used as a value type", "BLT2000");
        return found;
    }
    const auto explicit_count=args.size();
    const auto first_default=c->generic_parameters.size()-c->generic_defaults.size();
    if(explicit_count>=first_default && explicit_count<c->generic_parameters.size()) {
        for(size_t i=explicit_count;i<c->generic_parameters.size();++i)
            args.push_back(c->generic_defaults[i-first_default]);
    }
    if(args.size()<c->generic_parameters.size()||(c->pack_parameter.empty()&&args.size()!=c->generic_parameters.size()))throw Error(at.at,"incorrect generic arity for '"+found+"'", "BLT2000");
    for(size_t i=0;i<args.size();++i) {
        auto& a=args[i];
        const bool allow_void_argument=found=="System.Threading.Tasks.Task"||found=="Bolt.Runtime.TaskState";
        if(i<explicit_count) a=resolve(a,context,at,allow_void_argument);
        else {
            const auto key=std::make_pair(found,i);
            if(!resolving_defaults.insert(key).second)throw Error(at.at,"cyclic generic type default for "+found,"BLT2000");
            try {
                Substitution earlier;
                for(size_t j=0;j<i;++j)earlier[c->generic_parameters[j]]=args[j];
                a=resolve(substitute(a,earlier),c,at,allow_void_argument);
            } catch(...) {resolving_defaults.erase(key);throw;}
            resolving_defaults.erase(key);
        }
        if(referred(a)=="Arena")throw Error(at.at,"Arena owners cannot be collection elements or generic class arguments", "BLT2300");
    }
    const auto instance_name=args.empty()?found+"<>":generic_name(found,args);
    if(find_class(instance_name))return instance_name;
    if(++instantiations>1024)throw Error(at.at,"generic instantiation limit exceeded (1024)", "BLT2000");
    auto instance=clone(*c);
    instance.name=instance_name;
    instance.generic_parameters.clear();
    instance.generic_defaults.clear();
    instance.pack_parameter.clear();
    if(instance.is_native)instance.native_cpp=args.empty()?instance.native_cpp+"<>":generic_name(instance.native_cpp,args);
    Substitution map;
    for(std::size_t i=0;i<c->generic_parameters.size();++i)map[c->generic_parameters[i]]=args[i];
    if(!c->pack_parameter.empty())map.type_packs[c->pack_parameter]={args.begin()+static_cast<std::ptrdiff_t>(c->generic_parameters.size()),args.end()};
    if(found=="System.Func") {
        map["TResult"]=args.back();
        map.type_packs["TArgs"]={args.begin(),args.end()-1};
    }
    substitute(instance, map);
    if((found=="System.Threading.Tasks.Task" || found=="Bolt.Runtime.TaskState") && args==std::vector<std::string>{"void"})
        std::erase_if(instance.methods,[](const Method& m){return m.name=="SetResult";});
    if(found=="Bolt.Runtime.TaskState" && args==std::vector<std::string>{"void"} && !instance.is_native){
        auto observe=find_method(instance,"Observe");
        for(auto& method:instance.methods)if(method.name=="GetResult")method.body=clone(*observe->body);
    }
    if(found=="System.Threading.Tasks.Task"&&args.size()==1&&args[0]=="void") {
        std::erase_if(instance.fields,[](const Field& f){return f.type=="void";});
        // A completion-only Task has no value property. GetResult/await
        // still have the ordinary void return type.
        std::erase_if(instance.methods,[](const Method& m){return m.property_name=="Result";});
        std::erase_if(instance.properties,[](const Property& p){return p.name=="Result";});
    }
    append_class(std::move(instance));
    instantiate_nested_types(*c,program.classes.back(),map);
    return instance_name;
}

void Analyzer::instantiate_nested_types(const Class& original,const Class& owner,const Substitution& substitutions) {
    const auto class_count=program.classes.size(),enum_count=program.enums.size();
    for(std::size_t i=0;i<class_count;++i) {
        const auto& child=program.classes[i];
        if(child.declaring_type!=original.name)continue;
        auto nested=clone(child);nested.name=owner.name+"."+child.short_name;nested.declaring_type=owner.name;
        if(find_class(nested.name))continue;
        nested.is_template_member=owner.is_template_member||!owner.generic_parameters.empty()||!owner.pack_parameter.empty();
        const auto local = substitutions.without_parameters(child.generic_parameters, child.pack_parameter);
        substitute(nested, local);
        append_class(std::move(nested));
        instantiate_nested_types(child,program.classes.back(),local);
    }
    for(std::size_t i=0;i<enum_count;++i) {
        const auto& child=program.enums[i];
        if(child.declaring_type!=original.name)continue;
        Enum nested;nested.name=owner.name+"."+child.short_name;nested.declaring_type=owner.name;
        if(find_enum(nested.name))continue;
        nested.short_name=child.short_name;nested.namespace_name=child.namespace_name;nested.imports=child.imports;
        nested.token=child.token;nested.is_public=child.is_public;nested.attributes=child.attributes;nested.native_cpp=child.native_cpp;
        nested.underlying=substitute(child.underlying,substitutions);
        nested.is_template_member=owner.is_template_member||!owner.generic_parameters.empty()||!owner.pack_parameter.empty();
        for(const auto& member:child.members){EnumMember value;value.name=member.name;value.token=member.token;if(member.expression){value.expression=clone(*member.expression);substitute(*value.expression,substitutions);}nested.members.push_back(std::move(value));}
        auto& added=program.enums.emplace_back(std::move(nested));
        enums_by_name.try_emplace(added.name,&added);
    }
}

bool Analyzer::derives(const std::string& from,const std::string& to) const {
    return derives_from(program,from,to);
}

bool Analyzer::stringable(const std::string& type) {
    if(is_optional(type))return stringable(element_type(type));
    if(is_collection(type)) {
        for(const auto& argument:generic_args(type))if(!stringable(argument))return false;
        return true;
    }
    // A collection can mention a generic class/interface before any
    // element is constructed. Resolve its contracts before querying them.
    if(auto* c=find_class(type))prepare(*c);
    return stringable_type(program,type);
}

} // namespace bolt::semantic
