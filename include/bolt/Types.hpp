#pragma once
#include <string>
#include <string_view>
#include <vector>
namespace bolt {
    inline const std::vector<std::string_view>& builtin_concept_names() {
        static const std::vector<std::string_view> names={"Number","Integer","Integral","FloatingPoint","Signed","Unsigned","Addable","Subtractable","Multipliable","Dividable","Moduloable","Comparable","Equatable","Hashable","Pointer","Reference","Enum","Class","Struct","Iterable","Iterator","Constructible"};
        return names;
    }

    inline const std::vector<std::string_view>& builtin_attribute_names() {
        static const std::vector<std::string_view> names={"Reflect","ReadOnly","JsonName","JsonIgnore","Header","Native","NativeProperty","NativePack","SerializeValue","SerializeSequence","SerializeMap","StringableElements","SnapshotReceiver"};
        return names;
    }
    inline bool is_optional(std::string_view t) { return t.ends_with('?'); }
    inline bool is_reference(std::string_view t) {
        return t.ends_with('&');
    }
    inline bool is_pointer(std::string_view t) {
        return t.ends_with('*');
    }
    inline std::size_t array_rank(std::string_view t) {
        if(!t.ends_with(']'))return 0;
        const auto open=t.rfind('[');
        if(open==std::string_view::npos)return 0;
        for(auto i=open+1;i+1<t.size();++i)if(t[i]!=',')return 0;
        return t.size()-open-1;
    }
    inline bool is_array(std::string_view t) {
        return array_rank(t)!=0;
    }
    inline std::string array_suffix(std::size_t rank) {
        return "["+std::string(rank-1,',')+"]";
    }
    inline std::string referred(std::string_view t) {
        return is_reference(t) ? std::string(t.substr(0, t.size()-1)) : std::string(t);
    }
    inline std::string element_type(std::string_view t) {
        return std::string(t.substr(0, is_array(t) ? t.rfind('[') : t.size()-1));
    }
    // Only the final segment owns these arguments: Outer<int>.Inner<T>.
    inline std::size_t generic_suffix(std::string_view t) {
        if(!t.ends_with('>'))return std::string_view::npos;
        int depth=0;
        for(std::size_t i=t.size();i>0;--i) {
            if(t[i-1]=='>')++depth;
            else if(t[i-1]=='<'&&--depth==0)return i-1;
        }
        return std::string_view::npos;
    }
    inline std::string generic_base(std::string_view t) {
        return std::string(t.substr(0,generic_suffix(t)));
    }
    inline std::vector<std::string> generic_args(std::string_view t) {
        std::vector<std::string> out;
        const auto begin = generic_suffix(t);
        if (begin == std::string_view::npos || !t.ends_with('>')) return out;
        std::size_t start = begin+1;
        int depth = 0, brackets = 0;
        for (std::size_t i = start; i+1 < t.size(); ++i) {
            if (t[i]=='<') ++depth;
            if (t[i]=='>') --depth;
            if (t[i]=='[') ++brackets;
            if (t[i]==']') --brackets;
            if (t[i]==',' && depth==0 && brackets==0) {
                out.emplace_back(t.substr(start,i-start));
                start=i+1;
            }
        }
        if (start+1<t.size()) out.emplace_back(t.substr(start,t.size()-1-start));
        return out;
    }
    inline std::string generic_name(const std::string& base, const std::vector<std::string>& args) {
        if (args.empty()) return base;
        std::string s=base+"<";
        for (const auto& a:args) {
            if(s.back()!='<') s+=",";
            s+=a;
        }
        return s+">";
    }
    inline std::string generic_declaration_name(std::string_view t) {
        std::string result;int depth=0;
        for(char c:t) {
            if(c=='<')++depth;
            else if(c=='>')--depth;
            else if(depth==0)result+=c;
        }
        return result;
    }
    inline int action_arity(std::string_view t) {
        const auto base=generic_base(t);
        return base=="System.Action"?static_cast<int>(generic_args(t).size()):-1;
    }
    inline bool capture_cell_type(std::string_view t) { return t.starts_with("Bolt.Runtime.ICaptureCell<"); }
    inline bool function_type(std::string_view t) { return t.starts_with("System.Func<"); }
    inline std::string collection_kind(std::string_view t) {
        const auto base=generic_base(t);
        if (base == "Bolt.Runtime.Buffer") return "List";
        return {
        };
    }
    inline bool is_collection(std::string_view t) {
        return !collection_kind(t).empty() && !generic_args(t).empty();
    }
    inline bool is_integer(std::string_view t) {
        return t=="byte" || t=="short" || t=="ushort" || t=="int" || t=="uint" || t=="long" || t=="ulong";
    }
    inline bool is_unsigned(std::string_view t) {
        return t=="byte" || t=="ushort" || t=="uint" || t=="ulong";
    }
    inline bool is_floating(std::string_view t) {
        return t=="float" || t=="double";
    }
    inline bool is_numeric(std::string_view t) {
        return is_integer(t) || is_floating(t);
    }
    inline bool is_primitive(std::string_view t) {
        return is_numeric(t) || t=="bool" || t=="char" || t=="string";
    }
    inline int bits(std::string_view t) {
        if(t=="byte") return 8;
        if(t=="short"||t=="ushort") return 16;
        if(t=="long"||t=="ulong"||t=="double") return 64;
        return 32;
    }
    inline std::string promoted(std::string_view t) {
        return is_integer(t) && bits(t)<32 ? "int" : std::string(t);
    }
    inline std::string common_numeric(std::string_view a, std::string_view b) {
        if (!is_numeric(a)||!is_numeric(b)) return {
        };
        if (a=="double"||b=="double") return "double";
        if(a=="float"||b=="float") return "float";
        const auto x=promoted(a), y=promoted(b);
        if(is_unsigned(x)==is_unsigned(y)) {
            const int n=bits(x)>bits(y)?bits(x):bits(y);
            return is_unsigned(x)?(n==64?"ulong":"uint"):(n==64?"long":"int");
        }
        if(x=="ulong"||y=="ulong") return {
        };
        return "long";
    }
    inline bool assignable(std::string_view target,std::string_view from) {
        return target==from || (is_numeric(target)&&is_numeric(from)) || (is_pointer(target)&&from=="null");
    }
    // Single registry used by name resolution AND editor import completion.
    inline const std::vector<std::string_view>& builtin_import_names() {
        static const std::vector<std::string_view> names = {"Console","Runtime","Math","Random","Type","Arena","System.Collections.Generic","Ecs","System","System.IO","System.Diagnostics","System.Reflection","System.Text.Json","System.Threading.Tasks","File","Directory","Path","Stream","FileStream","MemoryStream","StreamReader","StreamWriter","BinaryReader","BinaryWriter","Json","Converter","DateTime","TimeSpan","Stopwatch","Task","Reflection","System.Memory","Owner"};
        return names;
    }
    inline bool is_builtin_import(std::string_view n) {
        for (auto name : builtin_import_names()) if (n == name) return true;
        return false;
    }
}
// namespace bolt
