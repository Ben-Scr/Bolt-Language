#include "Json.hpp"
#include "../ParseCache.hpp"
#include "../ProjectInputs.hpp"
#include <bolt/Version.hpp>
#include <bolt/Tooling.hpp>
#include <bolt/Project.hpp>
#include <bolt/Path.hpp>
#include <bolt/Types.hpp>
#include <bolt/TypeAccess.hpp>
#include <bolt/native/Layout.hpp>
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <fstream>
#include <functional>
#include <iostream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace bolt {
namespace {
namespace fs=std::filesystem;
using J=tooling::Json;using O=J::Object;using A=J::Array;
constexpr std::size_t max_message=8*1024*1024, max_document=4*1024*1024, max_workspace=32*1024*1024;
const std::vector<std::string> token_types={"namespace","type","class","enum","typeParameter","parameter","variable","property","enumMember","function","method","keyword","string","number","comment","operator","interface","identifier","struct","field","character","concept","decorator","macro","event"};
const std::unordered_set<std::string> keywords={"sizeof","event","implicit","explicit","import","namespace","class","interface","operator","enum","concept","public","private","static","get","set","const","sealed","virtual","override","if","elif","else","while","for","switch","case","return","break","continue","this","true","false","null","default","new","delete","void","var","struct","try","catch","finally","throw","async","await","abstract","require","protected","internal","extern"};
const std::unordered_set<std::string> primitives={"byte","short","ushort","int","uint","long","ulong","float","double","bool","string","char"};
bool alpha(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';}
bool digit(char c){return c>='0'&&c<='9';}
std::string canonical_path(const fs::path& p){return path_to_utf8(fs::weakly_canonical(fs::absolute(p)));}
int unhex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
std::string uri_path(const std::string& uri){
    if(!uri.starts_with("file://"))throw std::invalid_argument("Only local file:// document URIs are supported");
    auto text=uri.substr(7);
    if(text.starts_with("localhost/"))text=text.substr(9);
    if(text.empty()||text[0]!='/')throw std::invalid_argument("Remote file URI authorities are not supported");
    std::string out;
    for(std::size_t i=0;i<text.size();++i){if(text[i]=='%'){if(i+2>=text.size()||unhex(text[i+1])<0||unhex(text[i+2])<0)throw std::invalid_argument("Invalid percent-encoded URI");char c=static_cast<char>((unhex(text[i+1])<<4)|unhex(text[i+2]));if(c==0)throw std::invalid_argument("NUL in file URI");out+=c;i+=2;}else out+=text[i];}
#ifdef _WIN32
    if(out.size()>=3&&out[0]=='/'&&alpha(out[1])&&out[2]==':')out.erase(0,1);
#endif
    return canonical_path(path_from_utf8(out));
}
std::string path_uri(const std::string& file){
    std::string path=canonical_path(path_from_utf8(file));
#ifdef _WIN32
    if(path.size()>=2&&path[1]==':')path='/'+path;
#endif
    std::string result="file://";const char* h="0123456789ABCDEF";
    for(unsigned char c:path){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c==':'||c=='-'||c=='_'||c=='.'||c=='~')result+=static_cast<char>(c);else{result+='%';result+=h[c>>4];result+=h[c&15];}}
    return result;
}
struct Pos{int line=0,character=0;};
Pos position(std::string_view text,std::size_t end){
    Pos p;end=std::min(end,text.size());
    for(std::size_t i=0;i<end;){unsigned char c=static_cast<unsigned char>(text[i]);if(c=='\n'){++p.line;p.character=0;++i;continue;}
        std::size_t width=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;
        if(i+width>end)break;
        p.character+=width==4?2:1;i+=width;}
    return p;
}
std::size_t offset(std::string_view text,const J& pos){
    if(!pos["line"].is_integer()||!pos["character"].is_integer())throw std::invalid_argument("LSP positions need integer line and character");
    auto line=pos["line"].integer(),col=pos["character"].integer();if(line<0||col<0)throw std::invalid_argument("Negative LSP position");
    std::size_t i=0;for(std::int64_t n=0;n<line;++n){auto next=text.find('\n',i);if(next==text.npos)throw std::invalid_argument("Line is outside the document");i=next+1;}
    std::int64_t n=0;
    while(i<text.size()&&text[i]!='\n'&&text[i]!='\r'&&n<col){unsigned char c=static_cast<unsigned char>(text[i]);std::size_t width=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;int units=width==4?2:1;if(n+units>col)throw std::invalid_argument("Position splits a UTF-16 surrogate pair");i+=std::min(width,text.size()-i);n+=units;}
    if(n!=col)throw std::invalid_argument("Character is outside the line");
    return i;
}
J pos_json(Pos p){return O{{"line",p.line},{"character",p.character}};}
// Compiler columns count UTF-8 bytes; protocol columns count UTF-16 units.
// An outline, reference list, or rename can translate thousands of locations
// from the same source snapshot. Index its line starts once.
struct SourceLines {
    std::vector<std::size_t> starts{0};
    explicit SourceLines(std::string_view text){
        for(std::size_t i=0;(i=text.find('\n',i))!=text.npos;)starts.push_back(++i);
    }
    std::size_t line(const Location& at)const{
        return std::min<std::size_t>(std::max(1,at.line)-1,starts.size()-1);
    }
    std::pair<std::size_t,std::size_t> bounds(std::string_view text,const Location& at)const{
        const auto n=line(at);
        if(at.line>0&&static_cast<std::size_t>(at.line)>starts.size())return {text.size(),text.size()};
        auto end=n+1<starts.size()?starts[n+1]-1:text.size();
        if(end>starts[n]&&text[end-1]=='\r')--end;
        auto first=std::min(end,starts[n]+static_cast<std::size_t>(std::max(1,at.column)-1));
        return {first,std::min(end,first+static_cast<std::size_t>(std::max(1,at.length)))};
    }
    J range(std::string_view text,const Location& at)const{
        const auto n=line(at);
        const auto [first,last]=bounds(text,at);
        const auto line_text=text.substr(starts[n]);
        auto begin=position(line_text,first-starts[n]),end=position(line_text,last-starts[n]);
        begin.line=end.line=static_cast<int>(n);
        return O{{"start",pos_json(begin)},{"end",pos_json(end)}};
    }
};
J range_json(std::string_view source,const Location& at){
    std::size_t start=0;for(int n=1;n<at.line;++n){auto next=source.find('\n',start);if(next==source.npos){start=source.size();break;}start=next+1;}
    auto line_end=source.find('\n',start);if(line_end==source.npos)line_end=source.size();if(line_end>start&&source[line_end-1]=='\r')--line_end;
    start=std::min(line_end,start+static_cast<std::size_t>(std::max(1,at.column)-1));
    auto end=std::min(line_end,start+static_cast<std::size_t>(std::max(1,at.length)));
    return O{{"start",pos_json(position(source,start))},{"end",pos_json(position(source,end))}};
}
std::string hash_text(std::string_view text){std::uint64_t hash=14695981039346656037ull;for(unsigned char c:text){hash^=c;hash*=1099511628211ull;}std::ostringstream out;out<<std::hex<<std::setfill('0')<<std::setw(16)<<hash;return out.str();}
struct LexToken{std::size_t start=0,length=0;std::string text,kind;bool inactive=false;};
// Error-tolerant lexical coloring is deliberately separate from compiler parsing:
// unfinished strings/comments still get colored while the user is typing.
std::vector<LexToken> lex(std::string_view source){
    std::vector<LexToken> out;
    std::size_t i=0;
    unsigned nesting=0;
    auto add=[&](std::size_t start,const std::string& kind){
        if(i>start)out.push_back({start,i-start,std::string(source.substr(start,i-start)),kind});
    };
    std::function<void(bool)> code;
    std::function<void()> interpolation;
    interpolation=[&](){
        auto start=i;i+=2;
        while(i<source.size()) {
            const char c=source[i];
            if(c=='\\') {i+=std::min<std::size_t>(2,source.size()-i);continue;}
            if(c=='"') {++i;add(start,"string");return;}
            if(c=='\n'||c=='\r') {add(start,"string");return;}
            if(source.substr(i,2)=="{{"||source.substr(i,2)=="}}") {i+=2;continue;}
            if(c=='{') {
                add(start,"string");start=i++;add(start,"operator");
                if(nesting<128) {++nesting;code(true);--nesting;}
                else {while(i<source.size()&&source[i]!='}')++i;if(i<source.size())++i;}
                start=i;continue;
            }
            ++i;
        }
        add(start,"string");
    };
    code=[&](bool stop_at_brace){
        while(i<source.size()) {
            unsigned char c=static_cast<unsigned char>(source[i]);
            if(std::isspace(c)){++i;continue;}
            auto start=i;std::string kind;
            if(source.substr(i,2)=="//") {kind="comment";while(i<source.size()&&source[i]!='\n')++i;}
            else if(source.substr(i,2)=="/*") {kind="comment";i+=2;while(i<source.size()&&source.substr(i,2)!="*/")++i;if(i<source.size())i+=2;}
            else if(source.substr(i,2)=="$\"") {interpolation();continue;}
            else if(c=='"'||c=='\'') {kind=c=='"'?"string":"character";const char quote=static_cast<char>(c);++i;while(i<source.size()){char t=source[i++];if(t==quote)break;if(t=='\\'&&i<source.size())++i;if(t=='\n')break;}}
            else if(alpha(static_cast<char>(c))) {++i;while(i<source.size()&&(alpha(source[i])||digit(source[i])))++i;auto t=std::string(source.substr(start,i-start));kind=keywords.contains(t)?"keyword":primitives.contains(t)?"type":"";}
            else if(digit(static_cast<char>(c))||(c=='.'&&i+1<source.size()&&digit(source[i+1]))) {
                kind="number";
                while(i<source.size()&&digit(source[i]))++i;
                if(i<source.size()&&source[i]=='.'&&(i+1==source.size()||!alpha(source[i+1]))) {++i;while(i<source.size()&&digit(source[i]))++i;}
                if(i<source.size()&&(source[i]=='e'||source[i]=='E')){++i;if(i<source.size()&&(source[i]=='+'||source[i]=='-'))++i;while(i<source.size()&&digit(source[i]))++i;}
                while(i<source.size()&&(alpha(source[i])||digit(source[i])))++i;
            }
            else {++i;kind="operator";}
            add(start,kind);
            if(stop_at_brace&&c=='}')return;
            if(stop_at_brace&&c=='{'&&nesting<128){++nesting;code(true);--nesting;}
        }
    };
    code(false);
    return out;
}
#include "EditorCompletion.hpp"
bool known_library_member(const std::string& owner,const std::string& name,std::string* detail=nullptr) {
    EditorTokens empty("");EditorContext context(nullptr,empty,"",0);
    const auto type=owner=="Primitive"?"int":owner=="Array"?"int[]":owner;
    for(const auto& member:context.members(type,owner!="Primitive"&&!is_collection(type)&&!is_array(type)))if(member.name==name){if(detail)*detail=member.detail;return true;}
    return owner=="Intrinsic"&&name=="Reinterpret";
}
struct Symbol {std::string id,name,type,category,owner;Location at;int kind=13;};
struct Occurrence {Location at;std::string symbol,category;bool declaration=false;std::string detail;};
struct Index {
    std::map<std::string,Symbol> symbols;
    std::vector<Occurrence> occurrences;
    auto line_begin(const std::string& file,int line)const{
        return std::lower_bound(occurrences.begin(),occurrences.end(),std::tie(file,line),
            [](const Occurrence& hit,const auto& key){return std::tie(hit.at.file,hit.at.line)<key;});
    }
    std::map<std::string,std::string> accessor_properties;
    std::map<std::string,std::string> extension_signatures;
    std::map<std::tuple<std::string,int,int>,std::vector<std::string>> ambiguous;
    static std::string local(std::size_t id){return "local:"+std::to_string(id);}
    void add(Symbol symbol){auto id=symbol.id;if(!symbol.at.file.empty())occurrences.push_back({symbol.at,id,symbol.category,true,{}});symbols.emplace(id,std::move(symbol));}
    void ref(const Token& t,const std::string& id,const std::string& kind,std::string detail={}){occurrences.push_back({t.at,id,kind,false,std::move(detail)});}
    void variables(const Stmt& s,const Method& m){
        if((s.kind==Stmt::Kind::Variable||s.kind==Stmt::Kind::ForEach||s.kind==Stmt::Kind::Catch)&&s.local_id)add({local(s.local_id),s.name,s.type,"variable","",s.name_token.at,13});
        if(s.init)variables(*s.init,m);
        if(s.body)variables(*s.body,m);
        if(s.alternative)variables(*s.alternative,m);
        for(const auto& child:s.statements)variables(*child,m);
    }
    void expr(const Expr& e){
        using B=Expr::Binding;using K=Expr::Kind;
        if(!e.constant_owner.empty()){
            std::string id;
            for(const auto& [key,symbol]:symbols)if(symbol.kind==14&&symbol.owner==e.constant_owner&&symbol.name==e.constant_token.text){id=key;break;}
            ref(e.constant_token,id,"property","const "+e.type+" "+e.constant_token.text);
            return;
        }
        if(!e.callback_owner.empty()&&!e.callback_method.empty()&&e.binding==B::Construct){
            ref(e.token,"method:"+e.callback_owner+":"+e.callback_method,"method");return;
        }
        if((e.kind==K::OptionalHas||e.kind==K::OptionalValue)&&(e.token.text=="HasValue"||e.token.text=="Value"))
            ref(e.token,"","property",e.type+" "+e.text+(e.kind==K::OptionalValue?" (checked optional payload)":" { get; }"));
        if(e.kind==K::Default)ref(e.token,"","keyword",e.type+" default");
        if(e.kind==K::Name||e.kind==K::Member){
            if(e.binding==B::Local){auto id=local(e.local_id);auto it=symbols.find(id);ref(e.token,id,it==symbols.end()?"variable":it->second.category);}
            else if(e.binding==B::This)ref(e.token,"","keyword",e.type+"& this");
            else if(e.binding==B::Field&&!e.text.starts_with("@")){const auto id="field:"+e.owner+":"+e.text;auto found=symbols.find(id);ref(e.token,id,found==symbols.end()?"field":found->second.category);}
            else if(e.binding==B::Method&&e.token.text==e.text){
                auto id=e.resolved.empty()?std::string{}:"method:"+e.owner+":"+e.resolved;
                if(auto it=accessor_properties.find(id);it!=accessor_properties.end())ref(e.token,it->second,"property");
                else ref(e.token,id,"method",e.is_extension&&extension_signatures.contains(id)?extension_signatures.at(id):std::string{});
            }
            else if(e.binding==B::Class){auto id="class:"+e.owner;auto found=symbols.find(id);ref(e.token,id,found==symbols.end()?"class":found->second.category);}
            else if(e.binding==B::Enum)ref(e.token,"enum:"+e.owner,"enum");
            else if(e.binding==B::EnumValue)ref(e.token,"enum-value:"+e.owner+":"+e.text,"enumMember");
            else if(e.binding==B::Namespace)ref(e.token,"","namespace");
            else if(e.binding==B::Builtin){
                std::string detail;
                const auto category=e.kind==K::Member?(known_library_member(e.owner,e.text,is_numeric(e.owner)?&detail:nullptr)?"method":"identifier"):"class";
                ref(e.token,"",category,std::move(detail));
            }
            else if(e.binding==B::Collection)ref(e.token,"",known_library_member(e.owner,e.text)?"method":"identifier");
            else if(e.binding==B::Property){
                std::string detail;
                if(e.owner=="Math")detail="const "+e.type+" Math."+e.text+(e.type_args.empty()?"":"<"+e.type+">");
                if(e.owner=="Arena")detail="ulong Arena."+e.text;
                ref(e.token,"","property",std::move(detail));
            }
        }
        if(e.arena)expr(*e.arena);
        for(const auto& a:e.args)expr(*a);
    }
    void stmt(const Stmt& s){if(s.expression)expr(*s.expression);if(s.condition)expr(*s.condition);if(s.increment)expr(*s.increment);if(s.init)stmt(*s.init);if(s.body)stmt(*s.body);if(s.alternative)stmt(*s.alternative);for(const auto& x:s.statements)stmt(*x);}
    void build(const Program& p){
        symbols.clear();occurrences.clear();accessor_properties.clear();extension_signatures.clear();ambiguous.clear();
        for(const auto& c:p.classes){
            if(!c.is_global&&c.delegate_owner.empty())add({"class:"+c.name,c.short_name,c.name,c.is_interface?"interface":c.is_struct?"struct":"class",c.declaring_type.empty()?c.namespace_name:c.declaring_type,c.token.at,c.is_interface?11:c.is_struct?23:5});
            for(const auto& f:c.fields)if(!f.name.starts_with("@"))add({"field:"+c.name+":"+f.name,f.name,f.is_event?(f.event_type.empty()?f.type:f.event_type):f.type,f.is_event?"event":f.native_getter.empty()?"field":"property",c.name,f.token.at,f.is_event?24:f.native_getter.empty()?8:7});
            for(const auto& prop:c.properties)if(!prop.is_indexer)add({"property:"+c.name+":"+prop.name,prop.name,prop.type,"property",c.name,prop.token.at,7});
            for(const auto& m:c.methods){
                if(!m.property_name.empty()) {
                    accessor_properties["method:"+c.name+":"+m.id]="property:"+c.name+":"+m.property_name;
                    for(const auto& a:m.parameters)if(a.local_id)symbols.emplace(local(a.local_id),Symbol{local(a.local_id),a.name,a.type,"parameter","",a.name_token.at,13});
                    if(m.body)variables(*m.body,m);
                    continue;
                }
                if(m.is_constant_template&&m.is_specialization)continue;
                std::string signature;
                if(m.is_constant_template){
                    signature="const "+m.type+" "+m.name;
                    if(!m.generic_parameters.empty()){signature+="<";for(const auto& parameter:m.generic_parameters){if(signature.back()!='<')signature+=", ";signature+=parameter;}signature+=">";}
                }else{
                    signature=EditorContext::method_item(m).detail;
                }
                auto id=m.is_constant_template?"constant:"+c.name+":"+m.name:m.id.empty()?"template:"+c.name+":"+m.name+":"+std::to_string(m.token.at.offset):"method:"+c.name+":"+m.id;
                if(m.is_extension)extension_signatures[id]=EditorContext::method_item(m,true).detail;
                add({std::move(id),m.name,signature,m.is_constant_template?"property":"method",c.name,m.token.at,m.is_constant_template?14:m.is_constructor?9:6});
                if(m.body){for(const auto& a:m.parameters)if(a.local_id)add({local(a.local_id),a.name,a.type,"parameter","",a.name_token.at,13});variables(*m.body,m);}
            }
        }
        for(const auto& e:p.enums){add({"enum:"+e.name,e.short_name,e.name,"enum",e.declaring_type.empty()?e.namespace_name:e.declaring_type,e.token.at,10});for(const auto& m:e.members)add({"enum-value:"+e.name+":"+m.name,m.name,e.name,"enumMember",e.name,m.token.at,22});}
        for(const auto& c:p.concepts)add({"concept:"+c.name,c.name,"concept "+c.name,"concept",c.namespace_name,c.token.at,26});
        for(const auto& c:p.classes){for(const auto& f:c.fields)if(f.initializer)expr(*f.initializer);for(const auto& m:c.methods)if(m.body&&!m.is_constant_template)stmt(*m.body);}
        // Only the compiler can create these bindings. The parser retained the
        // exact physical token, and semantic analysis selected its declaration.
        for(const auto& binding:p.semantic_references) {
            if(!binding.symbol.empty()&&!binding.declaration.file.empty())symbols.try_emplace(binding.symbol,Symbol{binding.symbol,binding.name,binding.detail,binding.category,binding.owner,binding.declaration,13});
            ref(binding.token,binding.symbol,binding.category,binding.detail);
            if(binding.ambiguous)ambiguous[{binding.token.at.file,binding.token.at.line,binding.token.at.column}].push_back(binding.symbol);
        }
        std::sort(occurrences.begin(),occurrences.end(),[](const auto& a,const auto& b){return std::tie(a.at.file,a.at.line,a.at.column,a.symbol)<std::tie(b.at.file,b.at.line,b.at.column,b.symbol);});
        occurrences.erase(std::unique(occurrences.begin(),occurrences.end(),[](const auto& a,const auto& b){return a.at.file==b.at.file&&a.at.line==b.at.line&&a.at.column==b.at.column&&a.symbol==b.symbol;}),occurrences.end());
    }
};
#include "AnalysisQueue.hpp"
class Server {
    std::istream& input;std::ostream& output;std::ostream& log;
    std::mutex& writer;
    bool completion_view=false, silent=false, trace_checks=false;
    bool compact_completions=false;
    struct Document{std::string uri,text;std::int64_t version=0;};
    std::map<std::string,Document> documents;
    std::uint64_t next_draft_id=0;
    std::vector<std::string> roots;
    std::map<std::string,Inspection> inspections;
    std::map<std::string,ParseCache> syntax_by_root;
    std::map<std::string,Index> indexes;
    std::map<std::string,std::unique_ptr<Program>> declaration_programs;
    struct RecoveredFile {
        std::string source,configuration;
        std::optional<Program> syntax;
    };
    std::map<std::string,std::map<std::string,RecoveredFile>> recovered_by_root;
    std::set<std::string> published;
    struct LocationFile {
        std::string text,uri;
        SourceLines lines;
        LocationFile(std::string source,std::string uri_):text(std::move(source)),uri(std::move(uri_)),lines(text){}
    };
    mutable std::map<std::string,LocationFile> location_files;
    mutable std::map<std::string,std::string> query_roots;
    bool read_query=false;
    bool initialized=false,shutdown=false,custom_tokens=false,versioned_workspace_edits=false;
    PreprocessorOptions session_preprocessing;
    const Program* completion_program(const std::string& file)const {
        const auto root=root_for(file);
        if(auto i=declaration_programs.find(root);i!=declaration_programs.end())return i->second.get();
        if(auto i=inspections.find(root);i!=inspections.end())return i->second.program.get();
        return nullptr;
    }
    void ensure_index(const std::string& root) {
        if(indexes.contains(root))return;
        const Program* program=nullptr;
        if(auto recovery=declaration_programs.find(root);recovery!=declaration_programs.end()){
            ParseCacheScope cache(syntax_by_root[root]);
            cache.retain_unvisited();
            bind_navigation(*recovery->second);
            program=recovery->second.get();
        }else if(auto it=inspections.find(root);it!=inspections.end())program=it->second.program.get();
        if(program){
            Index index;
            index.build(*program);
            indexes.emplace(root,std::move(index));
        }
    }
    void send(J message){if(silent)return;auto body=message.dump();std::lock_guard lock(writer);output<<"Content-Length: "<<body.size()<<"\r\n\r\n";output.write(body.data(),static_cast<std::streamsize>(body.size()));output.flush();}
    void notify(const std::string& method,J params){
        O message{{"jsonrpc","2.0"},{"method",method}};
        message.emplace("params",std::move(params));
        send(std::move(message));
    }
    void result(J id,J value){
        O message{{"jsonrpc","2.0"},{"id",std::move(id)}};
        message.emplace("result",std::move(value));
        send(std::move(message));
    }
    void error(J id,int code,std::string text){send(O{{"jsonrpc","2.0"},{"id",std::move(id)},{"error",O{{"code",code},{"message",std::move(text)}}}});}
    std::string document_path(const std::string& uri,bool opening){
        if(uri.starts_with("bolt-stdlib:///"))return "<bolt-stdlib>/"+uri.substr(15);
        if(uri.starts_with("bolt-stdlib:/")&&!uri.starts_with("bolt-stdlib://"))return "<bolt-stdlib>/"+uri.substr(13);
        if(!uri.starts_with("untitled:"))return uri_path(uri);
        for(const auto& [file,doc]:documents)if(doc.uri==uri)return file;
        if(!opening)throw std::invalid_argument("Untitled document is not open");
        // Only the source overlay uses this key. Each unnamed buffer is a separate script.
        const auto base=fs::temp_directory_path()/"bolt-lsp-unsaved";
        fs::path parent;
        do{parent=base/std::to_string(++next_draft_id);}while(fs::exists(parent));
        return canonical_path(parent/"Script.bolt");
    }
    std::string document_uri(const std::string& file)const{
        if(auto open=documents.find(file);open!=documents.end())return open->second.uri;
        if(file.starts_with("<bolt-stdlib>/"))return "bolt-stdlib:///"+file.substr(14);
        return path_uri(file);
    }
    std::string root_for(const std::string& file)const{
        if(!read_query)return discover_root(file);
        auto [it,inserted]=query_roots.try_emplace(file);
        if(inserted)it->second=discover_root(file);
        return it->second;
    }
    std::string discover_root(const std::string& file)const{
        if(auto open=documents.find(file);open!=documents.end()&&open->second.uri.starts_with("untitled:"))return file;
        if(file.starts_with("<bolt-stdlib>/")){
            for(const auto& [root,inspection]:inspections)
                if(inspection.program&&inspection.program->sources.contains(file))return root;
            for(const auto& [root,program]:declaration_programs)
                if(program->sources.contains(file))return root;
            return {};
        }
        auto path=path_from_utf8(file);std::string best;
        // A marker isolates nested sample projects when a larger repository is open.
        for(auto dir=path.parent_path();!dir.empty();){if(fs::exists(dir/"bolt.proj")||fs::is_regular_file(dir/"bolt.project"))return canonical_path(dir);auto parent=dir.parent_path();if(parent==dir)break;dir=parent;}
        for(const auto& root:roots){auto rel=path.lexically_relative(path_from_utf8(root));if(!rel.empty()&&*rel.begin()!=".."&&root.size()>best.size())best=root;}
        return best.empty()?canonical_path(path.parent_path()):best;
    }
    std::string text_for(const std::string& file)const{
        if(auto d=documents.find(file);d!=documents.end())return d->second.text;
        for(const auto& [r,i]:inspections){(void)r;if(auto s=i.sources.find(file);s!=i.sources.end())return s->second;if(i.program)if(auto s=i.program->sources.find(file);s!=i.program->sources.end())return s->second;}
        if(file.starts_with("<bolt-stdlib>/")){
            for(const auto& [root,p]:declaration_programs)if(auto s=p->sources.find(file);s!=p->sources.end())return s->second;
            return {};
        }
        std::ifstream in(path_from_utf8(file),std::ios::binary);if(!in)return {};return {std::istreambuf_iterator<char>(in),{}};
    }
    PreprocessedSource active_source(const std::string& file,const std::string& source)const {
        PreprocessorOptions options;
        if(auto i=inspections.find(root_for(file));i!=inspections.end()) {
            options=i->second.preprocessor;
            if(auto own=i->second.source_preprocessors.find(file);own!=i->second.source_preprocessors.end())options=own->second;
        }
        try{return preprocess(source,file,options);}catch(const Error&){
            // Malformed directives keep compiler diagnostics. Do not throw out
            // the independent lexer layer while the user edits a condition.
            PreprocessedSource result;result.text=source;
            for(std::size_t i=0;i<result.text.size();){auto end=result.text.find('\n',i);if(end==std::string::npos)end=result.text.size();auto n=i;while(n<end&&(result.text[n]==' '||result.text[n]=='\t'))++n;if(n<end&&result.text[n]=='#')for(auto k=n;k<end;++k)if(result.text[k]!='\r')result.text[k]=' ';i=end<result.text.size()?end+1:end;}
            return result;
        }
    }
    const LocationFile& location_file(const std::string& file)const{
        auto it=location_files.find(file);
        if(it==location_files.end())
            it=location_files.try_emplace(file,text_for(file),document_uri(file)).first;
        return it->second;
    }
    J location(const Location& at)const{
        const auto& source=location_file(at.file);
        return O{{"uri",source.uri},{"range",source.lines.range(source.text,at)}};
    }
    J diagnostic(const Diagnostic& d)const{
        auto text=text_for(d.location.file);J j=O{{"range",range_json(text,d.location)},{"severity",d.severity==Severity::Warning?2:d.severity==Severity::Information?3:1},{"code",d.code},{"source","Bolt"},{"message",d.message+(d.hint.empty()?"":"\nHelp: "+d.hint)}};
        A related;for(const auto& note:d.notes)related.emplace_back(O{{"location",location(note.location)},{"message",note.message}});if(!related.empty())j["relatedInformation"]=related;
        return j;
    }
    template<class Emit> void colors(const std::string& file,Emit emit)const {
        auto source=text_for(file);
        auto processed=active_source(file,source);
        auto tokens=lex(processed.text);
        for(const auto& at:processed.directives)if(at.length>0)tokens.push_back({at.offset,static_cast<std::size_t>(at.length),source.substr(at.offset,at.length),"macro"});
        for(const auto& at:processed.inactive)if(at.length>0)tokens.push_back({at.offset,static_cast<std::size_t>(at.length),source.substr(at.offset,at.length),"comment",true});
        std::sort(tokens.begin(),tokens.end(),[](const auto& a,const auto& b){return a.start<b.start;});
        std::map<std::pair<int,int>,std::string> semantic;
        std::set<std::pair<int,int>> resolved_symbols;
        auto root=root_for(file);
        if(auto index=indexes.find(root);index!=indexes.end())
            for(const auto& occurrence:index->second.occurrences)
                if(occurrence.at.file==file){
                    semantic[{occurrence.at.line,occurrence.at.column}]=occurrence.category;
                    if(!occurrence.symbol.empty()&&index->second.symbols.contains(occurrence.symbol))
                        resolved_symbols.emplace(occurrence.at.line,occurrence.at.column);
                }
        // Builtin binding can precede validation of the member name. Unknown
        // names must explicitly override a syntactic method-name classification.
        if(auto inspection=inspections.find(root);inspection!=inspections.end())
            for(const auto& d:inspection->second.diagnostics)if(d.location.file==file&&
                !resolved_symbols.contains({d.location.line,d.location.column})&&
                (d.message.find("unknown")!=std::string::npos||d.message.find("has no member")!=std::string::npos))
                semantic[{d.location.line,d.location.column}]="identifier";
        // Re-resolve missing bindings from the CURRENT declaration model. No
        // stale-token cache: deleting/renaming a type cannot leave old references.
        // Parsing a broken statement may stop compilation, but does not turn
        // every otherwise known method name in the file into "identifier".
        const auto inspection=inspections.find(root);
        const bool incomplete=inspection==inspections.end()||!inspection->second.diagnostics.empty();
        std::map<std::size_t,std::string> recovered_kinds;
        if(incomplete) {
            EditorTokens scan(processed.text);
            const auto* p=completion_program(file);
            std::unique_ptr<EditorContext> context;
            std::size_t contexts=0;bool boundary=true;
            std::optional<std::size_t> context_point;
            for(std::size_t n=0;n<scan.tokens.size();++n) {
                const auto& token=scan.tokens[n];
                if(token.text==";"||token.text=="{"||token.text=="}"){boundary=true;continue;}
                if(!EditorContext::is_name(token)||!token.kind.empty())continue;
                // Bounded fallback; lexical classifications remain available even
                // for pathological multi-megabyte unfinished method bodies.
                if(boundary&&contexts<2048){context_point=token.start+token.length;++contexts;boundary=false;}
                const auto token_at=scan.location(file,n);
                if(auto known=semantic.find({token_at.line,token_at.column});known!=semantic.end()&&!known->second.empty())continue;
                if(context_point){
                    context=std::make_unique<EditorContext>(p,scan,file,*context_point);
                    context_point.reset();
                }
                if(!context)continue;
                for(const auto& item:context->candidates(n,token.text))if(item.name==token.text) {
                    recovered_kinds[token.start]=item.category;break;
                }
            }
        }
        // An unresolved receiver poisons only its own member chain, not other
        // calls with the same spelling elsewhere in the method.
        if(inspection!=inspections.end())for(const auto& d:inspection->second.diagnostics)
            if(d.location.file==file&&!resolved_symbols.contains({d.location.line,d.location.column})&&
                (d.message.find("unknown")!=std::string::npos||d.message.find("has no member")!=std::string::npos)){
                auto i=std::find_if(tokens.begin(),tokens.end(),[&](const auto& token){return token.start==d.location.offset;});
                if(i==tokens.end())continue;
                auto n=static_cast<std::size_t>(i-tokens.begin())+1;
                while(n+1<tokens.size()&&(tokens[n].text=="."||tokens[n].text=="->")){
                    if(!tokens[n+1].kind.empty())break;
                    recovered_kinds[tokens[n+1].start]="identifier";n+=2;
                }
            }
        // Walk UTF-8 once. Re-scanning the whole prefix for every token would make
        // coloring quadratic for large files, even though lexing is linear.
        std::size_t cursor=0,line_start=0;
        Pos current;
        auto advance=[&](std::size_t target) {
            while(cursor<target) {
                auto c=static_cast<unsigned char>(source[cursor]);
                if(c=='\n') { ++cursor;line_start=cursor;++current.line;current.character=0;continue; }
                std::size_t width=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:(c&0xf8)==0xf0?4:1;
                width=std::min(width,target-cursor);
                current.character+=width==4?2:1;
                cursor+=width;
            }
        };
        for(std::size_t token_index=0;token_index<tokens.size();++token_index) {
            const auto& token=tokens[token_index];
            advance(token.start);
            auto kind=token.kind;
            if(kind.empty()) {
                const auto key=std::make_pair(current.line+1,static_cast<int>(cursor-line_start)+1);
                if(auto it=semantic.find(key);it!=semantic.end())kind=it->second;
            }
            const auto end=token.start+token.length;
            if(kind.empty())if(auto r=recovered_kinds.find(token.start);r!=recovered_kinds.end())kind=r->second;
            if(kind.empty()){
                // Unknown bindings in a partially analyzed file must not erase
                // the independent lexical/structural colors from the IDE.
                if(incomplete){kind="identifier";}
                auto next=token_index+1;while(next<tokens.size()&&tokens[next].kind=="comment")++next;
                if(next<tokens.size()&&tokens[next].text=="<"){
                    int depth=0;const auto limit=std::min(tokens.size(),next+128);
                    do{const auto& t=tokens[next++].text;if(t=="<")++depth;else if(t==">")--depth;else if(t==";"||t=="{"||t=="}")break;}while(next<limit&&depth);
                    if(depth)next=tokens.size();
                }
                // Unresolved calls must override an editor's syntactic method heuristic.
                // Other unbound names keep the fast lexical fallback colors
                // while the compiler is stopped at an earlier edit/error.
                if(kind.empty()&&(next==tokens.size()||tokens[next].text!="(")){advance(end);continue;}
                kind="identifier";
            }
            while(cursor<end) {
                const auto begin=current;
                auto newline=source.find('\n',cursor);
                auto stop=std::min(end,newline==source.npos?end:newline);
                if(stop>cursor&&source[stop-1]=='\r')--stop;
                advance(stop);
                if(current.character>begin.character)emit(begin,current.character-begin.character,kind,token.inactive);
                if(newline==source.npos||newline>=end) { advance(end);break; }
                advance(newline+1);
            }
        }
    }
    static std::optional<Program> parse_declarations(const std::string& name,const std::string& source,const PreprocessorOptions& options){
        std::string filtered;try{filtered=preprocess(source,name,options).text;}catch(const Error&){filtered=source;}
        auto recovered=declaration_source(filtered);
        // An incomplete import ("import System.") is an ordinary edit
        // state. Preserve byte positions while recovering declarations.
        auto ts=lex(recovered);
        for(const auto& token:ts)if(token.text=="import"&&token.kind=="keyword") {
            auto end=recovered.find('\n',token.start);
            if(end==std::string::npos)end=recovered.size();
            auto begin=token.start+6;while(begin<end&&std::isspace(static_cast<unsigned char>(recovered[begin])))++begin;
            auto last=end;while(last>begin&&(std::isspace(static_cast<unsigned char>(recovered[last-1]))||recovered[last-1]==';'))--last;
            if(begin==last||(last>begin&&recovered[last-1]=='.'))
                for(auto n=token.start;n<end;++n)if(recovered[n]!='\r')recovered[n]=' ';
        }
        try {
            auto part=parse(recovered,name,options);
            part.active_sources[name]=std::move(recovered);
            part.source_preprocessors[name]=options;
            return part;
        } catch(const std::exception&) { return {}; }
    }
    static void append_declarations(Program& declarations,const std::string& name,const std::string& source,
                                    const PreprocessorOptions& options,RecoveredFile* cached=nullptr){
        std::optional<Program> part;
        if(cached){
            const auto config=preprocessor_identity(options);
            if(cached->configuration!=config||cached->source!=source)
                *cached={source,config,parse_declarations(name,source,options)};
            // Navigation binds the assembled recovery model. Keep its mutations
            // away from the per-file syntax that the next edit will reuse.
            if(cached->syntax)part=copy_syntax_tree(*cached->syntax);
        }else part=parse_declarations(name,source,options);
        if(!part)return;
        for(auto& type:part->source_types)declarations.source_types.push_back(std::move(type));
        for(auto& c:part->classes)declarations.classes.push_back(std::move(c));
        for(auto& e:part->enums)declarations.enums.push_back(std::move(e));
        for(auto& c:part->concepts)declarations.concepts.push_back(std::move(c));
        for(auto& i:part->imports)declarations.imports.push_back(std::move(i));
        declarations.sources.merge(part->sources);
        declarations.active_sources.merge(part->active_sources);
        declarations.source_preprocessors.merge(part->source_preprocessors);
    }
    static void remove_declarations(Program& p,const std::string& file){
        auto from_file=[&](const auto& entry){return entry.token.at.file==file;};
        std::erase_if(p.classes,from_file);
        std::erase_if(p.enums,from_file);
        std::erase_if(p.imports,from_file);
        std::erase_if(p.concepts,from_file);
        std::erase_if(p.source_types,from_file);
        p.sources.erase(file);
        p.active_sources.erase(file);
        p.source_preprocessors.erase(file);
    }
    void refresh_completion_root(const std::string& root,const Inspection& current){
        auto previous=inspections.find(root);
        auto& model=declaration_programs[root];
        const bool rebuild=!model||previous==inspections.end()||
            preprocessor_identity(previous->second.preprocessor)!=preprocessor_identity(current.preprocessor);
        if(rebuild){
            model=std::make_unique<Program>();
            model->preprocessor=current.preprocessor;
        }else{
            for(const auto& [file,source]:previous->second.sources)
                if(!current.sources.contains(file))remove_declarations(*model,file);
        }
        for(const auto& [file,source]:current.sources){
            const auto& options=current.source_preprocessors.at(file);
            if(!rebuild){
                const auto& old=previous->second;
                auto prior=old.sources.find(file);
                if(prior!=old.sources.end()&&prior->second==source&&
                   preprocessor_identity(old.source_preprocessors.at(file))==preprocessor_identity(options))continue;
                remove_declarations(*model,file);
            }
            append_declarations(*model,file,source,options);
        }
        if(rebuild)add_standard_library(*model);
    }
    // The completion view owns its syntax separately from semantic analysis.
    // Only the changed file needs new declarations, even across project references.
    void update_completion_source(const std::string& file) {
        for(auto& [root,inspection]:inspections){
            auto previous=inspection.sources.find(file);
            if(previous==inspection.sources.end())continue;
            const auto& source=documents.at(file).text;
            if(previous->second==source)continue;
            previous->second=source;
            auto& p=*declaration_programs.at(root);
            remove_declarations(p,file);
            append_declarations(p,file,source,inspection.source_preprocessors.at(file));
        }
    }
    void analyze_all(const std::string& edited={}){
        if(completion_view && !edited.empty()){update_completion_source(edited);return;}
        if(trace_checks&&!completion_view)notify("$/logTrace",O{{"message","Checking Bolt project"}});
        std::set<std::string> active;SourceOverrides overlays;for(const auto& [file,d]:documents){overlays[file]=d.text;active.insert(root_for(file));}
        const auto inactive=[&](const auto& item){return !active.contains(item.first);};
        std::erase_if(inspections,inactive);std::erase_if(indexes,inactive);
        std::erase_if(declaration_programs,inactive);std::erase_if(syntax_by_root,inactive);
        std::erase_if(recovered_by_root,inactive);
        std::map<std::string,A> diags;
        for(const auto& file:published)diags[file]={};
        for(const auto& [file,d]:documents){(void)d;diags[file]={};}
        const auto edited_root=edited.empty()?std::string():root_for(edited);
        for(const auto& root:active){
            auto prior=inspections.find(root);
            if(!edited.empty() && root!=edited_root && prior!=inspections.end() &&
               prior->second.diagnostics.empty() && !prior->second.sources.contains(edited))continue;
            indexes.erase(root);
            if(!completion_view)declaration_programs.erase(root);
            Inspection inspection;
            try{
                const auto inputs=project_inputs(path_from_utf8(root),overlays);
                const auto& files=inputs.files;
                if(files.size()>4096)throw std::runtime_error("Workspace has more than 4096 .bolt files; open a smaller project folder");
                std::uintmax_t total=0;for(const auto& f:files){auto key=canonical_path(f);auto ov=overlays.find(key);auto bytes=ov==overlays.end()?fs::file_size(f):ov->second.size();if(bytes>max_document)throw std::runtime_error("Source file exceeds the 4 MiB language-server limit: "+key);total+=bytes;if(total>max_workspace)throw std::runtime_error("Workspace exceeds the 32 MiB language-server source limit");}
                if(completion_view){
                    read_inspection_sources(inspection,inputs,path_from_utf8(root),overlays,session_preprocessing);
                }else{
                    ParseCacheScope cache(syntax_by_root[root]);
                    inspection=inspect_project(inputs,path_from_utf8(root),overlays,false,session_preprocessing);
                    // Even a failed inspection can parse many unchanged files.
                    // Each syntax entry is keyed by source bytes and preprocessing.
                    // A syntax error can stop the pass before it visits the library.
                    if(!inspection.program)syntax_by_root[root].reuse_standard_library();
                }
            }catch(const Error& e){inspection.diagnostics.push_back(e.diagnostic);}
            catch(const std::exception& e){auto d=tooling_diagnostic(e.what());for(const auto& [file,doc]:documents){(void)doc;if(root_for(file)==root){d.location={file,1,1};break;}}inspection.diagnostics.push_back(std::move(d));}
            if(completion_view)refresh_completion_root(root,inspection);
            else if(!inspection.program&&!inspection.sources.empty()){
                // A fresh declaration model survives unfinished expressions;
                // the original source and compiler diagnostics remain intact.
                // Recover each file separately. One unfinished declaration/import
                // must not discard declarations from every other project file.
                auto declarations=std::make_unique<Program>();
                declarations->preprocessor=inspection.preprocessor;
                auto& recovery=recovered_by_root[root];
                std::erase_if(recovery,[&](const auto& item){return !inspection.sources.contains(item.first);});
                for(const auto& [name,source]:inspection.sources) {
                    auto options=inspection.preprocessor;
                    if(auto own=inspection.source_preprocessors.find(name);own!=inspection.source_preprocessors.end())options=own->second;
                    append_declarations(*declarations,name,source,options,&recovery[name]);
                }
                // Only the embedded sources share cache entries with a regular
                // compilation. The masked project buffers above must stay separate.
                ParseCacheScope cache(syntax_by_root[root]);
                cache.retain_unvisited();
                add_standard_library(*declarations);
                declaration_programs[root]=std::move(declarations);
            }
            inspections.insert_or_assign(root,std::move(inspection));
        }
        if(completion_view)return;
        for(const auto& [root,i]:inspections){(void)root;for(const auto& d:i.diagnostics){auto file=d.location.file;if(file.empty()||file[0]=='<')continue;diags[file].push_back(diagnostic(d));}}
        published.clear();
        for(auto& [file,values]:diags){J p=O{{"uri",document_uri(file)},{"diagnostics",values}};if(auto it=documents.find(file);it!=documents.end())p["version"]=it->second.version;if(!values.empty())published.insert(file);notify("textDocument/publishDiagnostics",p);}
        if(custom_tokens)for(const auto& [file,d]:documents){
            ensure_index(root_for(file));
            A spans;
            colors(file,[&](Pos begin,int length,const std::string& kind,bool inactive){
                O span{{"line",begin.line},{"start",begin.character},{"length",length},{"kind",kind}};
                if(inactive)span.emplace("inactive",true);
                spans.emplace_back(std::move(span));
            });
            notify("bolt/semanticTokens",O{{"uri",d.uri},{"version",d.version},{"textHash",hash_text(d.text)},{"tokens",std::move(spans)}});
        }
    }
    const Occurrence* at_position(const std::string& file,const J& pos)const{
        auto source=text_for(file);auto byte=offset(source,pos);auto last=byte?source.rfind('\n',byte-1):source.npos;int column=static_cast<int>(byte-(last==source.npos?0:last+1))+1;
        auto i=indexes.find(root_for(file));if(i==indexes.end())return nullptr;
        const auto line=static_cast<int>(pos["line"].integer())+1;
        const Occurrence* fallback=nullptr;
        for(auto it=i->second.line_begin(file,line);it!=i->second.occurrences.end();++it) {
            const auto& occurrence=*it;
            const auto& at=occurrence.at;
            if(at.file!=file||at.line!=line||at.column>column)break;
            if(column>=at.column+std::max(1,at.length))continue;
            auto symbol=i->second.symbols.find(occurrence.symbol);
            if(symbol!=i->second.symbols.end()) {
                if(symbol->second.name.starts_with("@")||symbol->second.owner.starts_with("Bolt.Generated."))continue;
                return &occurrence;
            }
            if(!fallback)fallback=&occurrence;
        }
        return fallback;
    }
    std::optional<J> import_completion(const std::string& file,const std::string& source,std::size_t caret)const {
        auto line=caret?source.rfind('\n',caret-1):std::string::npos;
        auto begin=line==std::string::npos?0:line+1;
        while(begin<caret&&(source[begin]==' '||source[begin]=='\t'))++begin;
        if(source.compare(begin,6,"import")!=0||begin+6>=caret||!std::isspace(static_cast<unsigned char>(source[begin+6])))return {};
        // Do not treat text inside a multiline comment or string as an import.
        bool is_import=false;for(const auto& t:lex(source))if(t.start==begin&&t.text=="import"&&t.kind=="keyword"){is_import=true;break;}
        if(!is_import)return {};
        auto start=begin+6;while(start<caret&&(source[start]==' '||source[start]=='\t'))++start;
        const auto prefix=source.substr(start,caret-start);
        for(char c:prefix)if(!alpha(c)&&!digit(c)&&c!='.')return {};
        std::set<std::string> available,already;
        std::map<std::string,EditorItem> types;
        auto namespace_name=[&](std::string name){
            while(!name.empty()){available.insert(name);auto dot=name.rfind('.');if(dot==std::string::npos)break;name.resize(dot);}
        };
        for(auto name:builtin_import_names())namespace_name(std::string(name));
        // These types have compiler-provided operations even without a Bolt declaration.
        for(const std::string name:{"Arena","Math","Runtime","Type"})
            types.emplace(name,EditorItem{name,name,"built-in type "+name,"type",7});
        if(const auto* p=completion_program(file)) {
            auto standard_alias=[](const auto& declaration){
                return declaration.declaring_type.empty() && declaration.token.at.file.starts_with("<bolt-stdlib>/")
                    && is_builtin_import(declaration.short_name);
            };
            for(const auto& c:p->classes){
                namespace_name(c.namespace_name);
                if(!c.is_global && c.name.find('<')==std::string::npos && standard_alias(c))
                    types.insert_or_assign(c.short_name,EditorContext::class_item(c));
            }
            for(const auto& e:p->enums){
                namespace_name(e.namespace_name);
                if(standard_alias(e))types.insert_or_assign(e.short_name,EditorContext::enum_item(e));
            }
            for(const auto& c:p->concepts)namespace_name(c.namespace_name);
            for(const auto& i:p->imports)if(i.token.at.file==file&&i.token.at.offset!=begin&&!i.is_path)already.insert(i.name);
        }
        // The actual workspace snapshot includes unsaved/new files. Namespace
        // declarations can still be read when another statement is malformed.
        if(auto it=inspections.find(root_for(file));it!=inspections.end())for(const auto& [name,text]:it->second.sources) {
            if(name.starts_with("<"))continue;
            auto stem=path_to_utf8(path_from_utf8(name).stem());
            if(!stem.empty()&&alpha(stem[0])&&std::all_of(stem.begin(),stem.end(),[](char c){return alpha(c)||digit(c);}))available.insert(stem);
            auto ts=lex(active_source(name,text).text);
            for(std::size_t i=0;i<ts.size();++i)if(ts[i].text=="namespace"&&ts[i].kind=="keyword") {
                std::string name_space;bool expect_name=true;
                for(++i;i<ts.size();++i){auto t=ts[i].text;if(t==";")break;
                    if(expect_name&&EditorContext::is_name(ts[i])){name_space+=t;expect_name=false;}
                    else if(!expect_name&&t=="."){name_space+='.';expect_name=true;}
                    else break;
                }
                if(!expect_name)namespace_name(name_space);
                break;
            }
        }
        A values;
        for(const auto& name:available)if(name.starts_with(prefix)&&!already.contains(name)) {
            auto type=types.find(name);
            const auto* item=type==types.end()?nullptr:&type->second;
            values.emplace_back(O{{"label",name},{"kind",item?item->kind:9},{"detail",item?item->detail:"module "+name},
                {"insertText",name},{"insertTextFormat",1},{"filterText",name},
                {"textEdit",O{{"range",O{{"start",pos_json(position(source,start))},{"end",pos_json(position(source,caret))}}},{"newText",name}}},
                {"data",O{{"bolt",O{{"type",item?item->type:"module"},{"category",item?item->category:"namespace"}}}}}});
        }
        return J(O{{"isIncomplete",false},{"items",values}});
    }
    std::optional<J> attribute_completion(const std::string& file,const std::string& source,const EditorTokens& scan,std::size_t caret)const {
        const auto start=attribute_prefix(source,scan,caret);if(!start)return {};
        const auto prefix=source.substr(*start,caret-*start);
        A items;std::map<std::string,std::string> names;
        for(auto n:builtin_attribute_names())names.emplace(std::string(n),"Built-in attribute");
        const auto* p=completion_program(file);
        if(p) {
            EditorContext context(p,scan,file,caret);
            std::string ns;std::set<std::string> imports;
            // Current lexical namespace/imports also work above the first class.
            const auto ts=lex(source);
            for(std::size_t i=0;i<ts.size();++i)if(ts[i].text=="namespace"||ts[i].text=="import") {
                const bool space=ts[i].text=="namespace";const auto line=position(source,ts[i].start).line;
                std::string n;for(++i;i<ts.size();++i){auto t=ts[i].text;
                    if(t==";"||position(source,ts[i].start).line!=line){if(t!=";")--i;break;}
                    if(EditorContext::is_name(ts[i])||t==".")n+=t;else break;
                }
                if(space)ns=n;else imports.insert(n);
            }
            auto resolve=[&](const Class& owner,const std::string& name)->const Class* {
                if(auto c=p->find_class(name))return c;
                if(auto c=p->find_class(owner.namespace_name.empty()?name:owner.namespace_name+"."+name))return c;
                const Class* found=nullptr;for(const auto& scope:owner.imports)if(auto c=p->find_class(scope+"."+name)){if(found&&found!=c)return nullptr;found=c;}return found;
            };
            std::function<bool(const Class*,std::set<std::string>&)> is_attribute=[&](const Class* c,std::set<std::string>& seen){
                if(!c||!seen.insert(c->name).second)return false;
                if(c->name=="System.Attribute")return true;
                if(!c->base.empty()&&is_attribute(resolve(*c,c->base),seen))return true;
                for(const auto& parent:c->interfaces)if(is_attribute(resolve(*c,parent),seen))return true;
                return false;
            };
            // Use qualified names for out-of-scope or ambiguous attributes. No
            // automatic import mutation and no ordinary classes in this list.
            std::map<std::string,std::vector<const Class*>> short_names;
            std::vector<const Class*> attrs;
            for(const auto& c:p->classes) {
                if(c.name=="System.Attribute"||c.is_static||c.is_interface||!c.generic_parameters.empty()||!c.pack_parameter.empty())continue;
                if(!context.accessible(c.name))continue;
                std::set<std::string> seen;if(!is_attribute(&c,seen))continue;
                attrs.push_back(&c);
                if(c.declaring_type.empty()&&(c.namespace_name.empty()||c.namespace_name==ns||imports.contains(c.namespace_name))){
                    auto short_name=c.short_name;if(short_name.ends_with("Attribute"))short_name.resize(short_name.size()-9);
                    short_names[short_name].push_back(&c);
                }
            }
            for(const auto* c:attrs){
                auto qualified=c->name;if(qualified.ends_with("Attribute"))qualified.resize(qualified.size()-9);
                auto short_name=c->short_name;if(short_name.ends_with("Attribute"))short_name.resize(short_name.size()-9);
                const bool visible=short_names.contains(short_name)&&short_names.at(short_name).size()==1;
                const auto name=visible&&prefix.find('.')==std::string::npos?short_name:qualified;
                if(!name.empty())names.emplace(name,"Attribute "+c->name);
            }
        }
        for(const auto& [name,detail]:names)if(name.starts_with(prefix)) {
            std::string params;
            if(name=="Header"||name=="Native"||name=="NativeProperty"||name=="JsonName")params="string value";
            A parameters;if(!params.empty())parameters.emplace_back(params);
            items.emplace_back(O{{"label",name},{"kind",7},{"detail",detail},{"insertText",name},{"insertTextFormat",1},
                {"textEdit",O{{"range",O{{"start",pos_json(position(source,*start))},{"end",pos_json(position(source,caret))}}},{"newText",name}}},
                {"data",O{{"bolt",O{{"type","attribute"},{"category","attribute"},{"parameters",parameters}}}}}});
        }
        return J(O{{"isIncomplete",false},{"items",items}});
    }
    J completion(const std::string& file,const J& pos,const J& pending=nullptr)const{
        auto source=text_for(file);
        auto insertion=std::string::npos;
        if(!pending.is_null()){
            auto doc=documents.find(file);
            if(doc==documents.end()||!pending["version"].is_integer()||
               pending["version"].integer()!=doc->second.version)return nullptr;
            const auto text=pending["text"].string();
            const auto expression_char=[](char c){return alpha(c)||digit(c)||c=='.';};
            if(text.empty()||text.size()>256||!std::all_of(text.begin(),text.end(),expression_char))return nullptr;
            insertion=offset(source,pending["start"]);
            const auto end=pending.contains("end")?offset(source,pending["end"]):insertion;
            if(end<insertion||end-insertion>256||source.size()-(end-insertion)+text.size()>max_document||
               !std::all_of(source.begin()+insertion,source.begin()+end,expression_char))return nullptr;
            // Splitting a comment delimiter could change declarations later in
            // the file even though the inserted characters form an identifier.
            if(insertion&&insertion<source.size()){
                const auto boundary=std::string_view(source).substr(insertion-1,2);
                if(boundary=="/*"||boundary=="*/"||boundary=="//"||boundary=="$\"")return nullptr;
            }
            const auto line=insertion?source.rfind('\n',insertion-1):source.npos;
            const auto first=source.find_first_not_of(" \t",line==source.npos?0:line+1);
            if(first!=source.npos&&source[first]=='#')return nullptr;
            source.replace(insertion,end-insertion,text);
            if(offset(source,pos)!=insertion+text.size())return nullptr;
        }
        const auto caret=offset(source,pos);
        const auto active=active_source(file,source);
        for(const auto& span:active.inactive)if(caret>=span.offset&&caret<=span.offset+static_cast<std::size_t>(span.length))return O{{"isIncomplete",false},{"items",A{}}};
        if(insertion==std::string::npos)if(auto imports=import_completion(file,source,caret))return *imports;
        EditorTokens scan(active.text);
        if(insertion==std::string::npos) {
            if(auto attrs=attribute_completion(file,active.text,scan,caret))return *attrs;
        }
        EditorContext context(completion_program(file),scan,file,caret,insertion);
        if(insertion!=std::string::npos){
            const auto end=scan.at(caret);
            if(!context.method||!end)return nullptr;
            const auto& token=scan.tokens[end-1];
            if(token.start+token.length<caret||(!EditorContext::is_name(token)&&token.text!="."))return nullptr;
        }
        auto end=scan.at(caret);if(end&&scan.tokens[end-1].start+scan.tokens[end-1].length>=caret&&EditorContext::is_name(scan.tokens[end-1]))--end;
        A values;std::set<std::string> names;
        auto add=[&](const EditorItem& item) {
            // Method overloads stay distinct. Recovered inheritance must not duplicate
            // an overriding signature or shadow an innermost local with a field.
            auto identity=item.category=="method"?item.name+"|"+item.detail:item.name;
            if(!names.insert(identity).second)return;
            O entry{{"label",item.name},{"kind",item.kind},{"detail",item.detail}};
            // LSP uses the label for inserting and filtering by default. Clients
            // that consume Bolt's signature metadata keep the full representation.
            if(compact_completions){
                values.emplace_back(std::move(entry));
                return;
            }
            A parameters,type_parameters;
            for(const auto& parameter:item.parameters)parameters.emplace_back(parameter);
            for(const auto& parameter:item.type_parameters)type_parameters.emplace_back(parameter);
            O data{{"type",item.type},{"category",item.category},{"parameters",parameters},
                   {"typeParameters",type_parameters},{"static",item.is_static}};
            entry.emplace("insertText",item.name);
            entry.emplace("insertTextFormat",1);
            entry.emplace("filterText",item.name);
            O metadata;
            metadata.emplace("bolt",std::move(data));
            entry.emplace("data",std::move(metadata));
            values.emplace_back(std::move(entry));
        };
        for(const auto& item:context.candidates(end))add(item);
        const bool member=end&&(scan.tokens[end-1].text=="."||(end>=2&&scan.tokens[end-1].text==">"&&scan.tokens[end-2].text=="-"));
        if(!member){
            for(const auto& k:keywords)if(k!="this"||(context.method&&!context.method->is_static))
                add(EditorItem{k,"",k=="this"?"Current class instance (inside non-static methods and constructors)":k=="default"?"Default value of the destination type":"Bolt keyword","keyword",14,{}});
            for(const auto& k:primitives)add(EditorItem{k,k,"Bolt primitive type","type",7,{}});
        }
        // An earlier diagnostic may have stopped type resolution. Give the
        // current buffer's normal recovery pass a chance to resolve the receiver.
        if(insertion!=std::string::npos&&values.empty())return nullptr;
        O list{{"isIncomplete",false}};
        list.emplace("items",std::move(values));
        return list;
    }
    J signature_help(const std::string& file,const J& pos)const {
        const auto source=text_for(file);const auto caret=offset(source,pos);EditorTokens scan(active_source(file,source).text);
        EditorContext context(completion_program(file),scan,file,caret);
        auto end=scan.at(caret);int nested=0;std::size_t open=scan.tokens.size();
        for(auto i=end;i>0;){--i;const auto& t=scan.tokens[i].text;
            if(t==")"||t=="]"||t=="}")++nested;
            else if(t=="("||t=="["||t=="{"){if(nested)--nested;else if(t=="("){open=i;break;}else break;}
        }
        if(open==scan.tokens.size()||!open)return nullptr;
        auto name=open-1;std::vector<std::string> explicit_types;
        if(scan.tokens[name].text==">"){
            const auto last=name;int depth=1;while(name&&depth){--name;if(scan.tokens[name].text==">")++depth;else if(scan.tokens[name].text=="<")--depth;}
            if(!name||depth)return nullptr;std::string generic="F";for(auto i=name;i<=last;++i)generic+=scan.tokens[i].text;
            explicit_types=generic_args(generic);--name;
        }
        const auto& word=scan.tokens[name].text;A signatures;int active=0,best=-100000;std::vector<std::string> arguments;
        auto begin=open+1;int argument=0;
        for(auto i=begin;i<end;++i){
            const auto& token=scan.tokens[i].text;
            if(token==","){arguments.push_back(context.argument_type(begin,i,0));begin=i+1;++argument;}
            else if(scan.pair[i]>i&&scan.pair[i]<end)i=scan.pair[i];
            else if(token=="<"&&i&&EditorContext::is_name(scan.tokens[i-1])) {
                // Type arguments belong to a known generic, not an arbitrary a<b comparison.
                const auto reply=generic_context(file,pos_json(position(source,scan.tokens[i].start+1)));
                if(reply["generic"].boolean()){
                    int nesting=1;auto j=i+1;for(;j<end;++j){if(scan.tokens[j].text=="<")++nesting;else if(scan.tokens[j].text==">")--nesting;else if(scan.tokens[j].text==">>")nesting-=2;if(!nesting)break;}
                    if(nesting)return nullptr;i=j;
                }
            }
        }
        arguments.push_back(context.argument_type(begin,end,0));
        auto candidates=context.candidates(name);
        // Prefer the exact overload selected by the semantic analyzer whenever
        // the complete call is available; partial input still lists candidates.
        Location resolved_declaration;
        if(const auto* hit=at_position(file,pos_json(position(source,scan.tokens[name].start)))) {
            if(hit->declaration)return nullptr; // parameter declarations are not calls
            if(auto index=indexes.find(root_for(file));index!=indexes.end())if(auto symbol=index->second.symbols.find(hit->symbol);symbol!=index->second.symbols.end())resolved_declaration=symbol->second.at;
        }
        for(const auto& item:context.candidates(name))if(item.name==word&&(item.category=="class"||item.category=="struct")) {
            if(const auto* c=context.find_class(item.type))for(const auto& m:c->methods)if(m.is_constructor&&!m.is_specialization&&(m.is_public||c==context.owner||(m.is_protected&&context.owner&&derives_from(*context.program,context.owner->name,c->name)))) {
                auto constructor=EditorContext::method_item(m);constructor.name=word;constructor.kind=2;constructor.type_parameters=c->generic_parameters;constructor.type_pack=c->pack_parameter;candidates.push_back(std::move(constructor));
            }
        }
        int active_parameter=argument;
        for(auto item:candidates)if(item.name==word&&item.kind==2){
            auto mapping=item.receiver_types;int score=0;bool complete=true,compatible=true;
            for(std::size_t i=0;i<std::min(explicit_types.size(),item.type_parameters.size());++i) {
                const auto type=item.is_extension&&context.owner?context.extension_type(explicit_types[i],*context.owner):explicit_types[i];
                auto [it,inserted]=mapping.emplace(item.type_parameters[i],type);
                if(!inserted&&it->second!=type){score-=10000;compatible=false;}
            }
            for(std::size_t i=0;i<arguments.size();++i){
                if(arguments[i].empty()){complete=false;continue;}
                if(item.parameters.empty()||(!item.value_pack&&i>=item.parameters.size())){score-=100;compatible=false;continue;}
                const auto pattern=EditorContext::argument_parameter_type(item.parameters[std::min(i,item.parameters.size()-1)]);
                if(!EditorContext::infer(pattern,arguments[i],item.type_parameters,mapping)){score-=100;compatible=false;continue;}
                const auto wanted=bare_type(EditorContext::substitute(pattern,mapping)),actual=bare_type(arguments[i]);
                if(wanted==actual)score+=10;else if(is_numeric(wanted)&&is_numeric(actual))score+=1;else {score-=20;compatible=false;}
            }
            if(!explicit_types.empty()&&explicit_types.size()!=item.type_parameters.size()&&item.type_pack.empty())score-=100;
            if(complete&&arguments.size()<item.required_arguments)score-=5;
            if(compatible&&!item.is_extension)score+=1000;
            if(!resolved_declaration.file.empty()&&item.declaration.file==resolved_declaration.file&&item.declaration.line==resolved_declaration.line&&item.declaration.column==resolved_declaration.column)score+=100000;
            A parameters;std::string detail=EditorContext::substitute(item.detail,mapping);
            for(const auto& parameter:item.parameters)parameters.emplace_back(O{{"label",EditorContext::substitute(parameter,mapping)}});
            // All expanded arguments belong to the declared final pack slot.
            // LSP parameter indices refer to the displayed signature, not the
            // flattened runtime argument list.
            const auto parameter=item.value_pack&&!parameters.empty()?std::min(argument,static_cast<int>(parameters.size())-1):argument;
            if(score>best){best=score;active=static_cast<int>(signatures.size());active_parameter=parameter;}
            signatures.emplace_back(O{{"label",detail},{"parameters",parameters},{"activeParameter",parameter}});
        }
        if(signatures.empty())return nullptr;
        return O{{"signatures",signatures},{"activeSignature",active},{"activeParameter",active_parameter},{"boltCallStart",pos_json(position(source,scan.tokens[open].start))}};
    }

#include "EditorFeatures15.hpp"
#include "EditorRecovery.hpp"

    void handle(const J& message){
        location_files.clear();
        query_roots.clear();
        read_query=message.contains("id")&&message["method"].string().starts_with("textDocument/");
        const bool request=message.contains("id");const auto id=message["id"];auto method=message["method"].string();const auto& params=message["params"];
        if(request&&!id.is_integer()&&!id.is_string()&&!id.is_null()){error(nullptr,-32600,"Invalid JSON-RPC id");return;}
        try{
            if(method=="initialize"){
                if(initialized){error(id,-32600,"Already initialized");return;}
                versioned_workspace_edits=params["capabilities"]["workspace"]["workspaceEdit"]["documentChanges"].boolean();
                custom_tokens=params["initializationOptions"]["boltSemanticNotifications"].boolean();
                compact_completions=params["initializationOptions"]["boltCompactCompletions"].boolean();
                auto profile=params["initializationOptions"]["boltProfile"].string();
                if(!profile.empty()&&profile!="Debug"&&profile!="Release"){error(id,-32602,"boltProfile must be Debug or Release");return;}
                session_preprocessing.configuration=profile;initialized=true;
                trace_checks=params["trace"].string("off")!="off";
                for(const auto& root:params["workspaceFolders"].array())if(root["uri"].is_string())roots.push_back(uri_path(root["uri"].string()));
                if(roots.empty()&&params["rootUri"].is_string())roots.push_back(uri_path(params["rootUri"].string()));
                if(roots.empty()&&params["rootPath"].is_string())roots.push_back(canonical_path(path_from_utf8(params["rootPath"].string())));
                A legend;for(const auto& name:token_types)legend.emplace_back(name);
                result(id,O{{"capabilities",O{{"experimental",O{{"boltPendingCompletion",true}}},{"positionEncoding","utf-16"},{"textDocumentSync",O{{"openClose",true},{"change",2},{"save",O{{"includeText",true}}}}},{"hoverProvider",true},{"codeActionProvider",O{{"codeActionKinds",A{"quickfix"}}}},{"definitionProvider",true},{"referencesProvider",true},{"renameProvider",O{{"prepareProvider",true}}},{"documentSymbolProvider",true},{"completionProvider",O{{"resolveProvider",false},{"triggerCharacters",A{".",">"," ","[",","}}}},{"signatureHelpProvider",O{{"triggerCharacters",A{"(",","}},{"retriggerCharacters",A{","}}}},{"semanticTokensProvider",O{{"legend",O{{"tokenTypes",legend},{"tokenModifiers",A{"boltInactive"}}}},{"full",true}}}}},{"serverInfo",O{{"name","Bolt Language Server"},{"version",BOLT_VERSION_STRING}}}});return;
            }
            if(!initialized){if(request)error(id,-32002,"Server not initialized");return;}
            if(method=="shutdown"){shutdown=true;result(id,nullptr);return;}
            if(shutdown){if(request)error(id,-32600,"Server is shutting down");return;}
            if(method=="$/setTrace"){trace_checks=params["value"].string("off")!="off";return;}
            if(method=="initialized"||method=="$/cancelRequest")return;
            if(method=="workspace/didChangeConfiguration"){
                auto profile=params["settings"]["bolt"]["profile"].string();
                if(profile.empty()||profile=="Debug"||profile=="Release")session_preprocessing.configuration=profile;
                else notify("window/logMessage",O{{"type",2},{"message","Bolt profile must be Debug or Release; keeping previous selection"}});
                analyze_all();return;
            }
            if(method=="workspace/didChangeWatchedFiles"){analyze_all();return;}
            if(method=="workspace/didChangeWorkspaceFolders"){
                for(const auto& r:params["event"]["removed"].array()){auto path=uri_path(r["uri"].string());roots.erase(std::remove(roots.begin(),roots.end(),path),roots.end());}
                for(const auto& r:params["event"]["added"].array())roots.push_back(uri_path(r["uri"].string()));
                analyze_all();return;
            }
            if(method=="bolt/virtualDocument") {
                const auto uri=params["uri"].string();
                if(!uri.starts_with("bolt-stdlib:///"))throw std::invalid_argument("Not an embedded Bolt source URI");
                const auto key=std::string("<bolt-stdlib>/")+uri.substr(15);
                const auto text=text_for(key);if(!text.empty())result(id,O{{"uri",uri},{"text",text},{"languageId","bolt"},{"readOnly",true}});else result(id,nullptr);return;
            }
            if(!method.starts_with("textDocument/")){if(request)error(id,-32601,"Method not found: "+method);return;}
            auto uri=params["textDocument"]["uri"].string();auto file=document_path(uri,method=="textDocument/didOpen");
            if(file.starts_with("<bolt-stdlib>/")){
                // The provider displays compiler-owned sources. Client edits cannot replace them.
                if(!request)return;
                if(method=="textDocument/prepareRename"){result(id,nullptr);return;}
                if(method=="textDocument/rename"){error(id,-32602,"Embedded Bolt sources are read-only");return;}
            }
            if(method=="textDocument/didOpen"){
                if(documents.size()>=256&&!documents.contains(file))throw std::invalid_argument("Too many open documents (maximum 256)");
                auto text=params["textDocument"]["text"].string();if(text.size()>max_document)throw std::invalid_argument("Document exceeds 4 MiB");
                documents[file]={uri,text,params["textDocument"]["version"].integer()};analyze_all();return;
            }
            if(method=="textDocument/didChange"){
                auto d=documents.find(file);if(d==documents.end())throw std::invalid_argument("Document is not open");
                auto version=params["textDocument"]["version"].integer();if(version<=d->second.version)return;
                auto text=d->second.text;
                for(const auto& change:params["contentChanges"].array()){
                    auto replacement=change["text"].string();if(change.contains("range")){auto begin=offset(text,change["range"]["start"]),end=offset(text,change["range"]["end"]);if(end<begin)throw std::invalid_argument("Reversed edit range");text.replace(begin,end-begin,replacement);}else text=std::move(replacement);
                    if(text.size()>max_document)throw std::invalid_argument("Document exceeds 4 MiB");
                }
                d->second.text=std::move(text);d->second.version=version;analyze_all(file);return;
            }
            if(method=="textDocument/didClose"){published.erase(file);documents.erase(file);notify("textDocument/publishDiagnostics",O{{"uri",uri},{"diagnostics",A{}}});analyze_all();return;}
            if(method=="textDocument/didSave"){if(params["text"].is_string()&&documents.contains(file)){auto text=params["text"].string();if(text.size()>max_document)throw std::invalid_argument("Document exceeds 4 MiB");documents[file].text=text;}analyze_all();return;}
            if(!request)return;
            if(method=="textDocument/boltPendingCompletion"){
                result(id,completion(file,params["position"],params["edit"]));return;
            }
            if(method=="textDocument/completion"){result(id,completion(file,params["position"]));return;}
            ensure_index(root_for(file));
            if(method=="textDocument/semanticTokens/full"){
                A data;Pos last;
                colors(file,[&](Pos begin,int length,const std::string& kind,bool inactive){
                    const auto it=std::find(token_types.begin(),token_types.end(),kind);
                    if(it==token_types.end())return;
                    data.emplace_back(begin.line-last.line);
                    data.emplace_back(begin.line==last.line?begin.character-last.character:begin.character);
                    data.emplace_back(length);
                    data.emplace_back(static_cast<int>(it-token_types.begin()));
                    data.emplace_back(inactive?1:0);
                    last=begin;
                });
                result(id,O{{"data",std::move(data)}});return;
            }
            if(method=="textDocument/codeAction"){result(id,import_actions(file,params));return;}
            if(method=="textDocument/signatureHelp"){result(id,signature_help(file,params["position"]));return;}
            if(method=="textDocument/boltSymbolIndex"){result(id,symbol_index(file));return;}
            if(method=="textDocument/boltGenericContext"){result(id,generic_context(file,params["position"]));return;}
            if(method=="textDocument/prepareRename"){result(id,prepare_rename(file,params["position"]));return;}
            if(method=="textDocument/rename"){result(id,rename_symbol(file,params["position"],params["newName"].string()));return;}
            auto index=indexes.find(root_for(file));
            if(method=="textDocument/documentSymbol"){
                A symbols;std::set<std::pair<int,int>> seen;if(index!=indexes.end())for(const auto& [key,s]:index->second.symbols){(void)key;if(s.at.file!=file||s.category=="parameter"||s.category=="variable"||s.category=="typeParameter"||!seen.emplace(s.at.line,s.at.column).second)continue;symbols.emplace_back(O{{"name",s.name},{"kind",s.kind},{"location",location(s.at)},{"containerName",s.owner}});}result(id,symbols);return;
            }
            if(method=="textDocument/hover"||method=="textDocument/definition"||method=="textDocument/references"){
                if(method!="textDocument/references")if(auto group=unresolved_method_group(file,params["position"]);!group.is_null()){result(id,method=="textDocument/hover"?group["hover"]:group["definitions"]);return;}
                auto o=at_position(file,params["position"]);
                if(!o||index==indexes.end()||!index->second.symbols.contains(o->symbol)){
                    if(method=="textDocument/references"){result(id,A{});return;}
                    const bool hover=method=="textDocument/hover";
                    if(hover&&o&&!o->detail.empty())result(id,O{{"contents",O{{"kind","markdown"},{"value","```bolt\n"+o->detail+"\n```"}}},{"range",range_json(text_for(file),o->at)}});
                    else result(id,editor_navigation(file,params["position"],hover));
                    return;
                }
                const auto symbol=index->second.symbols.find(o->symbol);
                const auto& s=symbol->second;
                if(method=="textDocument/hover")result(id,hover_symbol(file,*o,s,index->second));
                else if(method=="textDocument/definition")result(id,A{location(s.at)});
                else {A refs;bool include=params["context"]["includeDeclaration"].boolean();std::set<std::tuple<std::string,int,int>> seen;auto family=symbol_family(index->second,s,file);for(const auto& r:index->second.occurrences)if(family.contains(r.symbol)&&(include||!r.declaration)&&seen.emplace(r.at.file,r.at.line,r.at.column).second)refs.push_back(location(r.at));result(id,refs);}return;
            }
            error(id,-32601,"Method not implemented: "+method);
        }catch(const std::exception& e){if(request)error(id,-32602,e.what());else notify("window/logMessage",O{{"type",1},{"message","Bolt: "+std::string(e.what())}});}
    }
public:
    Server(std::istream& in,std::ostream& out,std::ostream& err,std::mutex& output_mutex)
        :input(in),output(out),log(err),writer(output_mutex){}
    int run(){
        completion_view=true;
        Server checker(input,output,log,writer);
        AnalysisQueue analysis([&](const J& message){checker.handle(message);});
        // Some .NET Framework clients flush a UTF-8 preamble when exposing a
        // redirected input stream. Accept that optional preamble exactly once;
        // subsequent messages still use ordinary ASCII LSP headers.
        bool first_header=true;
        for(;;){
            std::string line;std::optional<std::size_t> length;std::size_t headers=0;
            while(std::getline(input,line)){
                if(first_header){first_header=false;if(line.starts_with("\xef\xbb\xbf"))line.erase(0,3);}
                headers+=line.size();if(headers>8192){log<<"LSP header too large\n";return 1;}
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                if(line.empty())break;
                auto colon=line.find(':');if(colon==line.npos)continue;auto name=line.substr(0,colon);std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                if(name=="content-length"){
                    auto number=line.substr(colon+1);auto first=number.find_first_not_of(" \t");if(first==number.npos){log<<"Invalid Content-Length\n";return 1;}number.erase(0,first);auto last=number.find_last_not_of(" \t");number.resize(last+1);
                    std::size_t n=0;auto r=std::from_chars(number.data(),number.data()+number.size(),n);if(length||r.ec!=std::errc{}||r.ptr!=number.data()+number.size()||n>max_message){log<<"Invalid Content-Length\n";return 1;}length=n;
                }
            }
            if(!input)return shutdown?0:1;
            if(!length){log<<"Missing Content-Length\n";return 1;}
            std::string body(*length,'\0');input.read(body.data(),static_cast<std::streamsize>(*length));if(static_cast<std::size_t>(input.gcount())!=*length){log<<"Truncated message\n";return 1;}
            J message;
            try{message=J::parse(body);}catch(const std::exception& e){error(nullptr,-32700,e.what());continue;}
            if(!message.is_object()||message["jsonrpc"].string()!="2.0"||!message["method"].is_string()){error(nullptr,-32600,"Invalid JSON-RPC request");continue;}
            const auto method=message["method"].string();
            if(method=="exit")return shutdown?0:1;
            const bool completion=method=="textDocument/completion"||method=="textDocument/boltPendingCompletion";
            if(completion && !analysis.idle()){
                handle(message);
                continue;
            }
            // Apply lifecycle and overlay messages in wire order on both lanes.
            // Only semantic analysis emits diagnostics and lifecycle replies.
            if(method=="initialize"||method=="shutdown"||method.starts_with("workspace/didChange")||
               method=="textDocument/didOpen"||method=="textDocument/didChange"||
               method=="textDocument/didSave"||method=="textDocument/didClose"){
                analysis.push(message,body.size());
                silent=true;
                handle(message);
                silent=false;
            }else analysis.push(std::move(message),body.size());
        }
    }
};
}
int run_language_server(std::istream& input,std::ostream& output,std::ostream& log){
#ifdef _WIN32
    if(&input==&std::cin)_setmode(_fileno(stdin),_O_BINARY);
    if(&output==&std::cout)_setmode(_fileno(stdout),_O_BINARY);
#endif
    std::mutex writer;
    return Server(input,output,log,writer).run();
}
} // namespace bolt
