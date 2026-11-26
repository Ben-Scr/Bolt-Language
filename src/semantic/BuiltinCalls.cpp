#include "Analyzer.hpp"
#include <bolt/Types.hpp>

namespace bolt::semantic {

void Analyzer::collection_call(Expr& e,Expr& target) {
    const auto t=target.owner;
    const auto kind=collection_kind(t);
    const auto a=generic_args(t);
    auto& base=*target.args[0];
    const auto count=e.args.size()-1;
    auto count_is=[&](std::size_t n) {
        if(count!=n)throw Error(e.token.at,"incorrect argument count for '"+target.text+"'", "BLT2000");
    };
    auto arg=[&](std::size_t i,const std::string& type) {
        convertible(type,*e.args[i+1]);
    };
    e.binding=Expr::Binding::Collection;
    e.owner=t;
    e.text=target.text;
    bool mutate=true;
    if(kind=="List"&&e.text=="AppendRange"){
        count_is(3);arg(0,a[0]+"[]");arg(1,"int");arg(2,"int");e.type="void";
    }else if(kind=="List"&&e.text=="AppendBuffer"){
        count_is(3);arg(0,"int");arg(1,"int");arg(2,t);e.type="void";
    }else if(kind=="List"&&e.text=="Swap"){count_is(1);arg(0,t+"&");e.type="void";
    }else if(kind=="List"&&(e.text=="Reserve"||e.text=="Truncate")){
        count_is(1);arg(0,"int");e.type="void";
    }else if(kind=="List"&&e.text=="ToArray"){
        count_is(0);e.type=a[0]+"[]";mutate=false;
    }else if(e.text=="Clear") {
        count_is(0);
        e.type="void";
    } else if(e.text=="Contains"&&kind!="Dictionary") {
        count_is(1);
        arg(0,a[0]);
        e.type="bool";
        mutate=false;
    } else if((e.text=="Add"||e.text=="Enqueue")&&(kind=="List"||kind=="HashSet"||kind=="Queue")) {
        if((kind=="Queue")!=(e.text=="Enqueue"))throw Error(e.token.at,"unknown collection method", "BLT2000");
        count_is(1);
        arg(0,a[0]);
        e.type=kind=="HashSet"?"bool":"void";
    }else if(e.text=="Remove"&&(kind=="List"||kind=="HashSet"||kind=="Dictionary")) {
        count_is(1);
        arg(0,a[0]);
        e.type="bool";
    } else if(e.text=="RemoveAt"&&kind=="List") {
        count_is(1);
        arg(0,"int");
        e.type="void";
    } else if(e.text=="Insert"&&kind=="List") {
        count_is(2);
        arg(0,"int");
        arg(1,a[0]);
        e.type="void";
    } else if(e.text=="IndexOf"&&kind=="List") {
        count_is(1);
        arg(0,a[0]);
        e.type="int";
        mutate=false;
    } else if((e.text=="Peek"||e.text=="Dequeue")&&kind=="Queue") {
        count_is(0);
        e.type=a[0];
        mutate=e.text=="Dequeue";
    } else if(e.text=="TryDequeue"&&kind=="Queue") {
        count_is(1);
        arg(0,a[0]+"&");
        e.type="bool";
    } else if((e.text=="Add"||e.text=="Set")&&kind=="Dictionary") {
        count_is(2);
        arg(0,a[0]);
        arg(1,a[1]);
        e.type="void";
    } else if(e.text=="ContainsKey"&&kind=="Dictionary") {
        count_is(1);
        arg(0,a[0]);
        e.type="bool";
        mutate=false;
    } else if(e.text=="TryGetValue"&&kind=="Dictionary") {
        count_is(2);
        arg(0,a[0]);
        arg(1,a[1]+"&");
        e.type="bool";
        mutate=false;
    } else throw Error(e.token.at,"unknown "+kind+" method '"+e.text+"'", "BLT2000");
    if(mutate&&base.constant&&!target.arrow)throw Error(e.token.at,"cannot modify a const collection", "BLT2000");
}

bool Analyzer::bind_implicit_static(Expr& e) {
    for(const auto& name:program.implicit_static_imports) {
        auto* c=find_class(name);
        if(!c||methods(*c,e.text).empty())continue;
        e.binding=Expr::Binding::Method;
        e.owner=c->name;
        e.is_static=true;
        e.type="#method";
        return true;
    }
    return false;
}

void Analyzer::builtin_call(Expr& e,Expr& target) {
    e.binding=Expr::Binding::Builtin;
    e.owner=target.owner;
    e.text=target.text;
    const auto n=e.args.size()-1;
    auto count=[&](std::size_t expected) {
        if(n!=expected)throw Error(e.token.at,e.owner+"."+e.text+" expects "+std::to_string(expected)+" argument(s)", "BLT2000");
    };
    if(e.owner=="String") {
        if(!target.type_args.empty()) {
            if(!target.is_static&&target.kind==Expr::Kind::Member&&!extension_methods(target.text,target.token).empty()) {
                std::vector<std::string> types;for(const auto& type:target.type_args)types.push_back(resolve(type,current_class,target.token,true));
                lower_extension_call(e,types);return;
            }
            throw Error(e.token.at,"string methods are not generic","BLT2000");
        }
        e.is_static=target.is_static;
        struct Signature {const char* result;std::vector<std::string> parameters;};
        std::vector<Signature> signatures;
        const auto add=[&](const char* result,std::initializer_list<const char*> args){Signature item{result,{}};for(const auto* type:args)item.parameters.emplace_back(type);signatures.push_back(std::move(item));};
        if(e.is_static) {
            if(e.text=="IsNullOrEmpty"||e.text=="IsNullOrWhiteSpace")add("bool",{"string"});
            else if(e.text=="Join")add("string",{"string","string[]"});
            else if(e.text=="FromUtf8")add("string",{"byte[]"});
            else if(e.text=="Equals")add("bool",{"string","string"});
            else if(e.text=="Compare")add("int",{"string","string"});
        } else {
            if(e.text=="Contains"){add("bool",{"string"});add("bool",{"char"});}
            else if(e.text=="StartsWith"||e.text=="EndsWith"||e.text=="Equals")add("bool",{"string"});
            else if(e.text=="IndexOf"){add("int",{"string"});add("int",{"char"});add("int",{"string","int"});add("int",{"char","int"});}
            else if(e.text=="LastIndexOf"){add("int",{"string"});add("int",{"char"});}
            else if(e.text=="Substring"||e.text=="Remove"){add("string",{"int"});add("string",{"int","int"});}
            else if(e.text=="Insert")add("string",{"int","string"});
            else if(e.text=="Replace"){add("string",{"string","string"});add("string",{"char","char"});}
            else if(e.text=="Trim"||e.text=="TrimStart"||e.text=="TrimEnd"||e.text=="ToUpper"||e.text=="ToLower")add("string",{});
            else if(e.text=="PadLeft"||e.text=="PadRight"){add("string",{"int"});add("string",{"int","char"});}
            else if(e.text=="Split"){add("string[]",{"string"});add("string[]",{"char"});add("string[]",{"string","bool"});add("string[]",{"char","bool"});}
            else if(e.text=="ToCharArray")add("char[]",{});
            else if(e.text=="ToUtf8")add("byte[]",{});
            else if(e.text=="CompareTo"){e.text="Compare";add("int",{"string"});}
        }
        for(const auto& signature:signatures) {
            if(signature.parameters.size()!=n)continue;bool matches=true;
            for(std::size_t i=0;i<n;++i)matches&=can_convert(signature.parameters[i],*e.args[i+1]);
            if(matches){e.type=signature.result;e.type_args=signature.parameters;program.require_cpp("Unicode string methods");return;}
        }
        if(!target.is_static&&target.kind==Expr::Kind::Member&&!extension_methods(target.text,target.token).empty()) {lower_extension_call(e,{});return;}
        throw Error(e.token.at,"no matching "+std::string(e.is_static?"static ":"")+"string method '"+e.text+"' for the supplied argument types","BLT2006");
    }
    if(is_numeric(e.owner)) {
        if(!target.type_args.empty())throw Error(e.token.at,"numeric parsing methods are not generic", "BLT2000");
        const bool attempt=e.text=="TryParse";
        if(!attempt&&e.text!="Parse")throw Error(e.token.at,"unknown numeric type method '"+e.text+"'", "BLT2000");
        count(attempt?2:1);
        convertible("string",*e.args[1]);
        if(attempt)convertible(e.owner+"&",*e.args[2]);
        e.type=attempt?"bool":e.owner;
        e.type_args={e.owner};
        return;
    }
    if(e.owner=="Array") {
        if(e.text!="GetLength"||!target.type_args.empty())throw Error(e.token.at,"array GetLength does not accept type arguments", "BLT2000");
        if(n!=1)throw Error(e.token.at,"array GetLength expects one dimension argument", "BLT2000");
        if(e.args[1]->type=="#default")expression(*e.args[1],false,"int");
        if(!is_integer(e.args[1]->type))throw Error(e.args[1]->token.at,"array dimension must be an integer", "BLT2000");
        e.type="int";
        return;
    }
    if(e.owner=="Arena") {
        if(!target.type_args.empty())throw Error(e.token.at,"Arena methods are not generic", "BLT2300");
        if(e.text=="Fixed"||e.text=="Growable") {
            if(&e!=arena_initializer)throw Error(e.token.at,"Arena factories require direct initialization of a local Arena owner", "BLT2300");
            count(e.text=="Fixed"?1:2);
            for(std::size_t i=1;i<e.args.size();++i) {
                if(!is_integer(e.args[i]->type))throw Error(e.args[i]->token.at,"Arena capacity must be an integer", "BLT2300");
                convertible("ulong",*e.args[i]);
            }
            e.type="Arena";
        }else {
            count(0);
            e.type="void";
        }
        return;
    }
    if(e.owner=="Intrinsic"&&e.text=="Reinterpret") {
        count(1);
        if(target.type_args.size()!=1)throw Error(e.token.at,"Reinterpret<T>(value) requires exactly one target type", "BLT2110");
        e.type=resolve(target.type_args[0],current_class,e.token);
        auto scalar=[&](const std::string& t) {
            if(const auto* en=find_enum(t))return en->underlying;
            return t;
        };
        const auto to=scalar(e.type),from=scalar(e.args[1]->type);
        if(!is_numeric(to)||!is_numeric(from)||bits(to)!=bits(from))
            throw Error(e.token.at,"Reinterpret requires equally sized integer, floating-point, or integer-backed enum values; received '"+e.args[1]->type+"' and '"+e.type+"'", "BLT2110");
        e.type_args={e.type};
        return;
    }
    if(e.owner=="Runtime") {
        if(!target.type_args.empty())throw Error(e.token.at,"Runtime methods are not generic", "BLT2000");
        if(e.text!="Execute"&&e.text!="ExecuteProgram"&&e.text!="ExecuteFile"&&e.text!="ExecuteJit"&&e.text!="ExecuteProgramJit"&&e.text!="ExecuteFileJit")throw Error(e.token.at,"unknown Runtime method", "BLT2000");
        count(1);
        if(e.args[1]->type=="#default")expression(*e.args[1],false,"string");
        if(e.args[1]->type!="string")throw Error(e.token.at,"runtime execution requires one string argument", "BLT2000");
        e.type="int";
        return;
    }
    if(e.owner=="Type") {
        if(e.text=="IsInteger"||e.text=="IsUnsigned"||e.text=="IsFloatingPoint"||e.text=="IsFloat32") {
            if(target.type_args.size()!=1)throw Error(e.token.at,"type predicate needs exactly one type argument", "BLT2000");
            count(0);const auto t=resolve(target.type_args[0],current_class,e.token);
            const bool result=e.text=="IsInteger"?is_integer(t):e.text=="IsUnsigned"?is_unsigned(t):e.text=="IsFloatingPoint"?is_floating(t):t=="float";
            e.kind=Expr::Kind::Literal;e.binding=Expr::Binding::None;e.type="bool";e.text=result?"true":"false";e.args.clear();e.type_args.clear();return;
        }
        if(e.text!="Name"||target.type_args.size()!=1)throw Error(e.token.at,"Type.Name<T>() requires one type argument", "BLT2000");
        count(0);
        e.type="string";
        e.type_args= {
            resolve(target.type_args[0],current_class,e.token)
        };
        return;
    }
    std::string t;
    if(target.type_args.empty()) {
        if(n==0)throw Error(e.token.at,"numeric method requires a generic type argument", "BLT2000");
        t=e.args[1]->type;
    }else {
        if(target.type_args.size()!=1)throw Error(e.token.at,"numeric method requires one type argument", "BLT2000");
        t=resolve(target.type_args[0],current_class,e.token);
    }
    if(!is_numeric(t))throw Error(e.token.at,"type '"+t+"' does not satisfy concept 'Number'", "BLT2101");
    e.type_args= {
        t
    };
    for(std::size_t i=1;i<e.args.size();++i)convertible(t,*e.args[i]);
    e.type=t;
    static const std::unordered_set<std::string> unary= {
        "Sin","Cos","Tan","Asin","Acos","Atan","Sqrt","Exp","Log","Log2","Log10","Abs","Round","Floor","Ceil","Ceiling","Truncate","Sign"
    };
    static const std::unordered_set<std::string> binary= {
        "Pow","Atan2","Min","Max"
    };
    if(unary.contains(e.text))count(1);
    else if(binary.contains(e.text))count(2);
    else if(e.text=="Clamp")count(3);
    else throw Error(e.token.at,"unknown Math method '"+e.text+"'", "BLT2000");
    static const std::unordered_set<std::string> real= {
        "Sin","Cos","Tan","Asin","Acos","Atan","Sqrt","Exp","Log","Log2","Log10","Pow","Atan2"
    };
    if(real.contains(e.text)&&!is_floating(t))throw Error(e.token.at,"Math."+e.text+" requires the FloatingPoint concept (float or double)", "BLT2000");
}

} // namespace bolt::semantic
