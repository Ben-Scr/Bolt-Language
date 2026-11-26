#pragma once
// Bounded runtime JSON value codec; independent of the LSP codec.
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace bolt::library::json_detail {
class Json {
public:
    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
private:
    using Value = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object>;
    Value value = nullptr;
    static void utf8(std::string& s, unsigned int n) {
        if(n<=0x7f)s+=static_cast<char>(n);
        else if(n<=0x7ff){s+=static_cast<char>(0xc0|(n>>6));s+=static_cast<char>(0x80|(n&63));}
        else if(n<=0xffff){s+=static_cast<char>(0xe0|(n>>12));s+=static_cast<char>(0x80|((n>>6)&63));s+=static_cast<char>(0x80|(n&63));}
        else{s+=static_cast<char>(0xf0|(n>>18));s+=static_cast<char>(0x80|((n>>12)&63));s+=static_cast<char>(0x80|((n>>6)&63));s+=static_cast<char>(0x80|(n&63));}
    }
    class Parser {
        std::string_view s;
        std::size_t p=0;
        [[noreturn]] void fail() const { throw std::runtime_error("Invalid JSON at byte "+std::to_string(p)); }
        void space(){while(p<s.size()&&(s[p]==' '||s[p]=='\r'||s[p]=='\n'||s[p]=='\t'))++p;}
        bool take(char c){space();if(p<s.size()&&s[p]==c){++p;return true;}return false;}
        unsigned int hex4(){unsigned int v=0;for(int i=0;i<4;++i){if(p==s.size())fail();char c=s[p++];v<<=4;
            if(c>='0'&&c<='9')v+=c-'0';else if(c>='a'&&c<='f')v+=c-'a'+10;else if(c>='A'&&c<='F')v+=c-'A'+10;else fail();}return v;}
        std::string string(){
            if(!take('"'))fail();
            std::string out;
            while(p<s.size()){
                unsigned char c=static_cast<unsigned char>(s[p++]);
                if(c=='"')return out;
                if(c<32)fail();
                if(c=='\\'){
                    if(p==s.size())fail();
                    c=static_cast<unsigned char>(s[p++]);
                    switch(c){case '"':case '\\':case '/':out+=static_cast<char>(c);break;
                    case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
                    case 'u':{unsigned int n=hex4();if(n>=0xd800&&n<=0xdbff){if(p+2>s.size()||s[p++]!='\\'||s[p++]!='u')fail();unsigned int low=hex4();if(low<0xdc00||low>0xdfff)fail();n=0x10000+((n-0xd800)<<10)+(low-0xdc00);}else if(n>=0xdc00&&n<=0xdfff)fail();utf8(out,n);break;}
                    default:fail();}
                }else if(c<128)out+=static_cast<char>(c);
                else { // Reject malformed/overlong UTF-8 and encoded surrogate code points.
                    unsigned int n=0,min=0;int count=0;
                    if(c>=0xc2&&c<=0xdf){n=c&31;count=1;min=0x80;}
                    else if(c>=0xe0&&c<=0xef){n=c&15;count=2;min=0x800;}
                    else if(c>=0xf0&&c<=0xf4){n=c&7;count=3;min=0x10000;}else fail();
                    for(int i=0;i<count;++i){if(p==s.size())fail();auto b=static_cast<unsigned char>(s[p++]);if((b&0xc0)!=0x80)fail();n=(n<<6)|(b&63);}
                    if(n<min||n>0x10ffff||(n>=0xd800&&n<=0xdfff))fail();
                    utf8(out,n);
                }
            }fail();
        }
        Json parse_value(int depth){
            if(depth>96)throw std::runtime_error("JSON nesting limit exceeded");
            space();
            if(p==s.size())fail();
            char c=s[p];
            if(c=='"')return string();
            if(c=='{'){++p;Object o;if(take('}'))return o;do{auto k=string();if(!take(':'))fail();auto v=parse_value(depth+1);if(!o.emplace(std::move(k),std::move(v)).second)fail();}while(take(','));if(!take('}'))fail();return o;}
            if(c=='['){++p;Array a;if(take(']'))return a;do{a.push_back(parse_value(depth+1));}while(take(','));if(!take(']'))fail();return a;}
            if(s.substr(p,4)=="null"){p+=4;return nullptr;}if(s.substr(p,4)=="true"){p+=4;return true;}if(s.substr(p,5)=="false"){p+=5;return false;}
            auto begin=p;if(c=='-')++p;if(p==s.size())fail();
            if(s[p]=='0')++p;else{if(s[p]<'1'||s[p]>'9')fail();while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;}
            bool real=false;
            if(p<s.size()&&s[p]=='.'){real=true;++p;auto start=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(p==start)fail();}
            if(p<s.size()&&(s[p]=='e'||s[p]=='E')){real=true;++p;if(p<s.size()&&(s[p]=='+'||s[p]=='-'))++p;auto start=p;while(p<s.size()&&s[p]>='0'&&s[p]<='9')++p;if(p==start)fail();}
            auto text=s.substr(begin,p-begin);
            if(!real){
                if(text.front()=='-'){std::int64_t v=0;auto r=std::from_chars(text.data(),text.data()+text.size(),v);if(r.ec==std::errc{}&&r.ptr==text.data()+text.size())return v;}
                else {std::uint64_t v=0;auto r=std::from_chars(text.data(),text.data()+text.size(),v);if(r.ec==std::errc{}&&r.ptr==text.data()+text.size())return v;}
                throw std::runtime_error("JSON integer is outside the supported 64-bit range at byte "+std::to_string(begin));
            }
            std::istringstream in{std::string(text)};in.imbue(std::locale::classic());double v=0;in>>v;if(!in||!std::isfinite(v))fail();return v;
        }
    public:
        explicit Parser(std::string_view text):s(text){}
        Json run(){auto j=parse_value(0);space();if(p!=s.size())fail();return j;}
    };
    static std::string quoted(std::string_view s){std::string o="\"";const char* h="0123456789abcdef";
        for(unsigned char c:s){if(c=='"'||c=='\\'){o+='\\';o+=static_cast<char>(c);}else if(c<32){o+="\\u00";o+=h[c>>4];o+=h[c&15];}else o+=static_cast<char>(c);}return o+'"';}
public:
    Json()=default;
    Json(std::nullptr_t):value(nullptr){}
    Json(bool v):value(v){}
    Json(int v):value(static_cast<std::int64_t>(v)){}
    Json(std::int64_t v):value(v){}
    Json(std::uint64_t v):value(v){}
    Json(double v):value(v){if(!std::isfinite(v))throw std::invalid_argument("non-finite JSON number");}
    Json(const char* v):value(std::string(v)){}
    Json(std::string v):value(std::move(v)){}
    Json(Object v):value(std::move(v)){}
    Json(Array v):value(std::move(v)){}
    bool is_null() const{return std::holds_alternative<std::nullptr_t>(value);}
    bool is_object() const{return std::holds_alternative<Object>(value);}
    bool is_array() const{return std::holds_alternative<Array>(value);}
    bool is_string() const{return std::holds_alternative<std::string>(value);}
    bool is_integer() const{return std::holds_alternative<std::int64_t>(value);}
    bool is_unsigned()const{return std::holds_alternative<std::uint64_t>(value);}
    bool is_bool()const{return std::holds_alternative<bool>(value);}
    bool is_real()const{return std::holds_alternative<double>(value);}
    std::uint64_t unsigned_integer()const{return std::get<std::uint64_t>(value);}
    double real()const{return std::get<double>(value);}
    long double number()const{if(is_integer())return static_cast<long double>(std::get<std::int64_t>(value));if(is_unsigned())return static_cast<long double>(std::get<std::uint64_t>(value));if(is_real())return std::get<double>(value);throw std::runtime_error("Expected a JSON number");}
    bool contains(const std::string& k)const{return is_object()&&std::get<Object>(value).contains(k);}
    const Json& operator[](const std::string& k)const{static const Json null;if(!is_object())return null;auto& o=std::get<Object>(value);auto i=o.find(k);return i==o.end()?null:i->second;}
    Json& operator[](const std::string& k){if(is_null())value=Object{};return std::get<Object>(value)[k];}
    const Array& array()const{static const Array empty;return is_array()?std::get<Array>(value):empty;}
    Array& array(){return std::get<Array>(value);}
    const Object& object()const{static const Object empty;return is_object()?std::get<Object>(value):empty;}
    std::string string(std::string fallback={})const{return is_string()?std::get<std::string>(value):std::move(fallback);}
    std::int64_t integer(std::int64_t fallback=0)const{return is_integer()?std::get<std::int64_t>(value):fallback;}
    bool boolean(bool fallback=false)const{return std::holds_alternative<bool>(value)?std::get<bool>(value):fallback;}
    static Json parse(std::string_view text){return Parser(text).run();}
    std::string dump()const{
        if(is_null())return "null";
        if(std::holds_alternative<bool>(value))return std::get<bool>(value)?"true":"false";
        if(is_integer())return std::to_string(std::get<std::int64_t>(value));
        if(is_unsigned())return std::to_string(std::get<std::uint64_t>(value));
        if(std::holds_alternative<double>(value)){std::ostringstream o;o.imbue(std::locale::classic());o<<std::setprecision(17)<<std::get<double>(value);return o.str();}
        if(is_string())return quoted(std::get<std::string>(value));
        std::string o=is_array()?"[":"{";bool first=true;
        if(is_array())for(const auto& j:array()){if(!first)o+=',';first=false;o+=j.dump();}
        else for(const auto& [k,j]:object()){if(!first)o+=',';first=false;o+=quoted(k)+":"+j.dump();}
        return o+(is_array()?"]":"}");
    }
};
} // namespace bolt::library::json_detail
