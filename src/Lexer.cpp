#include <bolt/Frontend.hpp>
#include <algorithm>
#include <cctype>
#include <bolt/Unicode.hpp>
#include <functional>
#include <array>
namespace bolt {
    std::vector<Token> tokenize(std::string_view source, const std::string& filename) {
        static thread_local unsigned nested=0;
        struct Nesting { unsigned& depth; explicit Nesting(unsigned& d):depth(d){++depth;} ~Nesting(){--depth;} } nesting(nested);
        if(nested>128)throw Error({filename,1,1},"interpolated string nesting limit exceeded", "BLT1005");
        std::vector<Token> out;
        out.reserve(std::min<std::size_t>(source.size() / 5 + 1, 4096));
        std::size_t i = 0;
        int line = 1, column = 1;
        const auto current = [&]() -> char {
            return i < source.size() ? source[i] : '\0';
        };
        const auto next = [&]() -> char {
            return i + 1 < source.size() ? source[i + 1] : '\0';
        };
        const auto take = [&]() -> char {
            const char c = source[i++];
            if (c == '\n') {
                ++line;
                column = 1;
            } else ++column;
            return c;
        };
        const auto alpha = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        };
        const auto digit = [](char c) {
            return c >= '0' && c <= '9';
        };
        // A UTF-8 BOM is accepted; identifiers intentionally remain ASCII.
        if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF && static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) { i = 3; column = 4; }
        while (i < source.size()) {
            if (std::isspace(static_cast<unsigned char>(current()))) {
                take();
                continue;
            }
            if (current() == '/' && next() == '/') {
                while (i < source.size() && current() != '\n') take();
                continue;
            }
            Location at {
                filename, line, column, 1, i
            };
            if (current() == '/' && next() == '*') {
                take();
                take();
                while (i < source.size() && !(current() == '*' && next() == '/')) take();
                if (i == source.size()) throw Error(at, "unterminated block comment", "BLT1003");
                take();
                take();
                continue;
            }
            if (alpha(current())) {
                do { take(); } while (alpha(current()) || digit(current()));
                at.length = static_cast<int>(i - at.offset);
                out.push_back( {
                    Token::Kind::Identifier, std::string(source.substr(at.offset, i - at.offset)), std::move(at)
                });
                continue;
            }
            if (digit(current()) || (current() == '.' && digit(next()))) {
                while (digit(current())) take();
                if (current() == '.' && !alpha(next())) {
                    take();
                    while (digit(current())) take();
                }
                if (current() == 'e' || current() == 'E') {
                    take();
                    if (current() == '+' || current() == '-') take();
                    if (!digit(current())) throw Error(at, "expected exponent digits", "BLT1004");
                    while (digit(current())) take();
                }
                while (alpha(current()) || digit(current())) take();
                at.length = static_cast<int>(i - at.offset);
                out.push_back( {
                    Token::Kind::Number, std::string(source.substr(at.offset, i - at.offset)), std::move(at)
                });
                continue;
            }
            if (current() == '$' && next() == '"') {
                // Interpolation lowers to ordinary string casts/concatenation. Every
                // expression token retains its original .bolt byte and line position.
                std::function<std::size_t(std::size_t)> hole_end, quoted_end, interpolated_end;
                auto bad=[&](const std::string& message)->std::size_t { throw Error(at,message,"BLT1005"); };
                quoted_end=[&](std::size_t p) {
                    ++p;
                    while(p<source.size()) {
                        if(source[p]=='\\') { p+=2; continue; }
                        if(source[p]=='"')return p+1;
                        ++p;
                    }
                    return bad("unterminated quoted string in interpolation");
                };
                unsigned scan_depth=0;
                hole_end=[&](std::size_t p) {
                    if(++scan_depth>128)return bad("interpolation nesting limit exceeded");
                    unsigned braces=1;
                    for(;p<source.size();) {
                        if(source.substr(p,2)=="//") {p+=2;while(p<source.size()&&source[p]!='\n')++p;continue;}
                        if(source.substr(p,2)=="/*") {auto end=source.find("*/",p+2);if(end==source.npos)return bad("unterminated block comment in interpolation");p=end+2;continue;}
                        if(source.substr(p,2)=="$\"") {p=interpolated_end(p);continue;}
                        if(source[p]=='"') {p=quoted_end(p);continue;}
                        if(source[p]=='\''){++p;while(p<source.size()){if(source[p]=='\\'){p+=2;continue;}if(source[p++]=='\'')break;}continue;}
                        if(source[p]=='{')++braces;
                        if(source[p]=='}'&&--braces==0){--scan_depth;return p;}
                        ++p;
                    }
                    return bad("unterminated interpolation; expected '}'");
                };
                interpolated_end=[&](std::size_t p) {
                    p+=2;
                    while(p<source.size()) {
                        if(source[p]=='\\'){p+=2;continue;}
                        if(source[p]=='"')return p+1;
                        if(source.substr(p,2)=="{{"||source.substr(p,2)=="}}"){p+=2;continue;}
                        if(source[p]=='{'){p=hole_end(p+1)+1;continue;}
                        ++p;
                    }
                    return bad("unterminated nested interpolated string");
                };
                auto symbol=[&](const std::string& text,Location where) {where.length=1;out.push_back({Token::Kind::Symbol,text,where});};
                auto here=[&]() {return Location{filename,line,column,1,i};};
                take();take();
                symbol("(",at);
                std::string part;
                Location part_at=here();
                auto flush=[&]() {auto loc=part_at;loc.length=static_cast<int>(i-part_at.offset);if(loc.length<1)loc.length=1;out.push_back({Token::Kind::String,std::move(part),loc});part.clear();};
                bool closed=false;
                while(i<source.size()) {
                    if(current()=='"') {flush();take();symbol(")",at);closed=true;break;}
                    if(current()=='\n'||current()=='\r')bad("newline in interpolated string text; use \\n");
                    if(current()=='{'&&next()=='{'){take();take();part+='{' ;continue;}
                    if(current()=='}'&&next()=='}'){take();take();part+='}' ;continue;}
                    if(current()=='}')bad("unmatched '}' in interpolated string; write '}}' for a literal brace");
                    if(current()=='{') {
                        flush();auto open=here();take();const auto start=here();
                        const auto end=hole_end(i);
                        symbol("+",open);
                        out.push_back({Token::Kind::Identifier,"string",open});symbol("(",open);
                        std::vector<Token> inner;
                        try { inner=tokenize(source.substr(i,end-i),filename); }
                        catch(const Error& error) {
                            auto diagnostic=error.diagnostic;
                            auto remap=[&](Location& loc) {
                                loc.offset+=start.offset;
                                if(loc.line==1)loc.column+=start.column-1;
                                loc.line+=start.line-1;
                            };
                            remap(diagnostic.location);
                            for(auto& note:diagnostic.notes)remap(note.location);
                            for(auto& frame:diagnostic.frames)remap(frame.location);
                            attach_source(diagnostic,source);
                            throw Error(std::move(diagnostic));
                        }
                        if(inner.size()==1)throw Error(open,"interpolation requires an expression between '{' and '}'", "BLT1005");
                        inner.pop_back();
                        for(auto& token:inner) {
                            token.at.offset+=start.offset;
                            if(token.at.line==1)token.at.column+=start.column-1;
                            token.at.line+=start.line-1;
                            out.push_back(std::move(token));
                        }
                        while(i<=end)take();
                        symbol(")",open);symbol("+",open);part_at=here();continue;
                    }
                    char c=take();
                    if(c=='\\') {
                        if(i==source.size())bad("unterminated interpolated string escape");
                        c=take();
                        switch(c) {
                        case 'n':part+='\n';break;case 'r':part+='\r';break;case 't':part+='\t';break;
                        case '0':part+='\0';break;case '\\':part+='\\';break;case '"':part+='"';break;
                        default:bad("unsupported interpolated string escape");
                        }
                    }else part+=c;
                }
                if(!closed)bad("unterminated interpolated string");
                continue;
            }
            if(current()=='\'') {
                take();std::string text;
                while(i<source.size()&&current()!='\'') {
                    if(current()=='\n'||current()=='\r')throw Error(at,"newline in character literal","BLT1006");
                    char c=take();
                    if(c=='\\') {
                        if(i==source.size())throw Error(at,"unterminated character escape","BLT1006");
                        c=take();switch(c){
                        case 'n':text+='\n';break;case 'r':text+='\r';break;case 't':text+='\t';break;case '0':text+='\0';break;
                        case '\\':text+='\\';break;case '\'':text+='\'';break;case '"':text+='"';break;
                        case 'u':case 'U':{
                            const unsigned digits=c=='u'?4:8;char32_t value=0;
                            for(unsigned n=0;n<digits;++n){if(i==source.size())throw Error(at,"incomplete Unicode escape","BLT1006");auto d=take();unsigned x=d>='0'&&d<='9'?d-'0':d>='a'&&d<='f'?d-'a'+10:d>='A'&&d<='F'?d-'A'+10:99;
                                if(x>15)throw Error(at,"Unicode escapes require hexadecimal digits","BLT1006");value=(value<<4)|x;}
                            if(!unicode::scalar(value))throw Error(at,"character must be a Unicode scalar, not a surrogate","BLT1006");text+=unicode::encode(value);break;
                        }
                        default:throw Error(at,"unsupported character escape","BLT1006");}
                    }else text+=c;
                }
                if(i==source.size())throw Error(at,"unterminated character literal","BLT1006");take();
                try{if(unicode::length(text)!=1)throw Error(at,"a char literal must contain exactly one Unicode scalar","BLT1006");}
                catch(const std::invalid_argument& e){throw Error(at,e.what(),"BLT1006");}
                at.length=static_cast<int>(i-at.offset);out.push_back({Token::Kind::Character,std::move(text),at});continue;
            }
            if (current() == '"') {
                take();
                std::string s;
                while (i < source.size() && current() != '"') {
                    if (current() == '\n' || current() == '\r') throw Error(at, "newline in string literal; use \\n", "BLT1002");
                    char c = take();
                    if (c == '\\') {
                        if (i == source.size()) throw Error(at, "unterminated string escape", "BLT1002");
                        c = take();
                        switch (c) {
                            case 'n': s += '\n';
                            break;
                            case 'r': s += '\r';
                            break;
                            case 't': s += '\t';
                            break;
                            case '0': s += '\0';
                            break;
                            case '\\': s += '\\';
                            break;
                            case '"': s += '"';
                            break;
                            default: throw Error(at, "unsupported string escape", "BLT1002");
                        }
                    } else s += c;
                }
                if (i == source.size()) throw Error(at, "unterminated string literal", "BLT1002");
                take();
                at.length = static_cast<int>(i - at.offset);
                out.push_back( {
                    Token::Kind::String, std::move(s), std::move(at)
                });
                continue;
            }
            static constexpr auto pairs = [] {
                std::array<std::array<bool, 128>, 128> table{};
                for (std::string_view pair : {"<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=",
                                              "++", "--", "->", "=>", "::", "<<", ">>", "&=", "|=", "^=", "??"})
                    table[static_cast<unsigned char>(pair[0])][static_cast<unsigned char>(pair[1])] = true;
                return table;
            }();
            if(source.substr(i,3)=="<<="||source.substr(i,3)==">>="||source.substr(i,3)=="?" "?=") {
                auto op=std::string(source.substr(i,3));take();take();take();at.length=3;out.push_back({Token::Kind::Symbol,std::move(op),std::move(at)});continue;
            }
            if (source.substr(i, 3) == "...") {
                take();
                take();
                take();
                at.length = static_cast<int>(i - at.offset);
                out.push_back( {
                    Token::Kind::Symbol, "...", std::move(at)
                });
                continue;
            }
            const auto first = static_cast<unsigned char>(current());
            const auto second = static_cast<unsigned char>(next());
            if (first < 128 && second < 128 && pairs[first][second]) {
                std::string pair(source.substr(i, 2));
                take();
                take();
                at.length = 2;
                out.push_back({Token::Kind::Symbol, std::move(pair), std::move(at)});
                continue;
            }
            const char c = take();
            if (std::string_view("{}[]&|^~?();,:.+-*/%!=<>").find(c) == std::string_view::npos) throw Error(at, "unexpected character '" + std::string(1, c) + "'", "BLT1001");
            at.length = static_cast<int>(i - at.offset);
            out.push_back( {
                Token::Kind::Symbol, std::string(1, c), std::move(at)
            });
        }
        out.push_back( {
            Token::Kind::End, "<end>", {
                filename, line, column, 1, i
            }
        });
        return out;
    }
}
// namespace bolt
