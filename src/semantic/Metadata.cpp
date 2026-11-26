#include "Analyzer.hpp"
#include <bolt/Frontend.hpp>
#include <bolt/Types.hpp>
#include <algorithm>
#include <map>

namespace bolt::semantic {
namespace {
constexpr auto generated_file = "<bolt-generated-metadata>";
bool metadata_owner(const Class& c) {
    return (c.name=="System.Reflection.Reflection" && c.token.at.file=="<bolt-stdlib>/System/Reflection.bolt") ||
           (c.name=="Bolt.Runtime.ValueTree" && c.token.at.file=="<bolt-stdlib>/System/PlatformValues.bolt");
}
std::string literal(std::string_view text) {
    std::string s="\"";
    for(char c:text)switch(c) {
        case '\\': s+="\\\\";break; case '"': s+="\\\"";break;
        case '\n': s+="\\n";break; case '\r': s+="\\r";break;
        case '\t': s+="\\t";break; case '\0': s+="\\0";break;
        default:s+=c;
    }
    return s+"\"";
}
const char* boolean(bool b) {return b?"true":"false";}
struct Member {
    const Class& owner;
    std::string name,type,key,attributes;
    bool property,immutable,snapshot,ignore=false;
    template<class F> Member(const Class& c,const F& f,bool p,bool ro,bool copy)
        :owner(c),name(f.name),type(referred(f.type)),key(f.name),attributes(attribute_text(f.attributes)),
         property(p),immutable(ro),snapshot(copy) {
        for(const auto& a:f.attributes) {
            if(a.name=="JsonIgnore")ignore=true;
            if(a.name=="JsonName"&&!a.arguments.empty())key=a.arguments.front();
        }
    }
    // Resolve the member in its declaring class. Mutable getters run on a
    // complete snapshot so inherited accessors retain virtual dispatch.
    std::string receiver(bool write,const std::string& concrete_type) const {
        const std::string source=write?"result":"value";
        if(snapshot && !write)return concrete_type+" copy = value; "+owner.name+"& receiver = copy;\n";
        return (write?std::string{}:"const ")+owner.name+"& receiver = "+source+";\n";
    }
};
std::vector<Member> members(const Program& program,const Class& c) {
    std::vector<const Class*> hierarchy;
    for(auto* base=&c;base&&base->is_reflected;base=program.find_class(base->base))hierarchy.push_back(base);
    std::vector<Member> result;
    for(auto it=hierarchy.rbegin();it!=hierarchy.rend();++it) {
        const auto& owner=**it;
        for(const auto& f:owner.fields) {
            if(f.name.starts_with("@")||f.is_event||f.is_static)continue;
            if(!f.is_public && std::none_of(f.attributes.begin(),f.attributes.end(),[](const Attribute& a){return a.name=="Reflect";}))continue;
            result.emplace_back(owner,f,false,f.constant,false);
        }
        for(const auto& p:owner.properties) {
            const auto* get=find_method(owner,p.getter);
            if(p.is_static||p.is_indexer||!p.is_public||!get||!get->is_public)continue;
            const auto* set=find_method(owner,p.setter);
            result.emplace_back(owner,p,true,!set||!set->is_public,!get->readonly_receiver);
        }
    }
    return result;
}
bool visible_method(const Method& m) {
    return m.property_name.empty()&&m.is_public&&!m.is_constructor&&!m.is_destructor&&
           m.generic_parameters.empty()&&m.pack_parameter.empty()&&!m.is_extern&&!m.is_constant_template;
}
std::string info(const Program& program,const Class& c) {
    std::string body;int nf=0,np=0,nm=0;
    for(const auto& f:members(program,c)) {
        auto slot=f.property?"properties["+std::to_string(np++):"fields["+std::to_string(nf++);
        body+=slot+"] = FieldInfo("+literal(f.name)+","+literal(f.type)+","+boolean(f.immutable)+","+
              boolean(f.property)+","+literal(f.attributes)+");\n";
    }
    for(const auto& m:c.methods)if(visible_method(m)) {
        std::string params;
        for(const auto& p:m.parameters) {if(!params.empty())params+="\n";params+=(p.constant?"const ":"")+p.type;}
        body+="methods["+std::to_string(nm++)+"] = MethodInfo("+literal(m.name)+","+literal(m.type)+","+
              boolean(m.is_static)+","+literal(params)+","+literal(attribute_text(m.attributes))+");\n";
    }
    return "FieldInfo[] fields = new FieldInfo["+std::to_string(nf)+"], properties = new FieldInfo["+std::to_string(np)+"];\n"+
           "MethodInfo[] methods = new MethodInfo["+std::to_string(nm)+"];\n"+body+
           "return TypeInfo("+literal(c.name)+","+literal(c.namespace_name)+","+boolean(c.is_struct)+","+
           literal(attribute_text(c.attributes))+",fields,properties,methods);";
}
std::string reflect(const Program& program,const Class* c,Method& m,
                    std::map<std::string,const Method*>& calls) {
    if(m.name=="IsReflected")return std::string("return ")+boolean(c&&c->is_reflected)+";";
    if(!c||!c->is_reflected)return "throw ReflectionException(\"The type is not marked [Reflect].\");";
    if(m.name=="GetType")return info(program,*c);
    std::string body;
    if(m.name=="Invoke") {
        std::map<std::string,std::vector<const Method*>> matches;
        for(const auto& target:c->methods) {
            if(!visible_method(target)||target.type!=m.type||target.parameters.size()+2!=m.parameters.size())continue;
            bool match=true;
            for(size_t i=0;i<target.parameters.size();i++)
                match &= referred(target.parameters[i].type)==referred(m.parameters[i+2].type);
            if(match)matches[target.name].push_back(&target);
        }
        for(const auto& [name,set]:matches) {
            body+="if(method=="+literal(name)+") {";
            if(set.size()!=1)body+="throw ReflectionException(\"Ambiguous reflected overload.\");";
            else {
                const auto& target=*set[0];
                auto placeholder="__metadataCall"+std::to_string(calls.size());
                calls.emplace(placeholder,&target);
                std::string args;
                for(size_t i=2;i<m.parameters.size();i++) {
                    if(i!=2)args+=",";
                    args+=m.parameters[i].name;
                }
                if(m.type!="void")body+="return ";
                body+=(target.is_static?c->name:"value")+"."+placeholder+"("+args+");";
                if(m.type=="void")body+="return;";
            }
            body+="}\n";
        }
        return body+"throw ReflectionException(\"No public method matches the name, result type and argument types.\");";
    }
    const bool set=m.name=="Set";
    if(set)body+=c->name+"& result = value;\n";
    const auto fields=members(program,*c);
    std::map<std::string,std::vector<const Member*>> by_name;
    for(const auto& field:fields)by_name[field.name].push_back(&field);
    for(const auto& [name,entries]:by_name) {
        body+="if(field=="+literal(name)+") {\n";
        for(const auto* field:entries) {
            if(set&&field->immutable) {
                body+="throw InvalidOperationException(\"The reflected field is read-only.\");";
                break;
            }
            if(field->type!=m.specialization_types.at(1)) {
                body+="throw InvalidCastException(\"The requested field type does not match its declaration.\");";
                break;
            }
            body+="{"+field->receiver(set,c->name);
            if(set)body+="receiver."+name+" = data;";
            else body+=std::string(field==entries.back()?"return ":"var previous = ")+"receiver."+name+";";
            body+="}\n";
        }
        if(set)body+="return;";
        body+="}\n";
    }
    return body+"throw ReflectionException(\"Field not found: \" + field);";
}
std::string encode(const std::string& t,const std::string& expr) {return "Encode<"+t+">("+expr+",depth+1)";}
std::string decode(const std::string& t,const std::string& expr) {return "Decode<"+t+">("+expr+",depth+1)";}
std::string codec(const Program& program,const std::string& t,bool writing) {
    if(is_optional(t)) {
        const auto elem=element_type(t);
        return writing?"if(value == null)return Null(); return "+encode(elem,"value.Value")+";":
                       "if(Kind(node)==0)return null; return "+decode(elem,"node")+";";
    }
    if(const auto* e=program.find_enum(t))
        return writing?"return "+encode(e->underlying,"("+e->underlying+")value")+";":"return ("+t+")"+decode(e->underlying,"node")+";";
    if(t=="bool")return writing?"return Bool(value);":"return Boolean(node);";
    if(t=="string")return writing?"return Text(value);":"return String(node);";
    if(t=="char")return writing?"return Text((string)value);":
        "string s=String(node); if(s.Length!=1)throw JsonException(\"A char needs exactly one Unicode scalar.\"); return s[0];";
    if(is_floating(t)) {
        if(writing)return "return Real((double)value);";
        return t=="double"?"return NumberValue(node);":
            "double n=NumberValue(node); if(n>3.4028234663852886e38 || n< -3.4028234663852886e38)throw JsonException(\"Float out of range.\"); return (float)n;";
    }
    if(is_integer(t)) {
        if(writing)return is_unsigned(t)?"return Unsigned((ulong)value);":"return Signed((long)value);";
        // Conversion checks are independent of the caller's checked arithmetic mode.
        const std::string fail="throw JsonException(\"JSON number outside target integer range.\");";
        auto signed_check=is_unsigned(t)?"if(n<0l || (ulong)n>(ulong)"+t+".MaxValue)"+fail:
            "if(n<(long)"+t+".MinValue || n>(long)"+t+".MaxValue)"+fail;
        const std::string limit=is_unsigned(t)&&bits(t)==64?"18446744073709551616.0":
            std::to_string(std::uint64_t{1}<<(is_unsigned(t)?bits(t):bits(t)-1))+".0";
        return "int k=Kind(node); if(k==2){long n=Integer(node);"+signed_check+"return ("+t+")n;}"+
            "if(k==3){ulong n=Natural(node);if(n>(ulong)"+t+".MaxValue)"+fail+"return ("+t+")n;}"+
            "double n=Floating(node);if(n<"+(is_unsigned(t)?"0.0":"-"+limit)+" || n>="+limit+")"+fail+
            t+" result=("+t+")n; if((double)result!=n)throw JsonException(\"Fractional integer.\"); return result;";
    }
    const auto* c=program.find_class(t);
    if(is_array(t)&&array_rank(t)!=1)return "throw JsonException(\"Serialization requires a rank-one array.\");";
    if(is_array(t)||is_collection(t)) {
        const auto elem=is_array(t)?element_type(t):generic_args(t).at(0);
        if(writing)return "int root=Array(); for(int i=0;i<value.Length;i++){int child="+encode(elem,"value[i]")+"; Add(root,child);} return root;";
        return "Expect(node,6);int length=Count(node);Bolt.Runtime.Buffer<"+elem+"> items;items.Reserve(length);"+
            "for(int i=0;i<length;i++)items.Add("+decode(elem,"Child(node,i)") + ");"+
            (is_array(t)?"return items.ToArray();":"return items;");
    }
    if(c)for(const auto& a:c->attributes) {
        if(a.name!="SerializeValue"&&a.name!="SerializeSequence"&&a.name!="SerializeMap")continue;
        const bool scalar=a.name=="SerializeValue",map=a.name=="SerializeMap";
        auto invalid=[&]{throw Error(a.token.at,"invalid "+a.name+" serialization contract","BLT2410");};
        if(a.arguments.size()!=(scalar?2:3))invalid();
        const auto* at=find_method(*c,a.arguments[scalar?0:1]);
        const auto* add=find_method(*c,a.arguments[scalar?1:2]);
        if(!at||!add||!at->is_public||!at->readonly_receiver||at->is_static||!add->is_public)invalid();
        const auto elem=referred(at->type);
        if(scalar) {
            if(!at->parameters.empty()||!add->is_static||add->type!=t||add->parameters.size()!=1||referred(add->parameters[0].type)!=elem)invalid();
            return writing?"return "+encode(elem,"value."+at->name+"()")+";":
                "return "+t+"."+add->name+"("+decode(elem,"node")+");";
        }
        const auto* count=find_property(*c,a.arguments[0]);
        if(!count||count->type!="int"||at->parameters.size()!=1||at->parameters[0].type!="int"||add->parameters.size()!=(map?2:1))invalid();
        std::string key,val;
        if(map) {
            const auto* pair=program.find_class(elem);
            const auto* k=pair?find_property(*pair,"Key"):nullptr;
            const auto* v=pair?find_property(*pair,"Value"):nullptr;
            if(!k||!v||referred(add->parameters[0].type)!=k->type||referred(add->parameters[1].type)!=v->type)invalid();
            key=k->type;val=v->type;
        } else if(referred(add->parameters[0].type)!=elem)invalid();
        const bool object=map&&key=="string";
        if(writing) {
            std::string body=std::string("int root=")+(object?"Object();":"Array();")+
                "int length=value."+count->name+"; for(int i=0;i<length;i++){var item=value."+at->name+"(i);";
            if(!map)body+="int child="+encode(elem,"item")+";Add(root,child);";
            else if(object)body+="int child="+encode(val,"item.Value")+";Put(root,item.Key,child);";
            else body+="int row=Object();int k="+encode(key,"item.Key")+";int v="+encode(val,"item.Value")+";Put(row,\"Key\",k);Put(row,\"Value\",v);Add(root,row);";
            return body+"}return root;";
        }
        std::string body=std::string("Expect(node,")+(object?"7);string[] keys=Keys(node);":"6);")+t+" result;for(int i=0;i<Count(node);i++){";
        if(object)body+="result."+add->name+"(keys[i],"+decode(val,"Member(node,keys[i])")+");";
        else if(map)body+="int row=Child(node,i);result."+add->name+"("+decode(key,"Member(row,\"Key\")")+","+decode(val,"Member(row,\"Value\")")+");";
        else body+="result."+add->name+"("+decode(elem,"Child(node,i)") +");";
        return body+"}return result;";
    }
    if(c&&c->is_reflected) {
        if(!writing && (c->is_interface || c->is_abstract))
            return "throw JsonException(\"Deserialization requires a concrete reflected type.\");";
        std::string body=writing?"int root=Object();":"Expect(node,7);"+t+" result;";
        std::set<std::string> keys;
        for(const auto& f:members(program,*c)) {
            if(!writing&&!keys.insert(f.key).second)return "throw JsonException(\"Duplicate reflected JSON name.\");";
            if(f.ignore)continue;
            auto key=literal(f.key);
            if(writing)body+="{"+f.receiver(false,t)+"int child="+encode(f.type,"receiver."+f.name)+";Put(root,"+key+",child);}\n";
            else if(!f.immutable)body+="{int child=Find(node,"+key+");if(child>=0){"+f.receiver(true,t)+"receiver."+f.name+"="+decode(f.type,"child")+";}}\n";
            else if(!f.property)body+="if(Find(node,"+key+")>=0)throw JsonException("+literal("Cannot deserialize into a read-only field: "+f.name)+");\n";
        }
        return body+(writing?"return root;":"return result;");
    }
    return "throw JsonException(\"Serialization requires a supported value/collection type or [Reflect] type.\");";
}
// Preserve the exact method selected from metadata, including ref overloads and
// compiler names that are not identifiers in source (operators and conversions).
using CallMap=std::map<std::string,const Method*>;
void bind_targets(Expr& expr,const CallMap& calls) {
    if(expr.kind==Expr::Kind::Member)if(auto it=calls.find(expr.text);it!=calls.end()) {
        expr.text=it->second->name;expr.callback_method=it->second->id;
    }
    for(auto& a:expr.args)bind_targets(*a,calls);
}
void bind_targets(Stmt& stmt,const CallMap& calls) {
    for(auto* e:{stmt.expression.get(),stmt.condition.get(),stmt.increment.get()})if(e)bind_targets(*e,calls);
    for(auto* s:{stmt.init.get(),stmt.body.get(),stmt.alternative.get()})if(s)bind_targets(*s,calls);
    for(auto& s:stmt.statements)bind_targets(*s,calls);
}
} // namespace

bool Analyzer::generated_metadata(const Token& at) const {
    return at.at.file==generated_file && current_class && metadata_owner(*current_class) &&
           current_method && current_method->is_specialization;
}
void Analyzer::specialize_metadata(Class& owner,Method& m) {
    if(!metadata_owner(owner)||m.specialization_types.empty())return;
    const bool reflection=owner.name=="System.Reflection.Reflection";
    if(!reflection&&m.name!="Encode"&&m.name!="Decode")return;
    const auto& type=m.specialization_types.at(m.name=="Invoke"?1:0);
    auto* target=find_class(referred(type));
    if(target)prepare(*target);
    for(size_t i=2;m.name=="Invoke"&&i<m.parameters.size();i++)m.parameters[i].name="argument"+std::to_string(i-2);
    CallMap calls;
    std::string body=reflection?reflect(program,target,m,calls):
        "if(depth>96)throw JsonException(\"JSON nesting exceeds 96 levels.\");\n"+codec(program,type,m.name=="Encode");
    auto parsed=parse("class MetadataBody { void Body(){\n"+body+"\n} }",generated_file);
    m.body=std::move(parsed.classes.front().methods.front().body);
    if(!calls.empty())bind_targets(*m.body,calls);
}
} // namespace bolt::semantic
