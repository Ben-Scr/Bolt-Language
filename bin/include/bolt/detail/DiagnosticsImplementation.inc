#include <bolt/Diagnostics.hpp>
#include <bolt/Path.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace bolt {
namespace {
std::string legacy(const Location& at, const std::string& message) {
    return at.file + ":" + std::to_string(at.line) + ":" + std::to_string(at.column) + ": " + message;
}
DiagnosticPhase phase_for(const std::string& code) {
    if(code.starts_with("BLT10")) return DiagnosticPhase::Lexing;
    if(code.starts_with("BLT11")) return DiagnosticPhase::Parsing;
    if(code.starts_with("BLT3")) return DiagnosticPhase::Runtime;
    if(code.starts_with("BLT4")) return DiagnosticPhase::Tooling;
    return DiagnosticPhase::Semantics;
}
std::string hint_for(const std::string& code) {
    if(code=="BLT1002") return "Close the string with a double quote; write \\n for a newline inside a string.";
    if(code=="BLT1003") return "Close the block comment with */.";
    if(code=="BLT1101") return "Check the highlighted token and the end of the preceding statement.";
    if(code=="BLT2001") return "Check the type spelling, its namespace, and whether its .bolt file belongs to this project.";
    if(code=="BLT2002") return "Use a compatible value or an explicit supported conversion. References require a compatible caller-owned variable.";
    if(code=="BLT2003") return "Declare this name before using it, check its spelling, or import its namespace.";
    if(code=="BLT2004") return "Check the member name and the receiver's type; member names are case-sensitive.";
    if(code=="BLT2005") return "Use the type within its declaring scope, or make the declaration public.";
    if(code=="BLT2101") return "Supply a type that satisfies every listed concept, or change the generic constraint.";
    if(code=="BLT3001") return "Check the divisor before dividing or taking the remainder.";
    if(code=="BLT3002") return "Use an index greater than or equal to zero and less than the array Length or collection Count.";
    if(code=="BLT3003") return "Check that the pointer is not null before dereferencing it.";
    if(code=="BLT3004") return "The result is outside the supported range. Check the input or use a wider numeric type.";
    return {};
}
std::string escape_json(std::string_view value) {
    std::string out="\"";
    const char* hex="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\') { out+='\\'; out+=static_cast<char>(c); }
        else if(c<32) { out+="\\u00"; out+=hex[c>>4]; out+=hex[c&15]; }
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
std::string position_json(const Location& at) {
    return "{\"file\":"+escape_json(at.file)+",\"line\":"+std::to_string(at.line)+
        ",\"column\":"+std::to_string(at.column)+",\"length\":"+std::to_string(std::max(1,at.length))+"}";
}
std::string notes_json(const std::vector<DiagnosticNote>& values) {
    std::string result="[";
    for(const auto& n:values) { if(result.size()>1)result+=',';
        result+="{\"location\":"+position_json(n.location)+",\"message\":"+escape_json(n.message)+"}"; }
    return result+"]";
}
std::string sanitize(std::string_view s, bool single_line=false) {
    std::string out;
    for(unsigned char c:s) {
        if(c=='\t')out+=single_line?" ":"    ";
        else if(c<32||c==127)out+=' ';
        else out+=static_cast<char>(c);
    }
    return out;
}
std::string snippet(const Diagnostic& d) {
    if(!d.source_line.empty()) return d.source_line;
    if(d.location.file.empty()||d.location.file.front()=='<') return {};
    std::ifstream in(path_from_utf8(d.location.file),std::ios::binary);
    std::string line;
    for(int n=1;n<=d.location.line&&std::getline(in,line);++n)if(n==d.location.line) {
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        return line;
    }
    return {};
}
}
Error::Error(Location at,const std::string& message,std::string code)
    :std::runtime_error(legacy(at,message)),location(at) {
    diagnostic.location=std::move(at); diagnostic.message=message;
    diagnostic.code=std::move(code); diagnostic.phase=phase_for(diagnostic.code);
    diagnostic.hint=hint_for(diagnostic.code);
}
Error::Error(Diagnostic value):std::runtime_error(legacy(value.location,value.message)),location(value.location),diagnostic(std::move(value)) {}
const char* severity_name(Severity s) noexcept {
    return s==Severity::Warning?"warning":s==Severity::Information?"information":"error";
}
const char* phase_name(DiagnosticPhase p) noexcept {
    switch(p) { case DiagnosticPhase::Lexing:return "lexing"; case DiagnosticPhase::Parsing:return "parsing";
    case DiagnosticPhase::Semantics:return "semantics";case DiagnosticPhase::Runtime:return "runtime";default:return "tooling"; }
}
std::string source_line_at(std::string_view source,int number) {
    if(number<1)return {};
    std::size_t start=0;
    for(int n=1;n<number;++n) { start=source.find('\n',start);if(start==source.npos)return {};++start; }
    auto end=source.find('\n',start);if(end==source.npos)end=source.size();
    if(end>start&&source[end-1]=='\r')--end;
    return std::string(source.substr(start,end-start));
}
void attach_source(Diagnostic& d,std::string_view source) { d.source_line=source_line_at(source,d.location.line); }
std::string format_diagnostic(const Diagnostic& d,DiagnosticFormat format) {
    if(format==DiagnosticFormat::Json) {
        return "{\"code\":"+escape_json(d.code)+",\"severity\":"+escape_json(severity_name(d.severity))+
            ",\"phase\":"+escape_json(phase_name(d.phase))+",\"message\":"+escape_json(d.message)+
            ",\"location\":"+position_json(d.location)+",\"hint\":"+escape_json(d.hint)+
            ",\"sourceLine\":"+escape_json(snippet(d))+",\"notes\":"+notes_json(d.notes)+
            ",\"stack\":"+notes_json(d.frames)+"}";
    }
    std::ostringstream out;
    if(format==DiagnosticFormat::MSBuild) {
        out<<sanitize(d.location.file,true)<<'('<<d.location.line<<','<<d.location.column<<"): "
           <<severity_name(d.severity)<<' '<<d.code<<": "<<sanitize(d.message,true)<<'\n';
        return out.str();
    }
    out<<sanitize(d.location.file,true)<<':'<<d.location.line<<':'<<d.location.column<<": "
       <<severity_name(d.severity)<<' '<<d.code<<" ["<<phase_name(d.phase)<<"]: "<<sanitize(d.message,true)<<'\n';
    const auto source=snippet(d);
    if(!source.empty()) {
        // Clip huge source lines and never emit terminal control sequences.
        const auto col=std::min(source.size(),static_cast<std::size_t>(std::max(1,d.location.column)-1));
        auto start=col>100?col-100:0;
        auto end=std::min(source.size(),start+240);
        const auto prefix=start?"...":"";
        auto shown=sanitize(std::string_view(source).substr(start,end-start));
        auto before=sanitize(std::string_view(source).substr(start,col-start));
        auto count=std::min<std::size_t>(std::max(1,d.location.length),std::max<std::size_t>(1,end-col));
        auto marked=sanitize(std::string_view(source).substr(col,count));
        out<<"  "<<d.location.line<<" | "<<prefix<<shown<<(end<source.size()?"...":"")<<'\n';
        out<<std::string(std::to_string(d.location.line).size()+2,' ')<<" | "
           <<std::string(before.size()+std::string(prefix).size(),' ')<<'^'
           <<std::string(std::min<std::size_t>(std::max<std::size_t>(1,marked.size())-1,120),'~')<<'\n';
    }
    if(!d.hint.empty())out<<"  help: "<<sanitize(d.hint,true)<<'\n';
    for(const auto& n:d.notes)out<<"  note: "<<sanitize(n.message,true)<<" ("<<sanitize(n.location.file,true)<<':'<<n.location.line<<':'<<n.location.column<<")\n";
    for(const auto& n:d.frames)out<<"  at "<<sanitize(n.message,true)<<" ("<<sanitize(n.location.file,true)<<':'<<n.location.line<<':'<<n.location.column<<")\n";
    return out.str();
}
std::string format_diagnostics_json(const std::vector<Diagnostic>& values) {
    std::string out="{\"schemaVersion\":1,\"coordinateEncoding\":\"utf-8-bytes-1-based\",\"diagnostics\":[";
    bool first=true;for(const auto& d:values){if(!first)out+=',';first=false;out+=format_diagnostic(d,DiagnosticFormat::Json);}
    return out+"]}\n";
}
Diagnostic runtime_diagnostic(Location at,std::string message) {
    std::string code="BLT3000";
    if(message.find("division by zero")!=message.npos||message.find("remainder by zero")!=message.npos)code="BLT3001";
    else if(message.find("index out of range")!=message.npos)code="BLT3002";
    else if(message.find("null pointer")!=message.npos)code="BLT3003";
    else if(message.find("overflow")!=message.npos||message.find("out of range")!=message.npos)code="BLT3004";
    else if(message.find("dangling")!=message.npos||message.find("expired")!=message.npos)code="BLT3005";
    return Error(std::move(at),message,code).diagnostic;
}
Diagnostic tooling_diagnostic(std::string message) { return Error({"<bolt>",1,1},message,"BLT4000").diagnostic; }
} // namespace bolt
