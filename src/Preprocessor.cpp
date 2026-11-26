#include <bolt/Preprocessor.hpp>
#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

namespace bolt {
namespace {
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool identifier(std::string_view s) {
    return !s.empty() && alpha(s[0]) && std::all_of(s.begin()+1,s.end(),[](char c){return alpha(c)||digit(c);});
}
bool reserved(std::string_view s) {
    for(auto name:builtin_define_names())if(s==name)return true;
    return s=="true"||s=="false"||s=="defined";
}
std::string_view trim(std::string_view s) {
    while(!s.empty()&&(s.front()==' '||s.front()=='\t'||s.front()=='\r'))s.remove_prefix(1);
    while(!s.empty()&&(s.back()==' '||s.back()=='\t'||s.back()=='\r'))s.remove_suffix(1);
    return s;
}
class Condition {
    std::string_view source;
    const std::map<std::string,bool>& symbols;
    Location at;
    std::size_t pos=0, depth=0;
    void space(){while(pos<source.size()&&(source[pos]==' '||source[pos]=='\t'||source[pos]=='\r'))++pos;}
    bool take(std::string_view t){space();if(source.substr(pos,t.size())!=t)return false;pos+=t.size();return true;}
    [[noreturn]] void error(const std::string& msg)const {auto p=at;p.column+=static_cast<int>(pos);p.offset+=pos;throw Error(p,msg,"BLT1202");}
    std::string name(){space();auto start=pos;if(pos>=source.size()||!alpha(source[pos]))error("expected a conditional-compilation symbol");while(pos<source.size()&&(alpha(source[pos])||digit(source[pos])))++pos;return std::string(source.substr(start,pos-start));}
    bool term(){
        if(++depth>128)error("conditional expression nesting limit exceeded (128)");
        struct Exit{std::size_t& d;~Exit(){--d;}}exit{depth};
        if(take("!"))return !term();
        if(take("(")){auto value=logical_or();if(!take(")"))error("expected ')' in conditional expression");return value;}
        space();
        if(pos<source.size()&&(source[pos]=='0'||source[pos]=='1')){bool value=source[pos++]=='1';if(pos<source.size()&&(alpha(source[pos])||digit(source[pos])))error("conditional values must be boolean (0 or 1)");return value;}
        auto n=name();
        if(n=="true")return true;if(n=="false")return false;
        if(n=="defined") {bool parentheses=take("(");auto symbol=name();if(parentheses&&!take(")"))error("expected ')' after defined(symbol)");return symbols.contains(symbol);}
        auto found=symbols.find(n);return found!=symbols.end()&&found->second;
    }
    bool logical_and(){auto value=term();while(take("&&")){auto rhs=term();value=value&&rhs;}return value;}
    bool logical_or(){auto value=logical_and();while(take("||")){auto rhs=logical_and();value=value||rhs;}return value;}
public:
    Condition(std::string_view s,const std::map<std::string,bool>& v,Location location):source(s),symbols(v),at(std::move(location)){}
    bool evaluate(){if(trim(source).empty())error("missing #if/#elif condition");auto result=logical_or();space();if(pos!=source.size())error("unsupported conditional expression; use symbols, !, &&, || and parentheses");return result;}
};
// Only active source affects lexical state. In an excluded branch arbitrary,
// even syntactically invalid, text is permitted; nested directives still count.
struct LexicalState {
    bool block_comment=false;char quote=0;
    void line(std::string_view s){
        for(std::size_t i=0;i<s.size();++i){
            if(block_comment){if(s.substr(i,2)=="*/"){block_comment=false;++i;}continue;}
            if(quote){if(s[i]=='\\'&&i+1<s.size()){++i;continue;}if(s[i]==quote)quote=0;continue;}
            if(s.substr(i,2)=="//")break;
            if(s.substr(i,2)=="/*"){block_comment=true;++i;continue;}
            if(s[i]=='\''||s[i]=='\"')quote=s[i];
        }
        // Ordinary Bolt string/char literals cannot span a physical newline.
        quote=0;
    }
};
std::string directive_body(std::string_view line,Location at){
    std::string clean;
    for(std::size_t i=0;i<line.size();){
        if(line.substr(i,2)=="//")break;
        if(line.substr(i,2)=="/*") {auto end=line.find("*/",i+2);if(end==std::string_view::npos)throw Error(at,"a directive's block comment must close on the same line","BLT1202");clean.push_back(' ');i=end+2;}
        else clean.push_back(line[i++]);
    }
    return clean;
}
}
const std::vector<std::string_view>& builtin_define_names(){
    static const std::vector<std::string_view> names={"Windows","Linux","MacOS","FreeBSD","IOS","Debug","Release","X64","ARM64","Web","WASM64","Android","RISCV64","X86","ARM32"};return names;
}
PreprocessorOptions normalized_preprocessor_options(PreprocessorOptions o){
    if(o.platform.empty()) {
#if defined(__EMSCRIPTEN__)
        o.platform="Web";
#elif defined(__ANDROID__)
        o.platform="Android";
#elif defined(_WIN32)
        o.platform="Windows";
#elif defined(__APPLE__) && (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__))
        o.platform="IOS";
#elif defined(__APPLE__)
        o.platform="MacOS";
#elif defined(__FreeBSD__)
        o.platform="FreeBSD";
#elif defined(__linux__)
        o.platform="Linux";
#else
        throw std::invalid_argument("unsupported conditional-compilation target OS; select a supported desktop, mobile or Web target");
#endif
    }
    if(o.architecture.empty()) {
#if defined(__wasm64__)
        o.architecture="WASM64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        o.architecture="ARM64";
#elif defined(__riscv) && __riscv_xlen==64
        o.architecture="RISCV64";
#elif defined(_M_X64) || defined(__x86_64__)
        o.architecture="X64";
#elif defined(_M_IX86) || defined(__i386__)
        o.architecture="X86";
#elif defined(_M_ARM) || defined(__arm__)
        o.architecture="ARM32";
#else
        throw std::invalid_argument("unsupported conditional-compilation architecture; select X64, ARM64, RISCV64, WASM64, X86 or ARM32");
#endif
    }
    if(o.configuration.empty())o.configuration="Debug";
    if(o.platform!="Windows"&&o.platform!="Linux"&&o.platform!="MacOS"&&o.platform!="FreeBSD"&&o.platform!="IOS"&&o.platform!="Web"&&o.platform!="Android")throw std::invalid_argument("invalid conditional-compilation platform: "+o.platform);
    if(o.architecture!="X64"&&o.architecture!="ARM64"&&o.architecture!="WASM64"&&o.architecture!="RISCV64"&&o.architecture!="X86"&&o.architecture!="ARM32")throw std::invalid_argument("invalid conditional-compilation architecture: "+o.architecture);
    if((o.platform=="Web")!=(o.architecture=="WASM64"))throw std::invalid_argument("Web requires the WASM64 language ABI");
    if(o.configuration!="Debug"&&o.configuration!="Release")throw std::invalid_argument("invalid conditional-compilation profile: "+o.configuration);
    for(const auto& [symbol,value]:o.defines){(void)value;if(!identifier(symbol)||reserved(symbol))throw std::invalid_argument("invalid or reserved user define: "+symbol);}
#ifdef BOLT_LINUX_MUSL
    if(o.platform=="Linux")o.defines["BoltMusl"]=true;
#endif
    return o;
}
std::map<std::string,bool> preprocessor_symbols(const PreprocessorOptions& options){
    auto o=normalized_preprocessor_options(options);auto symbols=o.defines;
    symbols.emplace(o.platform,true);symbols.emplace(o.architecture,true);symbols.emplace(o.configuration,true);return symbols;
}
std::string preprocessor_identity(const PreprocessorOptions& options){
    auto o=normalized_preprocessor_options(options);std::string identity=o.platform+"/"+o.architecture+"/"+o.configuration;
    for(const auto& [name,value]:o.defines)identity+=";"+name+"="+(value?"1":"0");return identity;
}
PreprocessedSource preprocess(std::string_view source,const std::string& file,const PreprocessorOptions& options){
    PreprocessedSource out;out.text=std::string(source);auto symbols=preprocessor_symbols(options);
    // With no directive marker, source and diagnostic offsets are already final.
    if(source.find('#')==std::string_view::npos)return out;
    struct Branch{bool parent,selected,active,seen_else=false;Location opening;};
    std::vector<Branch> branches;LexicalState lexical;
    auto active=[&](){return branches.empty()||branches.back().active;};
    auto mask=[&](std::size_t start,std::size_t end){for(auto i=start;i<end;++i)if(out.text[i]!='\n'&&out.text[i]!='\r')out.text[i]=' ';};
    std::size_t start=0;int line_number=1;
    while(start<source.size()){
        auto end=source.find('\n',start);if(end==std::string_view::npos)end=source.size();
        std::size_t first=start;
        if(first==0&&source.substr(0,3)=="\xef\xbb\xbf")first=3;
        while(first<end&&(source[first]==' '||source[first]=='\t'||source[first]=='\r'))++first;
        const bool directive=first<end&&source[first]=='#'&&(!active()||!lexical.block_comment);
        Location at{file,line_number,static_cast<int>(first-start+1),static_cast<int>(end-first),first};
        if(directive){
            const auto body=directive_body(source.substr(first+1,end-first-1),at);auto view=trim(body);
            auto word_end=std::size_t{0};while(word_end<view.size()&&alpha(view[word_end]))++word_end;
            auto word=view.substr(0,word_end), argument=trim(view.substr(word_end));
            auto fail=[&](const std::string& msg){throw Error(at,msg,"BLT1201");};
            auto symbol=[&](){if(!identifier(argument))fail("#"+std::string(word)+" expects one symbol name");return std::string(argument);};
            auto condition=[&](){auto location=at;const auto off=source.substr(first+1,end-first-1).find(argument);if(off!=std::string_view::npos){location.offset=first+1+off;location.column=static_cast<int>(location.offset-start+1);location.length=static_cast<int>(argument.size());}return Condition(argument,symbols,location).evaluate();};
            if(word=="if"||word=="ifdef"||word=="ifndef"){
                if(branches.size()>=256)fail("conditional-compilation nesting limit exceeded (256)");
                bool selected=word=="if"?condition():symbols.contains(symbol());if(word=="ifndef")selected=!selected;
                const bool parent=active();branches.push_back({parent,selected,parent&&selected,false,at});
            }else if(word=="elif"){
                if(branches.empty())fail("#elif without a matching #if");auto& branch=branches.back();if(branch.seen_else)fail("#elif is not allowed after #else");auto value=condition();branch.active=branch.parent&&!branch.selected&&value;branch.selected|=value;
            }else if(word=="else"){
                if(branches.empty())fail("#else without a matching #if");if(!argument.empty())fail("#else takes no arguments");auto& branch=branches.back();if(branch.seen_else)fail("duplicate #else");branch.seen_else=true;branch.active=branch.parent&&!branch.selected;branch.selected=true;
            }else if(word=="endif"){
                if(branches.empty())fail("#endif without a matching #if");if(!argument.empty())fail("#endif takes no arguments");branches.pop_back();
            }else if(word=="define"){
                if(active()){
                    auto n=std::size_t{0};while(n<argument.size()&&(alpha(argument[n])||digit(argument[n])))++n;
                    auto name=argument.substr(0,n),value=trim(argument.substr(n));
                    if(!identifier(name))fail("#define expects a symbol name");
                    if(reserved(name))fail("built-in conditional symbols are reserved and cannot be redefined: "+std::string(name));
                    if(!value.empty()&&value!="true"&&value!="false"&&value!="0"&&value!="1")fail("only boolean #define symbols are supported; text and function macros are not supported");
                    symbols[std::string(name)]=value.empty()||value=="true"||value=="1";
                }
            }else if(word=="undef"){
                if(active()){auto name=symbol();if(reserved(name))fail("built-in conditional symbols cannot be undefined: "+name);symbols.erase(name);}
            }else if(active())fail("unsupported preprocessor directive '#"+std::string(word)+"'; only conditional compilation is supported");
            out.directives.push_back(at);mask(first,end);
        }else if(!active()) {at.column=1;at.offset=start;at.length=static_cast<int>(end-start);out.inactive.push_back(at);mask(start,end);}
        else lexical.line(source.substr(start,end-start));
        start=end<source.size()?end+1:end;++line_number;
    }
    if(!branches.empty())throw Error(branches.back().opening,"unterminated conditional block; expected #endif","BLT1201");
    return out;
}
} // namespace bolt
