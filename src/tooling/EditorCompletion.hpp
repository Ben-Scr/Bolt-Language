// Internal language-server helpers. All positions refer to the current unsaved
// source; declarations recovered from a broken body never replace diagnostics.
struct EditorTokens {
    std::vector<LexToken> tokens;
    std::vector<std::size_t> pair,parent;
    std::vector<std::size_t> lines{0};
    std::size_t size;
    explicit EditorTokens(std::string_view source):size(source.size()) {
        for(std::size_t i=0;i<source.size();++i)if(source[i]=='\n')lines.push_back(i+1);
        for(auto& t:lex(source))if(t.kind!="comment")tokens.push_back(std::move(t));
        pair.assign(tokens.size(),tokens.size());parent=pair;
        std::vector<std::size_t> stack;
        for(std::size_t i=0;i<tokens.size();++i){
            if(!stack.empty())parent[i]=stack.back();
            const auto& t=tokens[i].text;
            if(t=="("||t=="["||t=="{")stack.push_back(i);
            else if(t==")"||t=="]"||t=="}"){
                const auto wanted=t==")"?"(":t=="]"?"[":"{";
                // An unfinished call must not swallow the enclosing method and
                // every declaration that follows it in the current document.
                if(t=="}")while(!stack.empty()&&tokens[stack.back()].text!="{")stack.pop_back();
                if(!stack.empty()&&tokens[stack.back()].text==wanted){pair[i]=stack.back();pair[stack.back()]=i;stack.pop_back();}
            }
        }
    }
    Location location(const std::string& file,std::size_t index)const {
        const auto& t=tokens[index];
        const auto line=std::upper_bound(lines.begin(),lines.end(),t.start)-lines.begin();
        return {file,static_cast<int>(line),static_cast<int>(t.start-lines[line-1]+1),
                static_cast<int>(t.length),t.start};
    }
    std::size_t at(std::size_t byte)const {
        return static_cast<std::size_t>(std::lower_bound(tokens.begin(),tokens.end(),byte,
            [](const LexToken& t,std::size_t b){return t.start<b;})-tokens.begin());
    }
    std::size_t end(std::size_t open)const{return pair[open]<tokens.size()?tokens[pair[open]].start:size;}
    std::size_t body(std::size_t byte)const {
        for(auto n=at(byte);n<tokens.size();++n){
            if(tokens[n].text==";")break;
            if(tokens[n].text=="{")return n;
        }
        return tokens.size();
    }
};
// A '[' only starts attribute completion in a declaration context, never
// in an array type, an index expression, a literal or an attribute argument.
std::optional<std::size_t> attribute_prefix(const std::string& source,const EditorTokens& scan,std::size_t caret) {
    std::vector<std::size_t> open;
    for(std::size_t i=0;i<scan.tokens.size()&&scan.tokens[i].start<caret;++i) {
        const auto& t=scan.tokens[i];
        if((t.kind=="string"||t.kind=="character"||t.kind=="comment")&&t.start+t.length>=caret)return {};
        if(t.text=="[")open.push_back(i);
        if(t.text=="]"&&!open.empty())open.pop_back();
    }
    if(open.empty())return {};
    const auto index=open.back();
    // Walk back through preceding attribute lists. Parentheses close calls,
    // identifiers finish array element types/receivers, and cannot start an attr.
    auto before=index;
    while(before&&scan.tokens[before-1].text=="]") {
        auto start=scan.pair[before-1];if(start>=before)return {};
        before=start;
    }
    if(before) {
        const auto& prev=scan.tokens[before-1].text;
        if(prev!=";"&&prev!="{"&&prev!="}"&&prev!=":")return {};
    }
    std::size_t start=scan.tokens[index].start+1;int parens=0;
    for(auto i=index+1;i<scan.tokens.size()&&scan.tokens[i].start<caret;++i) {
        const auto& t=scan.tokens[i];
        if(t.text=="(")++parens;else if(t.text==")"){if(--parens<0)return {};}
        else if(t.text==","&&!parens)start=t.start+t.length;
        else if(t.text=="["||t.text=="{"||t.text=="}"||t.text==";")return {};
    }
    if(parens)return {};
    while(start<caret&&std::isspace(static_cast<unsigned char>(source[start])))++start;
    for(auto n=start;n<caret;++n)if(!alpha(source[n])&&!digit(source[n])&&source[n]!='.')return {};
    return start;
}
std::string declaration_source(const std::string& text) {
    EditorTokens scan(text);auto result=text;
    for(const auto& token:scan.tokens)if(token.text=="[") {
        auto end=text.find('\n',token.start);if(end==std::string::npos)end=text.size();
        if(attribute_prefix(text,scan,end))for(auto n=token.start;n<end;++n)if(result[n]!='\r')result[n]=' ';
    }
    int braces=0;
    for(std::size_t i=0;i<scan.tokens.size();++i){
        const auto& token=scan.tokens[i];
        if(token.text=="{"){
            // Method bodies follow a parameter/constructor/constraint list.
            // Once a body starts, its contents (including nested statements)
            // can be omitted without changing any declaration offsets.
            bool method=i&&scan.tokens[i-1].text==")";
            if(!method&&i){
                for(auto j=i;j>0;){--j;auto t=scan.tokens[j].text;if(t==";"||t=="{"||t=="}")break;if(t==")"){method=true;break;}}
            }
            if(method){
                auto end=scan.end(i);
                for(auto j=token.start+1;j<end;++j)if(result[j]!='\r'&&result[j]!='\n')result[j]=' ';
                if(scan.pair[i]<scan.tokens.size())i=scan.pair[i];
                else {result+='}';break;}
            }else ++braces;
        }else if(token.text=="}")--braces;
    }
    while(braces-->0)result+='}';
    return result;
}
struct EditorItem {
    std::string name,type,detail,category;
    int kind=6;
    std::vector<std::string> parameters;
    std::vector<std::string> type_parameters;
    bool is_static=false;
    std::size_t required_arguments=0;
    bool value_pack=false;
    std::string type_pack;
    std::string event_owner;
    // Receiver inference remains part of overload selection after hiding `this`.
    std::map<std::string,std::string> receiver_types;
    bool is_extension=false;
    Location declaration{"",1,1};
    EditorItem(std::string name_,std::string type_,std::string detail_,std::string category_,int kind_=6,
               std::vector<std::string> parameters_={},std::vector<std::string> type_parameters_={},bool stat=false)
        :name(std::move(name_)),type(std::move(type_)),detail(std::move(detail_)),category(std::move(category_)),
         kind(kind_),parameters(std::move(parameters_)),type_parameters(std::move(type_parameters_)),is_static(stat){required_arguments=parameters.size();}
};
std::string bare_type(std::string type){while(is_reference(type)||is_pointer(type))type=element_type(type);return type;}
struct EditorContext {
    const Program* program;
    const Class* owner=nullptr;
    const Method* method=nullptr;
    const EditorTokens& scan;
    std::string file;
    std::size_t caret;
    std::vector<EditorItem> locals;
    explicit EditorContext(const Program* p,const EditorTokens& s,const std::string& file,std::size_t point,
                           std::size_t insertion=std::string::npos):program(p),scan(s),file(file),caret(point) {
        if(!p)return;
        // Declarations after an uncommitted insertion still carry old offsets.
        // Only a body that was already open before it can own this expression.
        const auto scope_point=std::min(point,insertion);
        for(const auto& c:p->classes){
            if(c.token.at.file!=file||c.is_global||c.token.at.offset>=scope_point)continue;
            auto body=scan.body(c.token.at.offset);
            if(body<scan.tokens.size()&&scan.tokens[body].start<scope_point&&caret<=scan.end(body)&&
                (!owner||c.token.at.offset>owner->token.at.offset))owner=&c;
        }
        for(const auto& c:p->classes)for(const auto& m:c.methods){
            if(m.token.at.file!=file||!m.body||m.is_specialization||m.is_constant_template||m.token.at.offset>=scope_point)continue;
            auto body=scan.body(m.token.at.offset);
            if(body<scan.tokens.size()&&scan.tokens[body].start<scope_point&&caret<=scan.end(body)){
                owner=&c;method=&m;
                for(const auto& a:m.parameters){
                    locals.push_back({a.name,a.type,a.type+" "+a.name,"parameter",6,{}});
                    locals.back().declaration=a.name_token.at;
                }
                auto add_local=[&](std::size_t at,const std::string& type){
                    const auto& name=scan.tokens[at].text;
                    locals.push_back({name,type,type+" "+name,"variable",6,{}});
                    locals.back().declaration=scan.location(file,at);
                };
                // Only tokens before the caret and whose complete lexical scope
                // contains it participate. This excludes sibling methods/blocks
                // and declarations below the cursor, even in an invalid body.
                for(auto i=body+1;i<scan.tokens.size()&&scan.tokens[i].start<caret;++i){
                    auto begin=i;const auto prev=scan.tokens[i-1].text;
                    if(prev!="{"&&prev!=";"&&prev!="("&&prev!="}")continue;
                    if(prev=="("&&begin>=2&&(scan.tokens[begin-2].text=="for"||scan.tokens[begin-2].text=="catch")){
                        const auto open=begin-1,close=scan.pair[open];auto end=scan.size;
                        if(close+1<scan.tokens.size()){if(scan.tokens[close+1].text=="{")end=scan.end(close+1);else for(auto n=close+1;n<scan.tokens.size();++n)if(scan.tokens[n].text==";"){end=scan.tokens[n].start+1;break;}}
                        if(close<scan.tokens.size()&&caret<=end){
                            auto declaration=begin;if(scan.tokens[declaration].text=="const")++declaration;
                            auto t=read_type(declaration);auto local_name=declaration;
                            if(declaration<scan.tokens.size()&&scan.tokens[declaration].text==":"&&declaration==begin+1){local_name=begin;t="var";}
                            auto after=local_name+1;
                            if(local_name<close&&is_name(scan.tokens[local_name])&&after<=close){
                                if(scan.tokens[after].text==":"&&scan.tokens[begin-2].text=="for"){
                                    if(t=="var")t=range_element(receiver(close).type);
                                    if(!t.empty())add_local(local_name,t);
                                }else if(after==close&&scan.tokens[begin-2].text=="catch")add_local(local_name,t);
                            }
                        }
                    }
                    if(scan.tokens[i].text=="const")++i;
                    auto type=read_type(i);
                    if(type.empty()||i>=scan.tokens.size()||!is_name(scan.tokens[i])||scan.tokens[i].start>=caret){i=begin;continue;}
                    auto name=i++;if(i>=scan.tokens.size()||(scan.tokens[i].text!="="&&scan.tokens[i].text!=";")){i=begin;continue;}
                    auto scope=scan.parent[begin];bool inside=true;
                    if(scope<scan.tokens.size()&&scan.tokens[scope].text=="("&&scope&&scan.tokens[scope-1].text=="for"){
                        auto close=scan.pair[scope];auto end=scan.size;
                        if(close+1<scan.tokens.size()){
                            if(scan.tokens[close+1].text=="{")end=scan.end(close+1);
                            else for(auto n=close+1;n<scan.tokens.size();++n)if(scan.tokens[n].text==";"){end=scan.tokens[n].start+1;break;}
                        }
                        inside=caret<=end;
                    }else if(scope<scan.tokens.size())inside=caret<=scan.end(scope);
                    if(inside){
                        if(type=="var"&&i+1<scan.tokens.size()&&scan.tokens[i].text=="="){
                            auto finish=i+1;while(finish<scan.tokens.size()&&scan.tokens[finish].text!=";"&&scan.tokens[finish].start<caret)++finish;
                            auto inferred=receiver(finish).type;
                            if(scan.tokens[i+1].text=="new"){
                                auto value=i+2;
                                inferred=read_type(value);
                                if(!inferred.empty()&&value<scan.tokens.size()&&scan.tokens[value].text=="["){
                                    const auto close=scan.pair[value];std::size_t rank=1;
                                    for(auto d=value+1;d<close&&d<scan.tokens.size();++d){
                                        if(scan.tokens[d].text==",")++rank;
                                        else if(scan.pair[d]>d&&scan.pair[d]<close)d=scan.pair[d];
                                    }
                                    inferred+=array_suffix(rank);
                                }else if(!inferred.empty())inferred+='*';
                            }else if(inferred.empty()){auto value=i+1;inferred=read_type(value);}
                            if(!inferred.empty())type=inferred;
                            if(type=="var"&&scan.tokens[i+1].kind=="number")type=scan.tokens[i+1].text.find('.')==std::string::npos?"int":"double";
                        }
                        add_local(name,type);
                    }
                    i=begin;
                }
                return;
            }
        }
    }
    std::string range_element(std::string type,bool extensions=true)const {
        type=bare_type(type);if(type=="string")return "char";if(is_array(type))return element_type(type);
        auto base=generic_base(type);auto args=generic_args(type);
        if(base=="Ecs.Query"||base=="Query")return "Ecs.Row";
        if(base=="Ecs.ComponentQuery"||base=="ComponentQuery")return "Ecs.Components";
        if(collection_kind(type)=="Dictionary"&&args.size()==2)return "System.Collections.Generic.KeyValuePair<"+args[0]+","+args[1]+">";
        if(is_collection(type)&&!args.empty())return args[0];
        for(const auto& get:extensions?members(type,false):declared_members(type,false))if(get.name=="GetEnumerator"&&get.category=="method"&&get.required_arguments==0) {
            const auto enumerator=declared_members(get.type,false);
            const bool advances=std::any_of(enumerator.begin(),enumerator.end(),[](const EditorItem& item){return item.name=="MoveNext"&&item.category=="method"&&item.type=="bool"&&item.required_arguments==0;});
            if(advances)for(const auto& current:enumerator)if(current.name=="Current"&&(current.category=="property"||current.category=="field"))return referred(current.type);
        }
        return {};
    }
    static bool is_name(const LexToken& t){return !t.text.empty()&&alpha(t.text[0])&&(t.kind.empty()||t.kind=="type"||t.text=="var");}
    std::string read_type(std::size_t& i)const {
        if(i>=scan.tokens.size()||!is_name(scan.tokens[i]))return {};
        std::string type=scan.tokens[i++].text;
        while(true){
            if(i<scan.tokens.size()&&scan.tokens[i].text=="<"){
                int depth=0;do{auto t=scan.tokens[i++].text;if(t=="<")++depth;else if(t==">")--depth;else if(t==">>")depth-=2;else if(t==";"||t=="{"||t=="}"||t=="=")return {};type+=t;}while(i<scan.tokens.size()&&depth);
                if(depth)return {};
            }
            if(i+1>=scan.tokens.size()||scan.tokens[i].text!="."||!is_name(scan.tokens[i+1]))break;
            type+='.'+scan.tokens[i+1].text;i+=2;
        }
        while(i<scan.tokens.size()){
            auto t=scan.tokens[i].text;
            if(t=="*"||t=="&"||t=="?"){type+=t;++i;}
            else if(t=="["){
                auto close=i+1;
                while(close<scan.tokens.size()&&scan.tokens[close].text==",")++close;
                if(close>=scan.tokens.size()||scan.tokens[close].text!="]")break;
                type+=array_suffix(close-i);i=close+1;
            }
            else break;
        }
        return type;
    }
    const Class* find_class(std::string name)const {
        if(!program)return nullptr;
        name=bare_type(std::move(name));
        auto lookup=[&](const std::string& value)->const Class* {
            if(const auto* c=program->find_class(value))return c;
            if(const auto* c=program->find_class(generic_base(value)))return c;
            return program->find_class(generic_declaration_name(value));
        };
        // A lexical nested type shadows a same-named top-level declaration.
        for(auto scope=owner;scope;scope=scope->declaring_type.empty()?nullptr:program->find_class(scope->declaring_type))
            if(const auto* c=lookup(scope->name+"."+name))return c;
        if(const auto* c=lookup(name))return c;
        if(owner){
            if(const auto* c=lookup(owner->namespace_name.empty()?name:owner->namespace_name+"."+name))return c;
            for(const auto& n:owner->imports)if(const auto* c=lookup(n+"."+name))return c;
        }
        name=generic_declaration_name(name);
        const Class* found=nullptr;
        for(const auto& c:program->classes)if(!c.is_global&&generic_base(c.name)==name)return &c;
        for(const auto& c:program->classes)if(!c.is_global&&c.declaring_type.empty()&&c.short_name==name){if(found)return nullptr;found=&c;}
        return found;
    }
    const Enum* find_enum(std::string name)const {
        if(!program)return nullptr;
        name=bare_type(std::move(name));
        auto lookup=[&](const std::string& value)->const Enum* {
            if(const auto* e=program->find_enum(value))return e;
            return program->find_enum(generic_declaration_name(value));
        };
        for(auto scope=owner;scope;scope=scope->declaring_type.empty()?nullptr:program->find_class(scope->declaring_type))
            if(const auto* e=lookup(scope->name+"."+name))return e;
        if(const auto* e=lookup(name))return e;
        if(owner){
            if(const auto* e=lookup(owner->namespace_name.empty()?name:owner->namespace_name+"."+name))return e;
            for(const auto& n:owner->imports)if(const auto* e=lookup(n+"."+name))return e;
        }
        for(const auto& e:program->enums)if(e.declaring_type.empty()&&e.short_name==name)return &e;
        return nullptr;
    }
    bool accessible(const std::string& name)const {
        return program&&type_accessible(*program,name,file,owner?owner->name:std::string{});
    }
    static bool type_item(const EditorItem& item) {
        return item.category=="class"||item.category=="struct"||item.category=="interface"||item.category=="enum";
    }
    static EditorItem class_item(const Class& c) {
        EditorItem item{c.short_name,c.name,c.is_interface?"interface "+c.name:c.is_struct?"struct "+c.name:"class "+c.name,
            c.is_interface?"interface":c.is_struct?"struct":"class",c.is_interface?8:c.is_struct?22:7,{}};
        item.declaration=c.token.at;item.type_parameters=c.generic_parameters;item.type_pack=c.pack_parameter;item.is_static=c.is_static;
        return item;
    }
    static EditorItem enum_item(const Enum& e) {
        EditorItem item{e.short_name,e.name,"enum "+e.name,"enum",13,{}};
        item.declaration=e.token.at;return item;
    }
    void nested_types(const std::string& declaring,std::vector<EditorItem>& out)const {
        if(!program)return;
        const auto declaration=generic_declaration_name(declaring);
        for(const auto& c:program->classes)if((c.declaring_type==declaring||c.declaring_type==declaration)&&
            generic_suffix(c.name)==std::string::npos&&c.delegate_owner.empty()&&accessible(c.name)){
                auto item=class_item(c);item.type=declaring+"."+c.short_name;out.push_back(std::move(item));
            }
        for(const auto& e:program->enums)if((e.declaring_type==declaring||e.declaring_type==declaration)&&accessible(e.name)){
            auto item=enum_item(e);item.type=declaring+"."+e.short_name;out.push_back(std::move(item));
        }
    }
    static std::string default_text(const Expr& e){
        if(e.kind==Expr::Kind::Literal&&e.token.kind==Token::Kind::String)return "\""+e.text+"\"";
        if(e.kind==Expr::Kind::Literal&&e.token.kind==Token::Kind::Character)return "'"+e.text+"'";
        if(e.kind==Expr::Kind::Member&&!e.args.empty())return default_text(*e.args[0])+"."+e.text;
        if(e.kind==Expr::Kind::Unary&&!e.args.empty())return e.text+default_text(*e.args[0]);
        return e.text.empty()?e.token.text:e.text;
    }
    static EditorItem method_item(const Method& m,bool extension_call=false){
        EditorItem item{m.name,m.type,"",m.is_constant_template?"property":"method",m.is_constant_template?21:2,{}};
        item.declaration=m.token.at;
        item.type_parameters=m.generic_parameters;
        item.type_pack=m.pack_parameter;
        item.is_static=m.is_static&&!extension_call;
        item.is_extension=extension_call;
        item.detail=m.is_constructor?(std::string(m.is_implicit?"implicit ":"")+m.name):(m.is_constant_template?"const ":"")+m.type+" "+m.name;
        if(!m.generic_parameters.empty()||!m.pack_parameter.empty()){
            item.detail+='<';for(const auto& g:m.generic_parameters){if(item.detail.back()!='<')item.detail+=", ";item.detail+=g;}
            if(!m.pack_parameter.empty()){if(item.detail.back()!='<')item.detail+=", ";item.detail+=m.pack_parameter+"...";}item.detail+='>';
        }
        if(!m.is_constant_template){
            item.detail+='(';
            for(const auto& a:m.parameters){
                if(extension_call&&a.is_this)continue;
                if(item.detail.back()!='(')item.detail+=", ";
                const auto type=a.variadic&&is_array(a.type)?element_type(a.type):a.type;
                auto parameter=std::string(a.is_this?"this ":"")+(a.constant?"const ":"")+type+((a.variadic||a.pack)?"...":"")+" "+a.name;
                if(a.default_value)parameter+=" = "+default_text(*a.default_value);
                if(!a.default_value&&!a.variadic&&!a.pack)++item.required_arguments;
                item.value_pack|=a.variadic||a.pack;item.detail+=parameter;item.parameters.push_back(std::move(parameter));
            }
            item.detail+=')';
        }
        if(m.is_conversion){item.name=m.type;item.detail=std::string(m.is_implicit?"implicit ":"explicit ")+m.type+"()";}
        bool first_constraint=true;
        for(const auto& group:m.constraints)for(const auto& capability:group.concepts){
            item.detail+=first_constraint?" : ":", ";first_constraint=false;
            item.detail+=capability;
            if(capability.find('<')==std::string::npos&&!group.parameter.empty())item.detail+="<"+group.parameter+">";
        }
        return item;
    }
    static std::string constant_type(const EditorItem& item,const std::string& generic){
        auto result=item.type;
        if(item.type_parameters.empty()||generic.empty())return result;
        const auto args=generic_args(item.name+generic);
        if(args.size()!=item.type_parameters.size())return result;
        const auto tokens=lex(result);
        for(auto token=tokens.rbegin();token!=tokens.rend();++token){
            auto parameter=std::find(item.type_parameters.begin(),item.type_parameters.end(),token->text);
            if(parameter!=item.type_parameters.end())result.replace(token->start,token->length,args[static_cast<std::size_t>(parameter-item.type_parameters.begin())]);
        }
        return result;
    }
    static std::string substitute(std::string value,const std::map<std::string,std::string>& mapping,
                                  const std::vector<std::string>& shadowed={}) {
        auto tokens=lex(value);
        for(auto it=tokens.rbegin();it!=tokens.rend();++it){
            if(std::find(shadowed.begin(),shadowed.end(),it->text)!=shadowed.end())continue;
            if(auto m=mapping.find(it->text);m!=mapping.end())value.replace(it->start,it->length,m->second);
        }
        return value;
    }
    static bool mentions_type_parameter(const std::string& type,const std::string& name) {
        if(name.empty())return false;
        for(const auto& token:lex(type))if(token.text==name)return true;
        return false;
    }
    static std::string substitute_pack_type(const std::string& type,const std::string& name,const std::vector<std::string>& pack) {
        if(is_array(type))return substitute_pack_type(element_type(type),name,pack)+array_suffix(array_rank(type));
        if(is_pointer(type)||is_reference(type)||is_optional(type))return substitute_pack_type(element_type(type),name,pack)+type.back();
        auto arguments=generic_args(type);
        if(arguments.empty())return type;
        std::vector<std::string> expanded;
        for(const auto& argument:arguments) {
            if(argument.ends_with("...")&&mentions_type_parameter(argument,name)) {
                auto pattern=argument.substr(0,argument.size()-3);
                for(const auto& element:pack)expanded.push_back(substitute(pattern,{{name,element}}));
            }else expanded.push_back(substitute_pack_type(argument,name,pack));
        }
        return expanded.empty()?generic_base(type)+"<>":generic_name(generic_base(type),expanded);
    }
    void class_members(const Class& c,bool stat,std::vector<EditorItem>& out,std::set<std::string>& visited,
                       bool unqualified=false,std::string concrete={},std::string_view wanted={})const {
        if(concrete.empty())concrete=c.name;
        if(visited.size()>=64||!visited.insert(concrete).second)return;
        const auto args=generic_args(concrete);std::map<std::string,std::string> mapping;
        // Broken-source snapshots contain templates, so recover substitutions
        // from each explicit enclosing type in Outer<int>.Inner<string>.
        int generic_depth=0;
        for(std::size_t i=0;i<concrete.size();++i){
            if(concrete[i]=='<')++generic_depth;else if(concrete[i]=='>')--generic_depth;
            if(concrete[i]!='.'||generic_depth)continue;
            const auto prefix=concrete.substr(0,i);const auto arguments=generic_args(prefix);
            if(arguments.empty())continue;
            if(const auto* enclosing=find_class(prefix))if(const auto* declaration=program->find_class(generic_declaration_name(enclosing->name)))
                for(std::size_t n=0;n<std::min(arguments.size(),declaration->generic_parameters.size());++n)
                    mapping[declaration->generic_parameters[n]]=arguments[n];
        }
        if(args.size()>=c.generic_parameters.size())for(std::size_t n=0;n<c.generic_parameters.size();++n)mapping[c.generic_parameters[n]]=args[n];
        const bool expand_pack=!c.pack_parameter.empty()&&args.size()>=c.generic_parameters.size()&&
            !(unqualified&&concrete==c.name)&&std::none_of(args.begin(),args.end(),[](const auto& type){return type.ends_with("...");});
        std::vector<std::string> pack;
        if(expand_pack)pack.assign(args.begin()+static_cast<std::ptrdiff_t>(c.generic_parameters.size()),args.end());
        auto pack_type=[&](const std::string& type){return expand_pack?substitute_pack_type(type,c.pack_parameter,pack):type;};
        auto access=[&](bool pub,bool prot){return pub||(&c==owner)||(prot&&owner&&program&&derives_from(*program,owner->name,c.name));};
        auto matches=[&](const std::string& name){return wanted.empty()||wanted==name;};
        auto mode=[&](bool is_static){return unqualified?(!method||!method->is_static||is_static):is_static==stat;};
        auto add=[&](EditorItem item){
            auto shadowed=item.type_parameters;if(!item.type_pack.empty())shadowed.push_back(item.type_pack);
            item.type=substitute(item.type,mapping,shadowed);
            item.detail=substitute(item.detail,mapping,shadowed);
            for(auto& argument:item.parameters)argument=substitute(argument,mapping,shadowed);
            out.push_back(std::move(item));
        };
        if(stat||unqualified)nested_types(concrete,out);
        for(const auto& f:c.fields)if(matches(f.name)&&!f.name.starts_with("@")&&access(f.is_public,f.is_protected)&&mode(f.is_static)){
            const auto type=pack_type(f.is_event&&!f.event_type.empty()?f.event_type:f.type);
            add({f.name,type,(f.is_event?"event ":"")+type+" "+f.name,f.is_event?"event":f.native_getter.empty()?"field":"property",f.is_event?23:f.native_getter.empty()?5:10,{},{},f.is_static});
            out.back().declaration=f.token.at;
            if(f.is_event)out.back().event_owner=c.name;
        }
        for(const auto& p:c.properties)if(matches(p.name)&&!p.is_indexer&&access(p.is_public,p.is_protected)&&mode(p.is_static)) {
            const auto type=pack_type(p.type);std::string detail=type+" "+p.name+" { ";
            if(!p.getter.empty())detail+="get; ";
            if(!p.setter.empty()){
                const Method* setter=nullptr;for(const auto& m:c.methods)if(m.name==p.setter){setter=&m;break;}
                if(setter&&!setter->is_public)detail+=setter->is_protected?"protected ":"private ";
                detail+="set; ";
            }
            detail+="}";add({p.name,type,detail,"property",10,{},{},p.is_static});out.back().declaration=p.token.at;
        }
        for(const auto& m:c.methods)if(matches(m.name)&&m.property_name.empty()&&!m.is_conversion&&!m.is_constructor&&!m.is_destructor&&!m.is_specialization&&access(m.is_public,m.is_protected)&&mode(m.is_static)) {
            const bool shadowed=m.pack_parameter==c.pack_parameter||std::find(m.generic_parameters.begin(),m.generic_parameters.end(),c.pack_parameter)!=m.generic_parameters.end();
            if(expand_pack&&!shadowed) {
                auto expanded=clone(m);expanded.type=pack_type(m.type);expanded.parameters.clear();
                for(const auto& parameter:m.parameters) {
                    if(!parameter.pack||!mentions_type_parameter(parameter.type,c.pack_parameter)){auto retained=parameter;retained.type=pack_type(parameter.type);expanded.parameters.push_back(std::move(retained));continue;}
                    for(std::size_t n=c.generic_parameters.size();n<args.size();++n) {
                        auto element=parameter;element.pack=false;
                        element.type=substitute(parameter.type,{{c.pack_parameter,args[n]}});
                        element.name+=std::to_string(n-c.generic_parameters.size());
                        expanded.parameters.push_back(std::move(element));
                    }
                }
                add(method_item(expanded));
            }else add(method_item(m));
        }
        auto inherit=[&](const std::string& name){
            if(name.empty())return;
            const auto type=substitute(pack_type(name),mapping);
            if(const auto* base=find_class(type))class_members(*base,stat,out,visited,unqualified,type,wanted);
        };
        inherit(c.base);
        // Before semantic analysis the parser stores all bases in interfaces.
        for(const auto& name:c.interfaces)inherit(name);
    }
    std::vector<EditorItem> declared_members(std::string type,bool stat)const {
        std::vector<EditorItem> out;type=bare_type(std::move(type));
        auto add=[&](std::string name,std::string result,std::vector<std::string> args={}){
            auto detail=result+" "+name+"(";for(const auto& a:args){if(detail.back()!='(')detail+=", ";detail+=a;}detail+=")";
            out.push_back({std::move(name),std::move(result),std::move(detail),"method",2,std::move(args),{},stat});
            if(stat&&(type=="Math"||type=="Random")&&out.back().name!="Seed") {
                out.back().type_parameters={"T"};
                auto at=out.back().detail.find('(');out.back().detail.insert(at,"<T>");
            }
        };
        auto property=[&](std::string name,std::string result,std::string kind="property"){auto category=kind=="enumMember"?20:21;out.push_back({name,result,result+" "+name,std::move(kind),category,{},{},stat});};
        if(is_optional(type)&&!stat){property("HasValue","bool");property("Value",element_type(type));return out;}
        if(type=="string"){
            if(stat){add("IsNullOrEmpty","bool",{"string value"});add("IsNullOrWhiteSpace","bool",{"string value"});add("Join","string",{"string separator","string[] values"});add("FromUtf8","string",{"byte[] bytes"});add("Equals","bool",{"string a","string b"});add("Compare","int",{"string a","string b"});return out;}
            property("Length","int");property("ByteLength","int");add("ToString","string");
            for(auto n:{"Trim","TrimStart","TrimEnd","ToUpper","ToLower"})add(n,"string");
            for(auto n:{"Contains","StartsWith","EndsWith","Equals"})add(n,"bool",{"string value"});add("Contains","bool",{"char value"});
            for(auto n:{"IndexOf","LastIndexOf"}){add(n,"int",{"string value"});add(n,"int",{"char value"});}
            add("IndexOf","int",{"string value","int start"});add("IndexOf","int",{"char value","int start"});
            for(auto n:{"Substring","Remove"}){add(n,"string",{"int start"});add(n,"string",{"int start","int count"});}
            add("Insert","string",{"int index","string value"});add("Replace","string",{"string oldValue","string newValue"});add("Replace","string",{"char oldValue","char newValue"});
            for(auto n:{"PadLeft","PadRight"}){add(n,"string",{"int totalWidth"});add(n,"string",{"int totalWidth","char padding"});}
            add("Split","string[]",{"string separator"});add("Split","string[]",{"char separator"});add("Split","string[]",{"string separator","bool removeEmpty"});add("Split","string[]",{"char separator","bool removeEmpty"});
            add("ToCharArray","char[]");add("ToUtf8","byte[]");add("CompareTo","int",{"string other"});return out;
        }
        if(stat&&type=="Math"){
            for(auto n:{"Pi","E","Tau"}){property(n,"double");out.back().detail="Math."+std::string(n)+" constant; optional <float> or <double> (default double)";}
            for(auto n:{"Abs","Round","Floor","Ceil","Ceiling","Truncate","Sign","Sin","Cos","Tan","Asin","Acos","Atan","Sqrt","Exp","Log","Log2","Log10"})add(n,"T",{"T value"});
            for(auto n:{"Min","Max","Pow","Atan2"})add(n,"T",{"T x","T y"});
            add("Clamp","T",{"T value","T min","T max"});return out;
        }
        if(stat&&type=="Console"){
            if(const auto* c=find_class(type)){std::set<std::string> visited;class_members(*c,true,out,visited);return out;}
            return out;
        }
        if(stat&&(is_numeric(type)||type=="int"||type=="uint")){
            type=type=="int"?"int":type=="uint"?"uint":type;
            for(auto n:{"MinValue","MaxValue"}){property(n,type);out.back().detail=type+" constant";}
            add("Parse",type,{"string text"});
            add("TryParse","bool",{"string text",type+"& value"});
            return out;
        }
        if(stat&&type=="Random"){
            if(program)if(const auto* shared=program->find_class("System.SharedRandom")){
                std::set<std::string> visited;class_members(*shared,true,out,visited);return out;
            }
            add("Next","T",{"T max"});add("Next","T",{"T min","T max"});add("Seed","void",{"ulong value"});return out;}
        if(stat&&type=="Runtime"){for(auto n:{"Execute","ExecuteProgram","ExecuteFile","ExecuteJit","ExecuteProgramJit","ExecuteFileJit"})add(n,"int",{"string source"});return out;}
        if(stat&&type=="Type"){add("Name","string");out.back().detail="string Name<T>()";out.back().type_parameters={"T"};return out;}
        if(const auto* c=find_class(type)){
            if(!accessible(c->name))return out;
            std::set<std::string> visited;class_members(*c,stat,out,visited,false,type);
            return out;
        }
        if(const auto* e=find_enum(type)){if(!accessible(e->name))return out;if(stat){for(const auto& m:e->members){property(m.name,e->name,"enumMember");out.back().declaration=m.token.at;}return out;}add("ToString","string");return out;}
        if(stat&&program){
            // Offer only the next namespace component for qualified insertions.
            std::set<std::string> namespaces;
            const auto prefix=type+".";
            auto child_namespace=[&](const std::string& name){
                const auto start=prefix.size();
                const auto child=name.substr(start,name.find('.',start)-start);
                if(namespaces.insert(child).second)out.push_back({child,prefix+child,"namespace "+prefix+child,"namespace",9,{}});
            };
            for(const auto& c:program->classes){
                if(c.is_global||!c.declaring_type.empty()||!c.delegate_owner.empty()||c.name.find('<')!=std::string::npos)continue;
                if(c.namespace_name==type&&accessible(c.name))out.push_back(class_item(c));
                else if(c.namespace_name.starts_with(prefix)&&accessible(c.name))child_namespace(c.namespace_name);
            }
            for(const auto& e:program->enums){
                if(!e.declaring_type.empty())continue;
                if(e.namespace_name==type&&accessible(e.name))out.push_back(enum_item(e));
                else if(e.namespace_name.starts_with(prefix)&&accessible(e.name))child_namespace(e.namespace_name);
            }
        }
        if(!stat){
            if(is_primitive(type)||is_collection(type))add("ToString","string");
            if(type=="string"||is_array(type)||is_collection(type)){property("Count","int");property("Length","int");}
            if(is_array(type)){property("Rank","int");add("GetLength","int",{"int dimension"});}
            const auto kind=collection_kind(type);const auto args=generic_args(type);const auto first=args.empty()?"T":args[0];
            if(!kind.empty()){
                add("Clear","void");
                if(kind!="Dictionary")add("Contains","bool",{first+" value"});
                if(kind=="List"||kind=="HashSet"){add("Add",kind=="List"?"void":"bool",{first+" value"});add("Remove","bool",{first+" value"});}
                if(kind=="List"){add("RemoveAt","void",{"int index"});add("Insert","void",{"int index",first+" value"});add("IndexOf","int",{first+" value"});}
                if(kind=="Queue"){add("Enqueue","void",{first+" value"});add("Peek",first);add("Dequeue",first);add("TryDequeue","bool",{first+"& value"});}
                if(kind=="Dictionary"){
                    const auto second=args.size()>1?args[1]:"TValue";
                    for(auto n:{"Add","Set"})add(n,"void",{first+" key",second+" value"});
                    add("Remove","bool",{first+" key"});add("ContainsKey","bool",{first+" key"});add("TryGetValue","bool",{first+" key",second+"& value"});
                }
            }
        }
        return out;
    }
    std::string extension_type(std::string type,const Class& scope)const {
        if(is_reference(type)||is_pointer(type)||is_optional(type))return extension_type(element_type(type),scope)+type.back();
        if(is_array(type))return extension_type(element_type(type),scope)+array_suffix(array_rank(type));
        auto base=generic_base(type);auto arguments=generic_args(type);
        auto lookup=[&](const std::string& name)->const Class* {
            if(const auto* found=program->find_class(name))return found;
            return program->find_class(generic_declaration_name(name));
        };
        const Class* found=lookup(base);
        if(!found&&!scope.namespace_name.empty())found=lookup(scope.namespace_name+"."+base);
        if(!found)for(const auto& imported:scope.imports)if((found=lookup(imported+"."+base)))break;
        if(!found)found=find_class(base);
        if(found)base=generic_base(found->name);
        else {
            const auto* enumeration=program->find_enum(base);
            if(!enumeration&&!scope.namespace_name.empty())enumeration=program->find_enum(scope.namespace_name+"."+base);
            if(!enumeration)for(const auto& imported:scope.imports)if((enumeration=program->find_enum(imported+"."+base)))break;
            if(!enumeration)enumeration=find_enum(base);
            if(enumeration)base=enumeration->name;
        }
        if(arguments.empty())return base;
        for(auto& argument:arguments)argument=extension_type(argument,scope);
        return generic_name(base,arguments);
    }
    bool extension_receiver(const std::string& pattern,const std::string& actual,const Method& candidate,
                            std::map<std::string,std::string>& mapping,bool conversions=true)const {
        const auto p=referred(pattern),a=referred(actual);
        if(std::find(candidate.generic_parameters.begin(),candidate.generic_parameters.end(),p)!=candidate.generic_parameters.end()) {
            auto [it,inserted]=mapping.emplace(p,a);return inserted||it->second==a;
        }
        if(is_array(p)||is_array(a))return is_array(p)&&is_array(a)&&array_rank(p)==array_rank(a)&&extension_receiver(element_type(p),element_type(a),candidate,mapping,false);
        if(is_pointer(p)||is_pointer(a))return is_pointer(p)&&is_pointer(a)&&extension_receiver(element_type(p),element_type(a),candidate,mapping,false);
        if(is_optional(p)||is_optional(a))return is_optional(p)&&is_optional(a)&&extension_receiver(element_type(p),element_type(a),candidate,mapping,false);
        const auto patterns=generic_args(p),types=generic_args(a);
        if(!patterns.empty()&&generic_base(p)==generic_base(a)&&patterns.size()==types.size()) {
            for(std::size_t i=0;i<patterns.size();++i)if(!extension_receiver(patterns[i],types[i],candidate,mapping,false))return false;
            return true;
        }
        return p==a||(conversions&&((is_numeric(p)&&is_numeric(a))||derives_from(*program,a,p)));
    }
    std::vector<EditorItem> members(std::string type,bool stat)const {
        auto out=declared_members(type,stat);
        if(stat||!program||type.empty())return out;
        for(const auto& c:program->classes) {
            if(!c.is_static||c.is_template_member||!c.generic_parameters.empty()||!c.pack_parameter.empty()||!accessible(c.name))continue;
            bool visible=c.namespace_name==(owner?owner->namespace_name:std::string{});
            if(owner)for(const auto& imported:owner->imports)visible|=imported==c.name||imported==c.namespace_name;
            visible|=std::find(program->implicit_static_imports.begin(),program->implicit_static_imports.end(),c.name)!=program->implicit_static_imports.end();
            if(!visible)continue;
            for(const auto& m:c.methods) {
                if(!m.is_extension||m.is_specialization||m.parameters.empty()||(!m.is_public&&(&c!=owner)))continue;
                std::map<std::string,std::string> mapping;
                const auto actual=extension_type(type,owner?*owner:c);
                if(!extension_receiver(extension_type(m.parameters.front().type,c),actual,m,mapping))continue;
                auto item=method_item(m,true);item.receiver_types=mapping;
                item.type=substitute(item.type,mapping);item.detail=substitute(item.detail,mapping);
                for(auto& parameter:item.parameters)parameter=substitute(parameter,mapping);
                out.push_back(std::move(item));
            }
        }
        return out;
    }
    std::vector<EditorItem> visible(std::string_view wanted={})const {
        auto matches=[&](const std::string& name){return wanted.empty()||wanted==name;};
        std::vector<EditorItem> out;
        // Reverse order makes the innermost local win during completion dedup.
        for(auto i=locals.rbegin();i!=locals.rend();++i)if(matches(i->name))out.push_back(*i);
        if(owner){std::set<std::string> visited;class_members(*owner,false,out,visited,true,{},wanted);}
        // Enclosing types contribute type names, never an implicit outer instance.
        for(auto scope=owner;scope&&!scope->declaring_type.empty();){
            scope=program->find_class(scope->declaring_type);if(scope)nested_types(scope->name,out);
        }
        if(program){
            for(const auto& c:program->classes){
                if(!c.delegate_owner.empty())continue;
                if(c.is_global){if(!owner||c.namespace_name==owner->namespace_name)for(const auto& m:c.methods)if(matches(m.name))out.push_back(method_item(m));}
                else if(matches(c.short_name)&&c.declaring_type.empty()&&accessible(c.name)&&c.name.find('<')==std::string::npos&&
                    (c.namespace_name.empty()||!owner||c.namespace_name==owner->namespace_name||std::find(owner->imports.begin(),owner->imports.end(),c.namespace_name)!=owner->imports.end()))
                    out.push_back(class_item(c));
            }
            for(const auto& e:program->enums)if(matches(e.short_name)&&e.declaring_type.empty()&&accessible(e.name)&&(e.namespace_name.empty()||!owner||e.namespace_name==owner->namespace_name||std::find(owner->imports.begin(),owner->imports.end(),e.namespace_name)!=owner->imports.end()))out.push_back(enum_item(e));
            for(const auto& n:program->implicit_static_imports)if(const auto* c=program->find_class(n))for(const auto& m:c->methods)if(matches(m.name)&&m.is_public)out.push_back(method_item(m));
        }
        for(auto k:{"Console","Math","Random","Runtime","Type","List","HashSet","Dictionary","Queue"}){
            if(!wanted.empty()&&wanted!=k)continue;
            out.push_back({k,k,"Bolt standard library","class",7,{}});
            if(program&&std::string_view(k)=="Console")if(const auto* type=program->find_class("System.Console"))out.back()=class_item(*type);
            if(program&&std::string_view(k)=="Random")if(const auto* type=program->find_class("System.SharedRandom")){
                out.back().declaration=type->token.at;out.back().detail="Random (System.SharedRandom)";
            }
        }
        if(wanted.empty()||wanted=="Reinterpret")out.push_back({"Reinterpret","T","T Reinterpret<T>(value)","method",2,{"value"},{"T"},true});
        return out;
    }
    static std::string argument_parameter_type(const std::string& parameter) {
        auto value=parameter;auto equals=value.find(" = ");if(equals!=value.npos)value.resize(equals);
        auto split=value.rfind(' ');if(split!=value.npos)value.resize(split);
        if(value.starts_with("const "))value.erase(0,6);
        if(value.starts_with("this ")){value.erase(0,5);if(value.starts_with("const "))value.erase(0,6);}
        if(value.ends_with("..."))value.resize(value.size()-3);
        return value;
    }
    static bool infer(const std::string& pattern,const std::string& actual,const std::vector<std::string>& parameters,
                      std::map<std::string,std::string>& mapping) {
        const auto p=bare_type(pattern),a=bare_type(actual);
        if(std::find(parameters.begin(),parameters.end(),p)!=parameters.end()){
            auto [i,inserted]=mapping.emplace(p,a);return inserted||i->second==a;
        }
        if(is_array(p)&&is_array(a))return infer(element_type(p),element_type(a),parameters,mapping);
        auto ps=generic_args(p),as=generic_args(a);
        if(!ps.empty()&&generic_base(p)==generic_base(a)&&ps.size()==as.size()){
            for(std::size_t i=0;i<ps.size();++i)if(!infer(ps[i],as[i],parameters,mapping))return false;
        }
        return true;
    }
    std::string argument_type(std::size_t begin,std::size_t end,unsigned depth)const {
        if(begin>=end||depth>32)return {};
        const bool negative=scan.tokens[begin].text=="-";
        if(end-begin==2&&(scan.tokens[begin].text=="-"||scan.tokens[begin].text=="+"))++begin;
        if(begin+1==end){
            const auto& token=scan.tokens[begin];
            if(token.kind=="string")return "string";
            if(token.kind=="character")return "char";
            if(token.kind=="number"){
                auto digits=token.text;std::string suffix;
                while(!digits.empty()&&std::isalpha(static_cast<unsigned char>(digits.back()))){
                    suffix.insert(suffix.begin(),static_cast<char>(std::tolower(static_cast<unsigned char>(digits.back()))));digits.pop_back();
                }
                if(suffix=="f")return "float";
                if(digits.find_first_of(".eE")!=std::string::npos)return suffix.empty()?"double":"";
                if(suffix!=""&&suffix!="u"&&suffix!="l"&&suffix!="ul"&&suffix!="lu")return {};
                const bool unsign=suffix.find('u')!=std::string::npos,wide=suffix.find('l')!=std::string::npos;
                if(negative&&unsign)return {};
                try {
                    std::size_t parsed=0;const auto value=std::stoull(digits,&parsed);
                    if(parsed!=digits.size())return {};
                    if(unsign)return !wide&&value<=std::numeric_limits<std::uint32_t>::max()?"uint":"ulong";
                    const auto signed_max=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
                    if(value>signed_max+(negative?1:0))return !wide&&!negative?"ulong":"";
                    const auto int_max=static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
                    return !wide&&value<=int_max+(negative?1:0)?"int":"long";
                }catch(const std::exception&){return {};}
            }
            if(token.text=="true"||token.text=="false")return "bool";
            if(token.text=="null")return "#null";
        }
        // Simple name, field, call and indexer chains use their resolved result.
        // Unknown expression types stay unknown rather than inventing a type.
        return receiver(end,depth+1).type;
    }
    std::string call_result(std::size_t open,std::size_t close,unsigned depth)const {
        if(!open||depth>32)return {};
        auto name=open-1;std::vector<std::string> explicit_types;
        if(scan.tokens[name].text==">"){
            auto end=name;int nesting=1;
            while(name>0&&nesting){--name;if(scan.tokens[name].text==">")++nesting;else if(scan.tokens[name].text=="<")--nesting;}
            if(nesting||!name)return {};
            std::string generic="F";for(auto i=name;i<=end;++i)generic+=scan.tokens[i].text;
            explicit_types=generic_args(generic);--name;
        }
        std::vector<EditorItem> candidates;
        if(name&&scan.tokens[name-1].text=="."){
            auto base=receiver(name-1,depth+1);candidates=members(base.type,base.stat);
        }else if(name>=2&&scan.tokens[name-1].text==">"&&scan.tokens[name-2].text=="-"){
            auto base=receiver(name-2,depth+1);candidates=members(base.type,false);
        }else candidates=visible(scan.tokens[name].text);
        std::vector<std::string> argument_types;
        if(open+1<close){
            std::size_t begin=open+1;
            for(auto i=begin;i<=close;++i){
                if(i==close||scan.tokens[i].text==","){
                    argument_types.push_back(argument_type(begin,i,depth+1));begin=i+1;
                }else if(scan.pair[i]>i&&scan.pair[i]<close)i=scan.pair[i];
                else if(scan.tokens[i].text=="<"){
                    // Generic calls can themselves contain comma-separated type arguments.
                    auto finish=i+1;int nesting=1;
                    for(;finish<close;++finish){if(scan.tokens[finish].text=="<")++nesting;else if(scan.tokens[finish].text==">"&&!--nesting)break;}
                    if(!nesting&&finish+1<close&&scan.tokens[finish+1].text=="(")i=finish;
                }
            }
        }
        int best=-1;std::string result;bool ambiguous=false,found_callable=false;
        for(const auto& item:candidates){
            if(item.name!=scan.tokens[name].text||item.category!="method")continue;
            found_callable=true;
            if(argument_types.size()<item.required_arguments||(!item.value_pack&&argument_types.size()>item.parameters.size()))continue;
            if(!explicit_types.empty()&&(item.type_pack.empty()?explicit_types.size()!=item.type_parameters.size():explicit_types.size()<item.type_parameters.size()))continue;
            auto mapping=item.receiver_types;bool valid=true;int score=item.is_extension?0:1000;
            for(std::size_t i=0;i<std::min(explicit_types.size(),item.type_parameters.size());++i) {
                const auto type=item.is_extension&&owner?extension_type(explicit_types[i],*owner):explicit_types[i];
                auto [it,inserted]=mapping.emplace(item.type_parameters[i],type);
                if(!inserted&&it->second!=type)valid=false;
            }
            if(!valid)continue;
            std::vector<std::string> pack;
            const bool explicit_pack=!item.type_pack.empty()&&explicit_types.size()>item.type_parameters.size();
            if(explicit_pack)pack.assign(explicit_types.begin()+static_cast<std::ptrdiff_t>(item.type_parameters.size()),explicit_types.end());
            bool pack_known=explicit_pack;
            auto infer_pack=[&](auto&& self,const std::string& pattern,const std::string& actual)->bool {
                if(!mentions_type_parameter(pattern,item.type_pack))return true;
                const auto p=referred(pattern),a=referred(actual);
                if((is_array(p)&&is_array(a))||(is_pointer(p)&&is_pointer(a)))return self(self,element_type(p),element_type(a));
                const auto patterns=generic_args(p),types=generic_args(a);
                if(patterns.empty())return true;
                if(patterns.back().ends_with("...")&&mentions_type_parameter(patterns.back(),item.type_pack)) {
                    if(types.size()+1<patterns.size())return false;
                    std::vector<std::string> found;
                    const auto element_pattern=patterns.back().substr(0,patterns.back().size()-3);
                    for(std::size_t i=patterns.size()-1;i<types.size();++i) {
                        std::map<std::string,std::string> element;
                        if(!infer(element_pattern,types[i],{item.type_pack},element)||!element.contains(item.type_pack))return false;
                        found.push_back(element.at(item.type_pack));
                    }
                    if(pack_known)return pack==found;
                    pack=std::move(found);pack_known=true;return true;
                }
                if(patterns.size()!=types.size())return false;
                for(std::size_t i=0;i<patterns.size();++i)if(!self(self,patterns[i],types[i]))return false;
                return true;
            };
            const bool parameter_pack=item.value_pack&&!item.type_pack.empty()&&!item.parameters.empty()&&
                mentions_type_parameter(argument_parameter_type(item.parameters.back()),item.type_pack);
            if(parameter_pack&&explicit_pack&&pack.size()!=argument_types.size()-(item.parameters.size()-1))continue;
            for(std::size_t i=0;i<argument_types.size();++i){
                auto pattern=argument_parameter_type(item.parameters[std::min(i,item.parameters.size()-1)]);
                const bool packed_argument=parameter_pack&&i+1>=item.parameters.size();
                if(item.value_pack&&!packed_argument&&i+1>=item.parameters.size()&&is_array(argument_types[i]))pattern+="[]";
                if(argument_types[i].empty())continue;
                if(!packed_argument&&!item.type_pack.empty()) {
                    if(!infer_pack(infer_pack,pattern,argument_types[i])){valid=false;break;}
                    if(pack_known)pattern=substitute_pack_type(pattern,item.type_pack,pack);
                }
                if(packed_argument) {
                    const auto index=i-(item.parameters.size()-1);
                    if(!pack_known) {
                        std::map<std::string,std::string> element;
                        if(!infer(pattern,argument_types[i],{item.type_pack},element)||!element.contains(item.type_pack)){valid=false;break;}
                        pack.push_back(element.at(item.type_pack));
                    }
                    if(index>=pack.size()){valid=false;break;}
                    pattern=substitute(pattern,{{item.type_pack,pack[index]}});
                }
                if(!infer(pattern,argument_types[i],item.type_parameters,mapping)){valid=false;break;}
                const auto wanted=bare_type(substitute(pattern,mapping)),actual=bare_type(argument_types[i]);
                if(wanted==actual)score+=2;
                else if(is_numeric(wanted)&&is_numeric(actual))++score;
                else if(wanted=="T"&&item.type_parameters.empty()){} // Console's formatting parameter, not a generic declaration.
                else {valid=false;break;}
            }
            if(!valid)continue;
            const auto candidate=substitute(item.type_pack.empty()?item.type:substitute_pack_type(item.type,item.type_pack,pack),mapping);
            if(score>best){best=score;result=candidate;ambiguous=false;}
            else if(score==best&&result!=candidate)ambiguous=true;
        }
        if(best>=0)return ambiguous?std::string{}:result;
        if(found_callable)return {};
        auto constructed=receiver(open,depth+1);
        return constructed.stat?constructed.type:std::string{};
    }
    struct Receiver {std::string type;bool stat=false;std::string event_owner;};
    Receiver receiver(std::size_t end,unsigned depth=0)const {
        if(!end||depth>32)return {};
        auto i=end-1;const auto& token=scan.tokens[i];
        // Calls, indexers, and explicit generic arguments preserve the returned
        // type so chains like values[0]. and GetPlayer(). are useful too.
        if(token.text==")"){
            const auto open=scan.pair[i];if(open>=i)return {};
            const bool call=open&&(is_name(scan.tokens[open-1])||scan.tokens[open-1].text==">"||
                                   scan.tokens[open-1].text==")"||scan.tokens[open-1].text=="]");
            if(call)return {call_result(open,i,depth+1),false};
            // A parenthesized simple receiver can itself be a member/call chain.
            // Do not guess the result of an arithmetic or conditional expression.
            for(auto n=open+1;n<i;++n){
                if(scan.pair[n]>n&&scan.pair[n]<i){n=scan.pair[n];continue;}
                if(!is_name(scan.tokens[n])&&scan.tokens[n].kind!="string"&&scan.tokens[n].text!="this"&&scan.tokens[n].text!=".")return {};
            }
            return open+1<i?receiver(i,depth+1):Receiver{};

        }
        if(token.text=="]"){
            auto open=scan.pair[i];if(open>=i)return {};
            auto base=receiver(open,depth+1);if(is_array(base.type))return {element_type(base.type),false};
            const auto args=generic_args(base.type);if(!args.empty())return {collection_kind(base.type)=="Dictionary"&&args.size()>1?args[1]:args[0],false};return {};
        }
        if(token.kind=="string")return {"string",false};
        if(token.kind=="character")return {"char",false};
        std::string generic;
        if(token.text==">"){
            int nesting=1;auto open=i;while(open>0&&nesting){--open;if(scan.tokens[open].text==">")++nesting;else if(scan.tokens[open].text=="<")--nesting;}
            if(nesting||!open)return {};
            for(auto n=open;n<=i;++n)generic+=scan.tokens[n].text;
            i=open-1;
        }
        auto name=scan.tokens[i].text;
        if(name=="this"&&owner&&method&&!method->is_static)return {owner->name,false};
        if(i&&scan.tokens[i-1].text=="."){
            auto base=receiver(i-1,depth+1);
            if(base.stat&&base.type=="Math"&&(name=="Pi"||name=="E"||name=="Tau")&&!generic.empty()){
                const auto args=generic_args(name+generic);
                if(args.size()==1&&is_floating(args[0]))return {args[0],false};
            }
            for(const auto& m:members(base.type,base.stat))if(m.name==name)return {type_item(m)?m.type+generic:constant_type(m,generic),type_item(m)||m.category=="namespace",m.event_owner};
            auto qualified=base.type.empty()?std::string{}:base.type+"."+name;
            if(const auto* c=find_class(qualified))return {c->name+generic,true};
            if(const auto* e=find_enum(qualified))return {e->name,true};
            return {qualified,true};
        }
        if(i>=2&&scan.tokens[i-1].text==">"&&scan.tokens[i-2].text=="-"){
            auto base=receiver(i-2,depth+1);for(const auto& m:members(base.type,base.stat))if(m.name==name)return {m.type,false,m.event_owner};return {};
        }
        for(const auto& item:visible(name))if(item.name==name){
            const bool type=type_item(item);
            if(type&&name=="Random"&&program&&program->find_class("System.SharedRandom")&&
               i+1<scan.tokens.size()&&scan.tokens[i+1].text==".")return {"Random",true};
            return {type?item.type+generic:constant_type(item,generic),type,item.event_owner};
        }
        if(primitives.contains(name)||name=="int"||name=="uint")return {name,true};
        if(const auto* c=find_class(name))return {c->name+generic,true};
        if(const auto* e=find_enum(name))return {e->name,true};
        // A namespace chain is resolved by its final qualified type name.
        if(program){
            for(const auto& c:program->classes)if(c.namespace_name==name||c.namespace_name.starts_with(name+"."))return {name,true};
            for(const auto& e:program->enums)if(e.namespace_name==name||e.namespace_name.starts_with(name+"."))return {name,true};
        }
        return {};
    }
    std::vector<EditorItem> receiver_members(const Receiver& r)const {
        if(!r.event_owner.empty()) {
            // Subscription syntax is allowed outside the declaring type, but
            // events never expose the delegate or its implementation identity.
            if(!owner||generic_base(owner->name)!=generic_base(r.event_owner))return {};
            auto type=r.type;const auto arguments=generic_args(type);
            if(generic_base(type)=="Action")type="System."+type;
            auto result=members(type,false);
            std::erase_if(result,[](const EditorItem& item){return item.name!="Invoke";});return result;
        }
        return members(r.type,r.stat);
    }
    std::vector<EditorItem> candidates(std::size_t end,std::string_view wanted={})const {
        if(end&&scan.tokens[end-1].text==".")return receiver_members(receiver(end-1));
        if(end>=2&&scan.tokens[end-1].text==">"&&scan.tokens[end-2].text=="-")return receiver_members(receiver(end-2));
        return visible(wanted);
    }
};
