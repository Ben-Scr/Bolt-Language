#include "Analyzer.hpp"
#include "../ClosureConversion.hpp"
#include "Substitution.hpp"
#include <bolt/Frontend.hpp>
#include <bolt/Types.hpp>

namespace bolt::semantic {

void Analyzer::require_event_owner(const Expr& e) const {
    if(!current_class||current_class->name!=e.owner)
        throw Error(e.token.at,"event '"+e.owner+"."+e.text+"' may only be raised by its declaring type; external code may use += or -=", "BLT2701");
}

void Analyzer::lower_event_receiver(Expr& e) {
    e.authorized_event_access=true;e.binding=Expr::Binding::Field;e.type=e.declared_type;
}

std::pair<Class*,Method*> Analyzer::delegate_method(const Expr& e,const std::string& action) {
    auto* klass=find_class(action);if(!klass)throw Error(e.token.at,"unknown delegate type", "BLT2702");
    prepare(*klass);auto* invoke=find_method(*klass,"Invoke");
    auto* owner=find_class(e.owner);if(!invoke||!owner)throw Error(e.token.at,"expected an Action and a method group", "BLT2702");
    std::vector<Expr> storage;std::vector<Expr*> arguments;storage.reserve(invoke->parameters.size());
    for(const auto& parameter:invoke->parameters){storage.emplace_back(Expr::Kind::Name,e.token);auto& value=storage.back();value.type=referred(parameter.type);value.declared_type=parameter.type;value.lvalue=true;value.constant=parameter.constant;arguments.push_back(&value);}
    auto candidates=methods(*owner,e.text);
    std::erase_if(candidates,[&](const auto& candidate){const auto& method=*candidate.second;return method.is_static!=e.is_static||method.type!=invoke->type||(!value_pack(method)&&method.parameters.size()!=arguments.size())||(value_pack(method)&&arguments.size()<required_arguments(method))||variadic(method);});
    std::vector<std::string> types;for(const auto& type:e.type_args)types.push_back(resolve(type,current_class,e.token));
    auto selected=select(std::move(candidates),types,arguments,e.token);
    // Delegate signatures do not silently insert user conversions/ref temporaries.
    for(std::size_t i=0;i<arguments.size();++i)if(selected.second->parameters[i].type!=invoke->parameters[i].type||selected.second->parameters[i].constant!=invoke->parameters[i].constant)
        throw Error(e.token.at,"Action parameter types must match the bound handler signature exactly", "BLT2702");
    accessible(*selected.first,selected.second->is_public,selected.second->is_protected,e);
    if(!e.is_static) {
        if(e.constant)throw Error(e.token.at,"binding an instance handler requires a mutable live receiver", "BLT2702");
        if(e.kind==Expr::Kind::Member&&!e.arrow&&!e.args[0]->lvalue)throw Error(e.token.at,"cannot bind an instance delegate to a temporary; keep the receiver alive until unsubscribed", "BLT2702");
    }
    return selected;
}

bool Analyzer::delegate_compatible(const Expr& e,const std::string& action) {
    try{delegate_method(e,action);return true;}catch(const Error&){return false;}
}

void Analyzer::bind_delegate(Expr& e,const std::string& action) {
    using K=Expr::Kind;
    auto [owner,method]=delegate_method(e,action);
    const auto key=action+"|"+owner->name+"|"+method->id;
    std::string adapter_name;
    if(auto it=delegate_adapters.find(key);it!=delegate_adapters.end())adapter_name=it->second;
    else {
        // Stable, injective name across independently compiled project libraries.
        std::uint64_t identity=1469598103934665603ull;for(unsigned char ch:key){identity^=ch;identity*=1099511628211ull;}
        std::string short_name="BoundAction";for(unsigned char ch:key){static constexpr char hex[]="0123456789abcdef";short_name+=hex[ch>>4];short_name+=hex[ch&15];}
        adapter_name="Bolt.Generated."+short_name;
        if(find_class(adapter_name))throw Error(e.token.at,"reserved generated delegate name collision", "BLT2702");
        std::string parameters,arguments;
        for(std::size_t i=0;i<method->parameters.size();++i){if(i){parameters+=", ";arguments+=", ";}parameters+=method->parameters[i].type+" arg"+std::to_string(i);arguments+="arg"+std::to_string(i);}
        const auto receiver=method->is_static?"NativeOwner.":"receiver->";
        auto handler=method->name;if(!method->specialization_types.empty())handler=generic_name(handler,method->specialization_types);
        std::string code="namespace Bolt.Generated; public sealed class "+short_name+" : "+action+" { ";
        if(!method->is_static)code+="private: NativeOwner* receiver; ";
        code+="public: "+short_name+(method->is_static?"() {} ":"(NativeOwner* value) { receiver=value; } ");
        code+=method->type+" Invoke("+parameters+"){ "+(method->type=="void" ? "" : "return ")+receiver+handler+"("+arguments+"); } ";
        code+="[ReadOnly] ulong InvocationId(){return "+std::to_string(identity)+"ul;} ";
        code+="[ReadOnly] string InvocationKey(){return \""+key+"\";} ";
        code+="[ReadOnly] void* InvocationTarget(){return "+std::string(method->is_static?"null":"(void*)receiver")+";} }";
        auto generated=parse(code,"<bolt-generated>/"+short_name+".bolt",program.preprocessor);
        auto adapter=std::move(generated.classes.front());
        Substitution substitution{{"NativeOwner",owner->name}};
        for(auto& f:adapter.fields)f.type=substitute(f.type,substitution);
        for(auto& m:adapter.methods)substitute(m,substitution);
        adapter.delegate_owner=owner->name;adapter.delegate_method=method->name;adapter.reachable=true;
        append_class(std::move(adapter));
        delegate_adapters[key]=adapter_name;prepare(program.classes.back());
    }
    const auto token=e.token;ExprPtr receiver;
    if(!method->is_static) {
        if(e.kind==K::Member) {
            receiver=std::move(e.args[0]);
            if(!e.arrow){auto address=std::make_unique<Expr>(K::Unary,token,"&");address->args.push_back(std::move(receiver));receiver=std::move(address);}
        } else {receiver=std::make_unique<Expr>(K::Unary,token,"&");receiver->args.push_back(std::make_unique<Expr>(K::Name,token,"this"));}
    }
    e=Expr(K::Call,token);e.args.push_back(std::make_unique<Expr>(K::Name,token,adapter_name));if(receiver)e.args.push_back(std::move(receiver));
    expression(e);
    // Retain the original bound symbol for editor navigation.
    e.callback_owner=owner->name;e.callback_method=method->id;
}

bool Analyzer::lambda_matches(const Expr& e,const std::string& target) {
    if(action_arity(target)<0&&!function_type(target))return false;
    auto* contract=find_class(target);
    if(!contract)return false;
    prepare(*contract);
    auto* invoke=find_method(*contract,"Invoke");
    if(!invoke||invoke->parameters.size()!=e.lambda->parameters.size())return false;
    for(std::size_t i=0;i<invoke->parameters.size();++i) {
        const auto& p=e.lambda->parameters[i];
        if(!p.type.empty()&&resolve(p.type,current_class,p.name)!=invoke->parameters[i].type)return false;
    }
    return true;
}

std::string Analyzer::infer_lambda(const Expr& e) {
    const auto& lambda=*e.lambda;
    std::vector<std::string> args;
    for(const auto& p:lambda.parameters) {
        if(p.type.empty())throw Error(p.name.at,"give the lambda parameter a type or declare an Action or Func target","BLT2702");
        args.push_back(resolve(p.type,current_class,p.name));
    }
    if(!lambda.expression_body||lambda.body->kind!=Stmt::Kind::Return)
        throw Error(e.token.at,"declare an Action or Func target for a block lambda","BLT2702");
    auto result=clone(*lambda.body->expression);
    auto saved=flow;scopes.emplace_back();
    for(std::size_t i=0;i<args.size();++i) {
        const auto& p=lambda.parameters[i];
        add_symbol(p.name.text,args[i],false,p.name,true);
    }
    expression(*result);value(*result,true);
    scopes.pop_back();flow=std::move(saved);
    auto contract=std::string("System.Action");
    if(result->type!="void"){contract="System.Func";args.push_back(result->type);}
    return resolve(generic_name(contract,args),current_class,e.token);
}

void Analyzer::bind_lambda(Expr& e,const std::string& target) {
    using K=Expr::Kind;
    if(!current_class||!lambda_matches(e,target))
        throw Error(e.token.at,"lambda needs a matching Action or Func target signature","BLT2702");
    if(!current_method)ClosureConversion{}.run(e);
    const auto& invoke=*find_method(*find_class(target),"Invoke");
    const bool capture_receiver=current_method&&!static_context&&lambda_uses_receiver(*e.lambda);
    const std::string key=current_class->name+"|"+(current_method?current_method->id:"@initializer")+"|"+e.token.at.file+"|"+std::to_string(e.token.at.offset)+"|"+target;
    std::uint64_t identity=14695981039346656037ull;
    for(unsigned char ch:key){identity^=ch;identity*=1099511628211ull;}
    const auto suffix=std::to_string(identity);
    const auto adapter_short="Closure"+suffix,adapter_name="Bolt.Generated."+adapter_short;
    const auto helper_name="@lambda"+suffix;
    if(!find_class(adapter_name)) {
        auto& helper=current_class->methods.emplace_back();
        helper.name=helper_name;helper.type=invoke.type;helper.token=e.token;
        helper.is_static=!capture_receiver;helper.readonly_receiver=capture_receiver&&current_method->readonly_receiver;
        helper.referenced=true;
        Substitution types{{"LambdaOwner",current_class->name},{"LambdaResult",invoke.type},{"LambdaTarget",target}};
        std::string fields,inputs,initializers,parameters,arguments;
        auto field=[&](const std::string& type,const std::string& name) {
            fields+=type+" "+name+"; ";
            if(!inputs.empty()){inputs+=", ";initializers+=", ";}
            inputs+=type+" in"+name;initializers+=name+"(in"+name+")";
        };
        if(capture_receiver) {
            fields+="Bolt.Runtime.ICaptureCell<LambdaOwner> receiver; ";
            inputs="LambdaOwner inreceiver";
            initializers="receiver(Bolt.Runtime.ClosureStorage.Create(inreceiver))";
        }
        for(const auto& name:e.lambda->captures) {
            auto* variable=symbol(name);
            if(!variable)throw Error(e.token.at,"unresolved lambda capture '"+name+"'","BLT2702");
            if(is_pointer(variable->type)||is_reference(variable->type)||variable->type=="Arena")
                throw Error(e.token.at,"a closure cannot capture a borrowed pointer, reference or Arena","BLT2702");
            const auto index=std::to_string(helper.parameters.size()),type="Capture"+index,slot="slot"+index;
            types[type]=variable->type;
            helper.parameters.push_back({variable->type,name,e.token,e.token,variable->constant});
            field(type,slot);
            if(!arguments.empty())arguments+=",";
            arguments+=slot;
        }
        for(std::size_t i=0;i<invoke.parameters.size();++i) {
            auto p=invoke.parameters[i];p.token=p.name_token=e.lambda->parameters[i].name;p.name=p.name_token.text;
            helper.parameters.push_back(p);
            if(!parameters.empty())parameters+=",";
            if(!arguments.empty())arguments+=",";
            const auto type="Input"+std::to_string(i),name="arg"+std::to_string(i);
            types[type]=p.type;
            parameters+=(p.constant?std::string("const "):"")+type+" "+name;
            arguments+=name;
        }
        helper.body=std::move(e.lambda->body);
        if(e.lambda->expression_body&&invoke.type=="void") {
            auto* result=helper.body.get();
            if(result->kind==Stmt::Kind::Block)result=result->statements.back().get();
            result->kind=Stmt::Kind::Expression;
        }
        if(helper.body->kind!=Stmt::Kind::Block) {
            auto block=std::make_unique<Stmt>(Stmt::Kind::Block,e.token);
            block->statements.push_back(std::move(helper.body));helper.body=std::move(block);
        }
        prepare_method(*current_class,helper);
        std::string callbacks;
        if(action_arity(target)>=0) {
            fields+="Bolt.Runtime.ICaptureCell<int> identity; void* identityAddress; ";
            if(!initializers.empty())initializers+=", ";
            initializers+="identity(Bolt.Runtime.ClosureStorage.Create(0))";
            callbacks="[ReadOnly] ulong InvocationId(){return "+suffix+"ul;} [ReadOnly] string InvocationKey(){return \""+adapter_short+"\";} [ReadOnly] void* InvocationTarget(){return identityAddress;}";
        }
        const std::string ctor=adapter_short+"("+inputs+")"+(initializers.empty()?"":" : "+initializers)+
            " {"+(callbacks.empty()?"":"identityAddress=(void*)&identity.Get();")+"}";
        const auto code="namespace Bolt.Generated; public sealed class "+adapter_short+" : LambdaTarget {private: "+fields+"public: "+ctor+
            " LambdaResult Invoke("+parameters+"){"+(invoke.type=="void"?"":"return ")+(helper.is_static?"LambdaOwner.":"receiver.Get().")+"LambdaBody("+arguments+");}"+callbacks+"}";
        auto generated=parse(code,"<bolt-generated>/"+adapter_short+".bolt",program.preprocessor);
        auto adapter=std::move(generated.classes.front());
        adapter.delegate_owner=current_class->name;adapter.delegate_method=helper_name;adapter.reachable=true;
        // The generated call targets a compiler-only method inaccessible to source identifiers.
        auto& target_call=*find_method(adapter,"Invoke")->body->statements.front()->expression->args.front();
        target_call.text=helper_name;
        substitute(adapter,types);
        append_class(std::move(adapter));prepare(program.classes.back());
    }
    const auto token=e.token;
    auto captured=std::move(e.lambda->captures);
    e=Expr(K::Call,token);e.args.push_back(std::make_unique<Expr>(K::Name,token,adapter_name));
    if(capture_receiver)e.args.push_back(std::make_unique<Expr>(K::Name,token,"this"));
    for(const auto& name:captured)e.args.push_back(std::make_unique<Expr>(K::Name,token,name));
    expression(e);
}

bool Analyzer::lambda_uses_receiver(const LambdaSyntax& lambda) {
    for(const auto& name:lambda.free_names) {
        if(name=="this")return true;
        if(auto f=field(*current_class,name).second;f&&!f->is_static)return true;
        if(auto p=property(*current_class,name).second;p&&!p->is_static)return true;
        for(const auto& m:methods(*current_class,name))if(!m.second->is_static)return true;
    }
    return false;
}

} // namespace bolt::semantic
