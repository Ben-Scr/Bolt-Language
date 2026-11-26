#pragma once
// Small TOML configuration reader. Supports the TOML values accepted by Bolt's
// schema (strings, booleans, integers, arrays and tables) including quoted/dotted
// keys, multiline strings and inline tables. Unsupported value kinds fail loudly.
#include <bolt/Diagnostics.hpp>
#include <charconv>
#include <cstdint>
#include <climits>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>
namespace bolt::project_toml {
struct Node {
    using Table=std::map<std::string,Node>;
    using Array=std::vector<Node>;
    std::variant<Table,Array,std::string,std::int64_t,bool> value=Table{};
    Location at;
    bool sealed=false, dotted=false;
    bool table() const {return std::holds_alternative<Table>(value);}
    Table& fields(){return std::get<Table>(value);}
    const Table& fields()const{return std::get<Table>(value);}
    const Node* get(const std::string& key)const{if(!table())return nullptr;auto i=fields().find(key);return i==fields().end()?nullptr:&i->second;}
};
class Parser {
    std::string text,file; std::size_t pos=0;int line=1,col=1;unsigned nesting=0;
    std::set<std::vector<std::string>> explicit_tables;
    std::map<std::string,Location> table_origins;
    [[noreturn]] void fail(const std::string& message) const {Error e(here(),message,"BLT4100");attach_source(e.diagnostic,text);throw e;}
    Location here()const{return {file,line,col,1,pos};}
    char peek(std::size_t n=0)const{return pos+n<text.size()?text[pos+n]:'\0';}
    char take(){if(pos==text.size())fail("unexpected end of TOML");char c=text[pos++];if(c=='\n'){++line;col=1;}else ++col;return c;}
    void want(char c){if(peek()!=c)fail(std::string("expected '")+c+"' in TOML");take();}
    void space(bool lines=false){while(pos<text.size()){if(peek()==' '||peek()=='\t'||(lines&&(peek()=='\r'||peek()=='\n'))){take();continue;}if(lines&&peek()=='#'){comment();continue;}break;}}
    void comment(){while(pos<text.size()&&peek()!='\n'){const unsigned char c=take();if(c<32&&c!='\t'&&c!='\r')fail("control character in comment");}}
    void newline(){space();if(peek()=='#')comment();if(pos==text.size())return;if(peek()=='\r')take();if(peek()!='\n')fail("expected newline after TOML value");take();}
    static int hex(char c){if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return -1;}
    static void unicode(std::string& out,std::uint32_t c){if(c<128)out+=char(c);else if(c<2048){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}else if(c<65536){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}}
    std::string string(bool key=false){
        const char q=take();bool multi=!key&&peek()==q&&peek(1)==q;
        if(multi){take();take();if(peek()=='\r'&&peek(1)=='\n'){take();take();}else if(peek()=='\n')take();}
        std::string out;
        while(pos<text.size()){
            char c=take();
            if(c==q){if(!multi)return out;if(peek()==q&&peek(1)==q){take();take();int extra=0;while(peek()==q&&extra<2){out+=take();++extra;}return out;}out+=c;continue;}
            if(c=='\r'){if(!multi||peek()!='\n')fail("newline in single-line string or bare carriage return");take();out+='\n';continue;}
            if(c=='\n'&&!multi)fail("newline in single-line string");
            if(static_cast<unsigned char>(c)<32&&c!='\t'&&c!='\n')fail("control character in string");
            if(c=='\\'&&q=='"'){
                if(multi){std::size_t p=pos;while(p<text.size()&&(text[p]==' '||text[p]=='\t'))++p;if(p<text.size()&&(text[p]=='\n'||text[p]=='\r')){while(pos<p)take();while(pos<text.size()&&(peek()==' '||peek()=='\t'||peek()=='\r'||peek()=='\n'))take();continue;}}
                const char e=take();switch(e){case 'b':out+='\b';break;case 't':out+='\t';break;case 'n':out+='\n';break;case 'f':out+='\f';break;case 'r':out+='\r';break;case '"':out+='"';break;case '\\':out+='\\';break;case 'u':case 'U':{std::uint32_t v=0;for(int k=0;k<(e=='u'?4:8);++k){int h=hex(take());if(h<0)fail("invalid Unicode escape");v=(v<<4)|unsigned(h);}if(v>0x10ffff||(v>=0xd800&&v<=0xdfff))fail("invalid Unicode scalar");unicode(out,v);break;}default:fail("invalid string escape");}continue;
            }
            out+=c;
        }
        fail("unterminated string");
    }
    std::vector<std::string> keys(){
        std::vector<std::string> result;
        while(true){space();std::string s;if(peek()=='"'||peek()=='\'')s=string(true);else{while(pos<text.size()){char c=peek();if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')s+=take();else break;}if(s.empty())fail("expected TOML key");}result.push_back(std::move(s));space();if(peek()!='.')break;take();}
        return result;
    }
    Node& descend(Node& n,const std::vector<std::string>& path,std::size_t count){Node* t=&n;for(std::size_t i=0;i<count;++i){if(!t->table()||t->sealed)fail("cannot extend a value or closed inline table");t=&t->fields()[path[i]];if(!t->table()||t->sealed)fail("table conflicts with existing value");}return *t;}
    void assign(Node& t,const std::vector<std::string>& key,Node v){Node* mark=&t;for(std::size_t i=0;i+1<key.size();++i){mark=&descend(*mark,{key[i]},1);mark->dotted=true;}Node& parent=descend(t,key,key.size()-1);if(!parent.fields().emplace(key.back(),std::move(v)).second)fail("duplicate key '"+key.back()+"'");}
    Node value(){
        if(++nesting>64)fail("TOML nesting limit (64) exceeded");
        struct Leave{unsigned& n;~Leave(){--n;}}leave{nesting};
        space();Node n;n.at=here();
        if(peek()=='"'||peek()=='\''){n.value=string();return n;}
        if(peek()=='['){take();n.value=Node::Array{};space(true);while(peek()!=']'){std::get<Node::Array>(n.value).push_back(value());space(true);if(peek()==']')break;want(',');space(true);}take();return n;}
        if(peek()=='{'){take();space();while(peek()!='}'){auto k=keys();want('=');assign(n,k,value());space();if(peek()=='}')break;want(',');space();if(peek()=='}')fail("trailing comma in inline TOML table");}take();n.sealed=true;return n;}
        std::string token;while(pos<text.size()&&peek()!=','&&peek()!=']'&&peek()!='}'&&peek()!='#'&&peek()!=' '&&peek()!='\t'&&peek()!='\n'&&peek()!='\r')token+=take();
        if(token=="true"||token=="false"){n.value=token=="true";return n;}
        if(token.empty())fail("expected TOML value");
        for(std::size_t i=0;i<token.size();++i)if(token[i]=='_'&&(i==0||i+1==token.size()||hex(token[i-1])<0||hex(token[i+1])<0))fail("invalid underscore in integer");
        std::string number;for(char c:token)if(c!='_')number+=c;
        bool neg=false;std::size_t start=0;int base=10;
        if(number[0]=='+'||number[0]=='-'){neg=number[0]=='-';start=1;}
        if(start+2<number.size()&&number[start]=='0'){const char p=number[start+1];if(p=='x'||p=='o'||p=='b'){if(start)fail("non-decimal integers cannot have a sign");base=p=='x'?16:p=='o'?8:2;start+=2;}}
        if(base==10&&start+1<number.size()&&number[start]=='0')fail("leading zeros in decimal integer");
        std::uint64_t magnitude=0;auto r=std::from_chars(number.data()+start,number.data()+number.size(),magnitude,base);
        if(r.ec!=std::errc{}||r.ptr!=number.data()+number.size()||start==number.size())fail("this configuration schema accepts strings, booleans, integers, arrays and tables; unsupported or invalid value '"+token+"'");
        const auto limit=std::uint64_t(INT64_MAX)+(neg?1:0);if(magnitude>limit)fail("TOML integer outside signed 64-bit range");n.value=neg?(magnitude==limit?INT64_MIN:-static_cast<std::int64_t>(magnitude)):static_cast<std::int64_t>(magnitude);return n;
    }
public:
    Parser(std::string source,std::string filename):text(std::move(source)),file(std::move(filename)){
        if(text.size()>1024*1024)fail("project configuration exceeds 1 MiB");
        if(text.starts_with("\xef\xbb\xbf"))text.erase(0,3);
        for(std::size_t i=0;i<text.size();){auto c=static_cast<unsigned char>(text[i++]);if(c<128){if(c==0)fail("NUL in TOML");continue;}unsigned n=c>=0xf0?3:c>=0xe0?2:c>=0xc2?1:99;if(n>3||i+n>text.size())fail("invalid UTF-8 TOML");std::uint32_t v=c&((1u<<(6-n))-1);for(unsigned j=0;j<n;++j){unsigned a=static_cast<unsigned char>(text[i++]);if((a&0xc0)!=0x80)fail("invalid UTF-8 TOML");v=(v<<6)|(a&63);}if(v>0x10ffff||(v>=0xd800&&v<=0xdfff)||(n==1&&v<128)||(n==2&&v<2048)||(n==3&&v<65536))fail("invalid UTF-8 TOML");}
    }
    Node parse(){Node root;root.at=here();Node* current=&root;while(pos<text.size()){space(true);if(pos==text.size())break;if(peek()=='['){take();if(peek()=='[')fail("arrays of tables are not supported by the Bolt configuration schema");auto path=keys();want(']');if(!explicit_tables.insert(path).second)fail("duplicate table");current=&descend(root,path,path.size());if(current->dotted)fail("table was already defined by a dotted key");current->at=here();newline();}else{auto k=keys();want('=');assign(*current,k,value());newline();}}return root;}
};
} // namespace bolt::project_toml
