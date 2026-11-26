#include <bolt/Preprocessor.hpp>
#include <bolt/Frontend.hpp>
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
using namespace bolt;
static int checks=0;
static void check(bool ok,const char* name){if(!ok)throw std::runtime_error(name);++checks;}
static void invalid(const std::string& text,const std::string& expected){
    try{(void)preprocess(text,"sample.bolt");throw std::runtime_error("unexpected success: "+text);}
    catch(const Error& e){check(e.diagnostic.code=="BLT1201"||e.diagnostic.code=="BLT1202","preprocessor diagnostic code");check(std::string(e.what()).find(expected)!=std::string::npos,"preprocessor diagnostic message");}
}
int main(){try{
    PreprocessorOptions web;
    web.platform="Web";web.architecture="WASM64";web.configuration="Release";
    auto web_symbols=preprocessor_symbols(web);
    check(web_symbols.contains("Web") && web_symbols.contains("WASM64") && !web_symbols.contains("Linux"),"Web release symbols");
    PreprocessorOptions riscv{"Linux","RISCV64","Debug",{}};
    auto rv_symbols=preprocessor_symbols(riscv);
    check(rv_symbols.contains("RISCV64") && !rv_symbols.contains("X64"),"RV64 Linux debug symbols");
    auto names=builtin_define_names();
    check(std::set<std::string_view>(names.begin(),names.end())==std::set<std::string_view>{"Windows","Linux","MacOS","FreeBSD","Debug","Release","X64","ARM64","Android","IOS","Web","WASM64","RISCV64","X86","ARM32"},"target and profile built-in names");
    for(const auto& os:{"Windows","Linux","MacOS","FreeBSD","Android","IOS"})for(const auto& arch:{"X64","ARM64"})for(const auto& profile:{"Debug","Release"}){
        PreprocessorOptions o{os,arch,profile,{}};auto syms=preprocessor_symbols(o);
        check(syms.size()==3&&syms.at(os)&&syms.at(arch)&&syms.at(profile),"target platform/arch/profile symbol matrix");
        auto src=std::string("#if ")+os+" && "+arch+" && "+profile+"\nENABLED\n#else\nDISABLED\n#endif\n";
        auto r=preprocess(src,"sample.bolt",o);check(r.text.find("ENABLED")!=std::string::npos&&r.text.find("DISABLED")==std::string::npos,"target condition selection");
        check(r.text.size()==src.size(),"preprocessed length preserved");
        for(std::size_t i=0;i<src.size();++i)if(src[i]=='\n')check(r.text[i]=='\n',"source line offsets preserved");
    }
    auto r=preprocess("#define FLAG false\n#ifdef FLAG\nPRESENT\n#endif\n#if FLAG\nVALUE_FALSE\n#elif !FLAG && defined(FLAG)\nVALUE_TRUE\n#endif\n#undef FLAG\n#if defined FLAG\nGONE\n#else\nAFTER\n#endif", "sample.bolt");
    check(r.text.find("PRESENT")!=std::string::npos&&r.text.find("VALUE_TRUE")!=std::string::npos&&r.text.find("AFTER")!=std::string::npos,"defined vs false semantics");
    check(r.text.find("VALUE_FALSE")==std::string::npos&&r.text.find("GONE")==std::string::npos,"false and undef exclusion");
    r=preprocess("#if false\n#define FLAG\n#undef Debug\n#unknown arbitrary\n#if true\n@ broken \"unterminated\n#endif\n#endif\n#ifndef FLAG\nVALID\n#endif\n","sample.bolt");
    check(r.text.find("VALID")!=std::string::npos&&r.text.find("broken")==std::string::npos,"inactive definitions and invalid code excluded");
    r=preprocess("/*\n#if Invalid\n*/\nstring x=\"#endif\"; // #if X\n#if true\nYES\n#endif\n","sample.bolt");
    check(r.text.find("#if Invalid")!=std::string::npos&&r.text.find("YES")!=std::string::npos,"directives in active comments/strings ignored");
    const std::string crlf="\xef\xbb\xbf#if true\r\nint x = 5;\r\n#else\r\nÜ @ broken\r\n#endif\r\n";
    r=preprocess(crlf,"unicode.bolt");check(r.text.size()==crlf.size()&&r.text.find("int x = 5;")==crlf.find("int x = 5;"),"BOM UTF8 CRLF offsets preserved");
    for(std::size_t i=0;i<crlf.size();++i)if(crlf[i]=='\r'||crlf[i]=='\n')check(crlf[i]==r.text[i],"all newline bytes preserved");
    auto a=preprocess("#define LOCAL\nLOCAL\n","a.bolt"),b=preprocess("#ifdef LOCAL\nBAD\n#endif\n","b.bolt");
    check(a.text.find("LOCAL")!=std::string::npos&&b.text.find("BAD")==std::string::npos,"user symbols are file local and never text replaced");
    check(preprocessor_identity({"Linux","X64","Debug",{{"X",true}}})!=preprocessor_identity({"Linux","X64","Release",{{"X",true}}}),"profile cache identity");
    check(preprocessor_identity({"Linux","X64","Debug",{{"X",true}}})!=preprocessor_identity({"Linux","X64","Debug",{{"X",false}}}),"boolean define cache identity");
    r=preprocess("#if !(false || 0) && (true || Missing) // trailing\nYES\n#endif\n","sample.bolt");check(r.text.find("YES")!=std::string::npos,"boolean precedence");
    r=preprocess("#define X /* comment */ true\n#if X /* here */ && 1\nYES\n#endif","sample.bolt");check(r.text.find("YES")!=std::string::npos,"same-line comments");
    for(auto n:names){invalid("#define "+std::string(n)+"\n","reserved");invalid("#undef "+std::string(n)+"\n","cannot be undefined");}
    invalid("#else\n","without a matching");invalid("#endif\n","without a matching");invalid("#elif true\n","without a matching");
    invalid("#if true\n","unterminated");invalid("#if true\n#else\n#else\n#endif","duplicate");invalid("#if true\n#else\n#elif true\n#endif","after #else");
    invalid("#if 1+1\n#endif","unsupported");invalid("#if\n#endif","missing");invalid("#ifdef A B\n#endif","one symbol");
    invalid("#define SUM(a,b) a+b\n","function macros");invalid("#define COUNT 25\n","only boolean");invalid("#include \"x\"\n","unsupported");
    invalid("#if true\n#endif extra\n","no arguments");invalid("#define X /*","must close");
    std::string deep;for(int i=0;i<257;i++)deep+="#if true\n";invalid(deep,"nesting limit");
    invalid("#if "+std::string(140,'!')+"true\n#endif","nesting limit");
    bool threw=false;try{(void)preprocessor_symbols({"Linux","X64","Debug",{{"Windows",true}}});}catch(const std::invalid_argument&){threw=true;}check(threw,"API builtin override rejected");
    try{(void)parse("#if false\nunknown @\n#endif\nclass A { int value = ; }\n","position.bolt");throw std::runtime_error("syntax error expected");}
    catch(const Error& e){check(e.location.line==4,"normal parse error references original line");}
    std::cout<<checks<<" preprocessor checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
