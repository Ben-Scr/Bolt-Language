#include <bolt/native/IR.hpp>
#include <bolt/native/ABI.hpp>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <string_view>
#include <utility>
#include <set>
#include <sstream>
#include <stdexcept>

namespace bolt::ir {
unsigned width(Type t) { switch(t) {case Type::Void:return 0; case Type::I8:return 1;case Type::I16:return 2;case Type::I32:case Type::F32:return 4;default:return 8;} }
bool floating(Type t) {return t==Type::F32||t==Type::F64;}
const char* name(Type t) {switch(t){case Type::Void:return "void";case Type::I8:return "i8";case Type::I16:return "i16";case Type::I32:return "i32";case Type::I64:return "i64";case Type::F32:return "float";case Type::F64:return "double";case Type::Ptr:return "ptr";}throw std::runtime_error("invalid IR type");}
void verify(const Module& m) {
    std::set<std::string> symbols;
    for(const auto& g:m.globals) {
        if(!symbols.insert(g.name).second)throw std::runtime_error("duplicate IR global: "+g.name);
        for(const auto& r:g.relocations)if(r.offset+8>g.bytes.size())throw std::runtime_error("IR relocation outside global");
    }
    for(const auto& e:m.externals)if(!symbols.insert(e.name).second)throw std::runtime_error("duplicate IR external: "+e.name);
    for(const auto& f:m.functions)if(!symbols.insert(f.name).second)throw std::runtime_error("duplicate IR function: "+f.name);
    for(const auto& f:m.functions) {
        if(f.blocks.empty())throw std::runtime_error("IR function has no entry block: "+f.name);
        std::vector<bool> defined(f.registers.size());
        for(std::size_t i=0;i<f.parameters.size();++i){if(i+1>=f.registers.size()||f.registers[i+1]!=f.parameters[i])throw std::runtime_error("invalid IR parameter");defined[i+1]=true;}
        for(const auto& b:f.blocks) {
            if(b.code.empty())throw std::runtime_error("empty IR block in "+f.name);
            for(std::size_t j=0;j<b.code.size();++j) {
                const auto& i=b.code[j];
                auto term=i.op==Op::Jump||i.op==Op::Branch||i.op==Op::Return;
                if(term!=(j+1==b.code.size()))throw std::runtime_error("IR terminator placement in "+f.name);
                if((i.op==Op::Jump||i.op==Op::Branch)&&i.yes>=f.blocks.size())throw std::runtime_error("invalid IR branch");
                if(i.op==Op::Branch&&i.no>=f.blocks.size())throw std::runtime_error("invalid IR branch");
                for(auto a:i.args)if(a==0||a>=f.registers.size())throw std::runtime_error("invalid IR operand in "+f.name);
                if (i.storage_move_stride) {
                    const auto count=i.args.size();
                    const bool hosted=i.text=="br_memory_move" && count==5;
                    const bool source_move=m.runtime_free() && count==4 && i.text=="b_Bolt_2eRuntime_2eMemoryOps_3a_3aMove_28byte_2a_2cconst_20byte_2a_2culong_2c_29";
                    if (!std::has_single_bit(i.storage_move_stride) || i.storage_move_stride>8 ||
                        i.op!=Op::Call || i.result || (!hosted && !source_move) ||
                        f.registers[i.args[count-3]]!=Type::Ptr || f.registers[i.args[count-2]]!=Type::Ptr ||
                        f.registers[i.args.back()]!=Type::I64)
                        throw std::runtime_error("invalid IR storage move in "+f.name);
                }
                if ((i.op==Op::AtomicCompareExchange || i.op==Op::AtomicAdd) &&
                    (i.args.size()!=(i.op==Op::AtomicAdd?2u:3u) || !i.result || i.result>=f.registers.size() ||
                     f.registers[i.result]!=Type::I64 || f.registers[i.args[0]]!=Type::Ptr ||
                     f.registers[i.args[1]]!=Type::I64 || f.registers[i.args.back()]!=Type::I64))
                    throw std::runtime_error("invalid atomic word operation in "+f.name);
                if (i.op==Op::CopyElement && (i.args.size()!=2 || i.result || i.overflow ||
                    !i.immediate || i.immediate>128 || f.registers[i.args[0]]!=Type::Ptr ||
                    f.registers[i.args[1]]!=Type::Ptr))
                    throw std::runtime_error("invalid IR element copy in "+f.name);
                for(auto r:{i.result,i.overflow})if(r){if(r>=defined.size()||defined[r])throw std::runtime_error("duplicate IR register definition");defined[r]=true;}
                if((i.op==Op::Call||i.op==Op::Address)&&!symbols.contains(i.text))throw std::runtime_error("unknown IR symbol: "+i.text);
            }
        }
        for(std::size_t r=1;r<defined.size();++r)if(!defined[r])throw std::runtime_error("undefined IR register "+std::to_string(r)+" in "+f.name);
    }
}
std::string dump(const Module& m) { return llvm_text(m,"",false); }
namespace {
std::string q(std::string_view bytes) {
    constexpr char hex[] = "0123456789abcdef";
    std::string quoted;
    quoted.reserve(bytes.size() + 2);
    quoted.push_back('"');
    for (const unsigned char byte : bytes) {
        if (byte >= 32 && byte < 127 && byte != '"' && byte != '\\') {
            quoted.push_back(static_cast<char>(byte));
        } else {
            quoted.push_back('\\');
            quoted.push_back(hex[byte >> 4]);
            quoted.push_back(hex[byte & 15]);
        }
    }
    quoted.push_back('"');
    return quoted;
}
std::string r(Reg v){return "%r"+std::to_string(v);}
std::string sym(const std::string& s){return "@"+s;}
std::string signature(Type result,const std::vector<Type>& args){std::string s=name(result);s+=" (";for(std::size_t j=0;j<args.size();++j){if(j)s+=", ";s+=name(args[j]);}return s+")";}
#include "StorageShift.inc"
// In a short in-place shift the source and destination usually overlap. Two
// loads from the ends cover the interval, as long as both precede either store.
// The largest case uses four baseline x86-64 vector registers per half.
void emit_short_storage_move(std::ostream& out, unsigned stride, const std::string& fallback) {
    out << "define internal void @bolt.storage.move." << stride << "(ptr %ctx, ptr %dst, ptr %src, i64 %n, i64 %delta) alwaysinline {\n"
        << "entry:\n  %short = icmp ule i64 %n, 128\n"
        << "  br i1 %short, label %small, label %large\n"
        << "large:\n  %flags.addr = getelementptr i8, ptr %ctx, i64 " << offsetof(BrContext,flags) << "\n"
        << "  %flags = load i32, ptr %flags.addr, align 4\n"
        << "  %bit = and i32 %flags, " << BrContext::Avx2 << "\n"
        << "  %avx = icmp ne i32 %bit, 0\n"
        << "  %medium = icmp ule i64 %n, 262144\n"
        << "  %use = and i1 %avx, %medium\n"
        << "  br i1 %use, label %shift, label %bulk\n"
        << "shift:\n  %up = icmp sgt i64 %delta, 0\n"
        << "  br i1 %up, label %right, label %left\n";
    for (bool right : {false,true}) {
        const std::string side=right ? "right" : "left";
        out << side << ":\n";
        if (stride>=4) {
            out << "  %zbit." << side << " = and i32 %flags, " << BrContext::Avx512F << "\n"
                << "  %zmm." << side << " = icmp ne i32 %zbit." << side << ", 0\n"
                << "  %enough." << side << " = icmp uge i64 %n, 4096\n"
                << "  %sizeok." << side << " = and i1 %enough." << side << ", %zmm." << side << "\n"
                << "  %adjacent." << side << " = icmp eq i64 %delta, " << (right ? int(stride) : -int(stride)) << "\n"
                << "  %wide." << side << " = and i1 %sizeok." << side << ", %adjacent." << side << "\n"
                << "  br i1 %wide." << side << ", label %cacheline." << side << ", label %vector." << side << "\n"
                << "cacheline." << side << ":\n  call fastcc void @bolt.bulk." << side << "." << stride << "(ptr %dst, i64 %n)\n  ret void\n"
                << "vector." << side << ":\n";
        }
        out << "  call fastcc void @bolt.shift." << side << "(ptr %dst, ptr %src, i64 %n)\n  ret void\n";
    }
    out << "bulk:\n";
    if (fallback.empty()) out << "  call void @llvm.memmove.p0.p0.i64(ptr %dst, ptr %src, i64 %n, i1 false)\n";
    else out << "  call void @" << fallback << "(ptr %ctx, ptr %dst, ptr %src, i64 %n)\n";
    out << "  ret void\n";
    std::vector<unsigned> limits{0};
    for (unsigned n=stride; n<=128; n*=2) limits.push_back(n);
    auto cases = [&](auto&& self, std::size_t first, std::size_t end, const std::string& label) -> void {
        out << label << ":\n";
        if (end-first>1) {
            const auto mid=first+(end-first)/2;
            const auto limit=limits[mid-1];
            const auto id=std::to_string(limit);
            out << "  %at" << id << " = icmp ule i64 %n, " << limit << "\n"
                << "  br i1 %at" << id << ", label %low" << id << ", label %high" << id << "\n";
            self(self,first,mid,"low"+id);
            self(self,mid,end,"high"+id);
            return;
        }
        const auto limit=limits[first];
        // In the first two nonempty buckets there is only one possible length.
        if (limit<=2*stride) {
            if (limit) out << "  %v" << limit << " = load i" << limit*8 << ", ptr %src, align 1\n"
                           << "  store i" << limit*8 << " %v" << limit << ", ptr %dst, align 1\n";
            out << "  ret void\n";
            return;
        }
        const unsigned bytes=limit/2;
        const auto type=bytes<16 ? "i"+std::to_string(bytes*8) : "<"+std::to_string(bytes/8)+" x i64>";
        const auto id=std::to_string(limit);
        out << "  %off" << id << " = sub i64 %n, " << bytes << "\n"
            << "  %from" << id << " = getelementptr i8, ptr %src, i64 %off" << id << "\n"
            << "  %to" << id << " = getelementptr i8, ptr %dst, i64 %off" << id << "\n"
            << "  %first" << id << " = load " << type << ", ptr %src, align 1\n"
            << "  %last" << id << " = load " << type << ", ptr %from" << id << ", align 1\n"
            << "  store " << type << " %first" << id << ", ptr %dst, align 1\n"
            << "  store " << type << " %last" << id << ", ptr %to" << id << ", align 1\n  ret void\n";
    };
    cases(cases,0,limits.size(),"small");
    out << "}\n";
}
// Preserve the known displacement across LLVM's reassociation of indexed
// addresses. Single edits have an adjacent source and destination in this block.
int known_storage_displacement(const Block& block, std::size_t index) {
    const auto& move = block.code[index];
    const auto count=move.args.size();
    const Reg dest = move.args[count-3], source = move.args[count-2];
    while (index) {
        const auto& def = block.code[--index];
        if (def.result != dest && def.result != source) continue;
        if (def.op == Op::Offset && def.args.size() == 1 && def.immediate == move.storage_move_stride) {
            if (def.result == dest && def.args[0] == source) return move.storage_move_stride;
            if (def.result == source && def.args[0] == dest) return -int(move.storage_move_stride);
        }
        break;
    }
    return 0;
}
// Expose successful byte operations to LLVM on targets that link libc directly.
// Exceptional inputs still go through BoltRuntime for the original exception
// class, source location, and first-error ordering.
static void emit_memory_wrapper(std::ostream& out, const std::string& op) {
    const bool set = op == "set";
    const bool compare = op == "compare";
    const std::string result = compare ? "i32" : "void";
    const std::string done = compare ? "ret i32 0" : "ret void";
    const std::string second = set ? "i8 %value" : "ptr %b";
    const std::string args = "ptr %ctx, ptr %site, ptr %a, " + second + ", i64 %n";

    out << "define internal " << result << " @bolt.memory." << op << "(" << args << ") alwaysinline {\n"
        << R"(entry:
  %empty = icmp eq i64 %n, 0
  %noctx = icmp eq ptr %ctx, null
  %skip = or i1 %empty, %noctx
  br i1 %skip, label %done, label %state
state:
  %status = load i32, ptr %ctx, align 1
  %failed = icmp ne i32 %status, 0
  br i1 %failed, label %done, label %range
range:
  %lastbase = sub i64 0, %n
  %ai = ptrtoint ptr %a to i64
  %anull = icmp eq ptr %a, null
  %aoverflow = icmp ugt i64 %ai, %lastbase
  %bada = or i1 %anull, %aoverflow
)";
    if (set) {
        out << "  br i1 %bada, label %error, label %apply\n";
    } else {
        out << R"(  %bi = ptrtoint ptr %b to i64
  %bnull = icmp eq ptr %b, null
  %boverflow = icmp ugt i64 %bi, %lastbase
  %badb = or i1 %bnull, %boverflow
  %invalid = or i1 %bada, %badb
)";
        if (op == "copy") {
            out << R"(  %above = icmp uge i64 %ai, %bi
  %ab = sub i64 %ai, %bi
  %ba = sub i64 %bi, %ai
  %distance = select i1 %above, i64 %ab, i64 %ba
  %overlap = icmp ult i64 %distance, %n
  %badcopy = or i1 %invalid, %overlap
  br i1 %badcopy, label %error, label %apply
)";
        } else {
            out << "  br i1 %invalid, label %error, label %apply\n";
        }
    }
    out << "apply:\n";
    if (compare) {
        out << R"(  %raw = call i32 @memcmp(ptr %a, ptr %b, i64 %n)
  %neg = icmp slt i32 %raw, 0
  %pos = icmp sgt i32 %raw, 0
  %positive = zext i1 %pos to i32
  %sign = select i1 %neg, i32 -1, i32 %positive
  ret i32 %sign
)";
    } else {
        const char* intrinsic = set ? "memset.p0.i64" :
                                op == "copy" ? "memcpy.p0.p0.i64" : "memmove.p0.p0.i64";
        out << "  call void @llvm." << intrinsic << "(ptr %a, " << second << ", i64 %n, i1 false)\n"
            << "  ret void\n";
    }
    out << "error:\n  " << (compare ? "%error_result = " : "") << "call " << result
        << " @br_memory_" << op << "(" << args << ") cold\n"
        << "  %error_status = load i32, ptr %ctx, align 1\n"
        << "  %has_error = icmp ne i32 %error_status, 0\n"
        << "  call void @llvm.assume(i1 %has_error)\n  "
        << done << "\ndone:\n  " << done << "\n}\n";
}

}
std::string llvm_text(const Module& m,const std::string& triple,bool debug,bool release) {
    verify(m);
    std::ostringstream o;
    const bool windows_freestanding=m.runtime_free() && triple.find("windows")!=std::string::npos;
    const bool captures_stack = std::any_of(m.externals.begin(), m.externals.end(), [](const External& e) {
        return e.name == "RtlCaptureContext" || e.name == "RtlCaptureStackBackTrace" || e.name == "backtrace" || e.name == "bolt_diagnostic_backtrace";
    });
    const bool windows_x64_probe=windows_freestanding && triple.starts_with("x86_64");
    o<<"; Bolt native IR. No generated C/C++ is used by this pipeline.\n";
    if(!triple.empty())o<<"target triple = "<<q(triple)<<"\n";
    if(windows_x64_probe){
        // A Win64 prolog passes its frame size in RAX. Touch every guard page
        // before RSP moves. R10 and R11 are the ABI's two scratch registers.
        o<<"module asm "<<q(R"(
            .text
            .p2align 4
            .globl bolt_stack_probe
        bolt_stack_probe:
            leaq 8(%rsp), %r10
            movq %rax, %r11
        1:
            cmpq $4096, %r11
            jb 2f
            subq $4096, %r10
            testb $0, (%r10)
            subq $4096, %r11
            jmp 1b
        2:
            subq %r11, %r10
            testb $0, (%r10)
            retq
        )")<<"\n";
    }
    if(triple.find("windows-msvc")!=std::string::npos){
        // LLVM emits this CRT marker for floating-point code on Windows. Bolt
        // links with /NODEFAULTLIB and uses BoltRuntime for runtime services.
        // Weak ODR keeps the definition through optimization while COMDAT lets
        // separately emitted objects share it without duplicate definitions.
        o<<"$_fltused = comdat any\n@_fltused = weak_odr global i32 0, comdat, align 4\n";
    }
    for(const auto& g:m.globals){
        const std::string_view bytes(reinterpret_cast<const char*>(g.bytes.data()), g.bytes.size());
        if(g.relocations.empty()) {
            o<<sym(g.name)<<" = internal "<<(g.writable?"global":"constant")<<" ["<<g.bytes.size()<<" x i8] c";
            o<<q(bytes)<<", align "<<g.alignment<<"\n";
        } else { // Pointer relocations are only used in descriptor/function tables.
            o<<sym(g.name)<<" = internal "<<(g.writable?"global":"constant")<<" <{ ";
            std::size_t pos=0;bool first=true;
            for(const auto& rel:g.relocations){if(rel.offset<pos)throw std::runtime_error("overlapping IR relocations");if(rel.offset>pos){if(!first)o<<", ";o<<"["<<rel.offset-pos<<" x i8]";first=false;}if(!first)o<<", ";o<<"ptr";first=false;pos=rel.offset+8;}
            if(pos<g.bytes.size()){if(!first)o<<", ";o<<"["<<g.bytes.size()-pos<<" x i8]";}
            o<<" }> <{ ";pos=0;first=true;
            for(const auto& rel:g.relocations){if(rel.offset>pos){if(!first)o<<", ";o<<"["<<rel.offset-pos<<" x i8] c"<<q(bytes.substr(pos, rel.offset - pos));first=false;}if(!first)o<<", ";o<<"ptr ";if(rel.addend)o<<"getelementptr (i8, ptr "<<sym(rel.symbol)<<", i64 "<<rel.addend<<")";else o<<sym(rel.symbol);first=false;pos=rel.offset+8;}
            if(pos<g.bytes.size()){if(!first)o<<", ";o<<"["<<g.bytes.size()-pos<<" x i8] c"<<q(bytes.substr(pos));}o<<" }>, align "<<g.alignment<<"\n";
        }
    }
    // Match Clang's narrow C argument/return extension on Darwin. Bolt-to-Bolt
    // calls use their own consistently typed scalar IR.
    std::unordered_map<std::string_view,const External*> darwin_externals;
    const bool darwin=triple.find("apple")!=std::string::npos;
    auto extension=[](Type t,bool sign)->const char* {
        return t==Type::I8 || t==Type::I16 ? (sign?"signext ":"zeroext ") : "";
    };
    // POSIX open takes its mode through varargs. This affects the stack on
    // Apple ARM64 and the vector-register count on System V x86-64.
    const bool bsd=darwin || triple.find("freebsd")!=std::string::npos;
    auto open_varargs=[&](const External& e) {
        return bsd && e.foreign && e.name=="open" &&
               e.parameters==std::vector<Type>{Type::Ptr,Type::I32,Type::I32};
    };
    const External* posix_open=nullptr;
    for(const auto& e:m.externals){
        if(open_varargs(e))posix_open=&e;
        const bool c_abi=darwin && e.foreign;
        if(c_abi)darwin_externals.emplace(e.name,&e);
        o<<"declare "<<(c_abi?extension(e.result,e.signed_result):"")<<name(e.result)<<' '<<sym(e.name)<<'(';
        // On Apple ARM64 the optional fcntl argument travels on the stack.
        // Preserve the system prototype for the standalone POSIX handle calls.
        const bool fcntl_varargs=c_abi && e.name=="fcntl" && e.parameters.size()==3;
        const bool variadic=fcntl_varargs || &e==posix_open;
        const auto fixed=variadic ? std::size_t{2} : e.parameters.size();
        for(std::size_t j=0;j<fixed;++j){
            if(j)o<<", ";
            o<<name(e.parameters[j]);
            if(c_abi && (e.parameters[j]==Type::I8 || e.parameters[j]==Type::I16))o<<(e.signed_argument(j)?" signext":" zeroext");
        }
        if(variadic)o<<", ...";
        o<<")\n";
    }
    const bool budgets = release && m.instrument_budget;
    if (budgets) {
        static_assert(offsetof(BrContext, failed)==0 && offsetof(BrContext, flags)==4 &&
                      offsetof(BrContext, steps)==8 && offsetof(BrContext, depth)==16 &&
                      offsetof(BrContext, max_depth)==20 && BR_PRIVATE_STEPS==256);
        auto imports=[&](std::string_view n) {
            return std::any_of(m.externals.begin(),m.externals.end(),[&](const External& e){return e.name==n;});
        };
        if (imports("br_tick")) o << R"ir(
define internal void @bolt.budget.tick(ptr %c, ptr %site) alwaysinline {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %done, label %test
test:
  %f = load i32, ptr %c
  %fp = getelementptr i8, ptr %c, i64 4
  %flags = load i32, ptr %fp
  %enabled = and i32 %flags, 1
  %ok = icmp eq i32 %f, 0
  %limited = icmp ne i32 %enabled, 0
  %active = and i1 %ok, %limited
  br i1 %active, label %owner, label %done
owner:
  %private = and i32 %flags, 256
  %exclusive = icmp ne i32 %private, 0
  br i1 %exclusive, label %check, label %slow
check:
  %sp = getelementptr i8, ptr %c, i64 8
  %remaining = load i64, ptr %sp
  %empty = icmp eq i64 %remaining, 0
  br i1 %empty, label %slow, label %debit
debit:
  %next = sub nuw i64 %remaining, 1
  store i64 %next, ptr %sp
  br label %done
slow:
  call void @br_tick(ptr %c, ptr %site)
  br label %done
done:
  ret void
}
)ir";
        if (imports("br_enter")) o << R"ir(
define internal i32 @bolt.budget.enter(ptr %c, ptr %site) alwaysinline {
entry:
  %null = icmp eq ptr %c, null
  br i1 %null, label %skip, label %test
test:
  %f = load i32, ptr %c
  %ok = icmp eq i32 %f, 0
  br i1 %ok, label %check, label %skip
check:
  %dp = getelementptr i8, ptr %c, i64 16
  %lp = getelementptr i8, ptr %c, i64 20
  %depth = load i32, ptr %dp
  %limit = load i32, ptr %lp
  %bounded = icmp ne i32 %limit, 0
  %atlimit = icmp uge i32 %depth, %limit
  %full = and i1 %bounded, %atlimit
  br i1 %full, label %slow, label %advance
advance:
  %next = add i32 %depth, 1
  store i32 %next, ptr %dp
  ret i32 1
slow:
  %result = call i32 @br_enter(ptr %c, ptr %site)
  ret i32 %result
skip:
  ret i32 0
}
)ir";
        if (imports("br_leave")) o << R"ir(
define internal void @bolt.budget.leave(ptr %c, i32 %entered) alwaysinline {
entry:
  %valid = icmp ne ptr %c, null
  %active = icmp ne i32 %entered, 0
  %check = and i1 %active, %valid
  br i1 %check, label %test, label %done
test:
  %dp = getelementptr i8, ptr %c, i64 16
  %depth = load i32, ptr %dp
  %empty = icmp eq i32 %depth, 0
  br i1 %empty, label %done, label %leave
leave:
  %previous = sub nuw i32 %depth, 1
  store i32 %previous, ptr %dp
  br label %done
done:
  ret void
}
)ir";
    }
    std::set<std::string> intrinsic_declarations;
    std::set<std::string> memory_wrappers;
    std::set<unsigned> storage_move_strides;
    std::string storage_move_fallback;
    const bool direct_libc = triple.find("linux") != std::string::npos ||
                             triple.find("darwin") != std::string::npos ||
                             triple.find("apple") != std::string::npos;
    if (release && direct_libc) {
        const bool has_memcmp = std::any_of(m.externals.begin(), m.externals.end(),
            [](const External& e) { return e.name == "memcmp"; });
        for (const auto& e : m.externals) {
            if (e.foreign || !e.name.starts_with("br_memory_")) continue;
            const auto op = e.name.substr(10);
            if (op == "set") intrinsic_declarations.insert("declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)");
            else if (op == "move") intrinsic_declarations.insert("declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)");
            else if (op == "copy") intrinsic_declarations.insert("declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)");
            else if (op == "compare" && !has_memcmp)
                intrinsic_declarations.insert("declare i32 @memcmp(ptr, ptr, i64) nounwind willreturn memory(argmem: read)");
            else continue;
            intrinsic_declarations.insert("declare void @llvm.assume(i1 noundef)");
            memory_wrappers.insert(e.name);
            emit_memory_wrapper(o, op);
        }
    }
    std::vector<std::string> metadata;
    auto md=[&](const std::string& s){auto id=metadata.size();metadata.push_back(s);return id;};
    // LLVM may reuse the execution status across a successful internal
    // buffer store. An untagged user pointer or arbitrary call can still
    // access either object, so it invalidates the ordinary alias analysis.
    std::unordered_map<std::string,std::size_t> access_tags;
    std::size_t root_tag = 0;
    auto alias_tag = [&](const std::string& object) -> std::string {
        if (!release || object.empty()) return {};
        if (access_tags.empty()) root_tag = md("!{!\"Bolt runtime storage\"}");
        auto [tag, inserted] = access_tags.try_emplace(object,0);
        if (inserted) {
            const auto type = std::to_string(md("!{!" + q(object) + ", !" + std::to_string(root_tag) + "}"));
            tag->second = md("!{!" + type + ", !" + type + ", i64 0}");
        }
        return ", !tbaa !" + std::to_string(tag->second);
    };
    std::size_t compile_unit=0;
    if(debug){auto file=md("!DIFile(filename: \"Bolt\", directory: \".\")");compile_unit=md("distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !"+std::to_string(file)+", producer: \"Bolt 0.4 native\", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)");}
    std::string block_text;
    for(const auto& f:m.functions){
        o<<"\ndefine "<<(f.exported?"":"internal ")<<name(f.result)<<' '<<sym(f.name)<<'(';
        for(std::size_t j=0;j<f.parameters.size();++j){if(j)o<<", ";o<<name(f.parameters[j])<<' '<<r(static_cast<Reg>(j+1));}
        std::size_t scope=0;
        if(debug){auto file=md("!DIFile(filename: "+q(f.at.file.empty()?"<bolt>":f.at.file)+", directory: \".\")");auto params=md("!{}");auto type=md("!DISubroutineType(types: !"+std::to_string(params)+")");scope=md("distinct !DISubprogram(name: "+q(f.display_name.empty()?f.name:f.display_name)+", linkageName: "+q(f.name)+", scope: !"+std::to_string(file)+", file: !"+std::to_string(file)+", line: "+std::to_string(std::max<std::size_t>(1,f.at.line))+", type: !"+std::to_string(type)+", scopeLine: "+std::to_string(std::max<std::size_t>(1,f.at.line))+", spFlags: DISPFlagDefinition, unit: !"+std::to_string(compile_unit)+")");}
        o<<")";
        if(release && f.cold) o<<" noinline cold";
        if(captures_stack) o<<" uwtable";
        if(windows_freestanding){
            o<<" \"no-builtins\"";
            if(windows_x64_probe)o<<" \"probe-stack\"=\"bolt_stack_probe\"";
        }
        if(debug)o<<" !dbg !"<<scope;o<<" {\n";
        auto typed=[&](Reg v){return std::string(name(f.registers.at(v)))+" "+r(v);};
        for(std::size_t b=0;b<f.blocks.size();++b){o<<"b"<<b<<":\n";
            block_text.clear();
            for(std::size_t j=0;j<f.blocks[b].code.size();++j){
                const auto& i=f.blocks[b].code[j];std::string suffix;
                if(debug){auto loc=md("!DILocation(line: "+std::to_string(std::max<std::size_t>(1,i.at.line))+", column: "+std::to_string(i.at.column)+", scope: !"+std::to_string(scope)+")");suffix=", !dbg !"+std::to_string(loc);}
                auto emit = [&](std::string_view line) {
                    block_text.append("  ").append(line).append(suffix).push_back('\n');
                };
                auto dest=i.result?r(i.result)+" = ":"";
                auto ty=i.result?f.registers[i.result]:Type::Void;
                auto a=[&](std::size_t n){return r(i.args.at(n));};
                auto binary=[&](const char* op){emit(dest+op+" "+typed(i.args[0])+", "+a(1));};
                switch(i.op){
                case Op::Constant:{
                    if(ty==Type::Ptr)emit(dest+"inttoptr i64 "+std::to_string(i.immediate)+" to ptr");
                    else if(floating(ty))emit(dest+"bitcast i"+std::to_string(width(ty)*8)+" "+std::to_string(i.immediate)+" to "+name(ty));
                    else emit(dest+"add "+std::string(name(ty))+" 0, "+std::to_string(i.immediate));break;}
                case Op::Address:emit(dest+"getelementptr i8, ptr "+sym(i.text)+", i64 0");break;
                case Op::Alloca:emit(dest+"alloca i8, i64 "+std::to_string(i.immediate)+", align 8");break;
                case Op::Offset:emit(dest+"getelementptr i8, ptr "+a(0)+", "+(i.args.size()>1?typed(i.args[1]):"i64 "+std::to_string(i.immediate)));break;
                case Op::AtomicAdd:
                    emit(dest+"atomicrmw add ptr "+a(0)+", i64 "+a(1)+" seq_cst");break;
                case Op::AtomicCompareExchange:{
                    const auto pair="%cas"+std::to_string(i.result);
                    emit(pair+" = cmpxchg ptr "+a(0)+", i64 "+a(1)+", i64 "+a(2)+" seq_cst seq_cst");
                    emit(dest+"extractvalue { i64, i1 } "+pair+", 0");break;
                }
                case Op::Load: {
                    std::string range;
                    if (i.immediate) {
                        const std::string t = name(ty);
                        range = ", !range !{" + t + " 0, " + t + " " + std::to_string(i.immediate) + "}";
                    }
                    emit(dest+"load "+std::string(name(ty))+", ptr "+a(0)+", align 1"+range+alias_tag(i.text));
                    break;
                }
                case Op::Store:emit("store "+typed(i.args[1])+", ptr "+a(0)+", align 1"+alias_tag(i.text));break;
                case Op::CopyElement:
                    if (windows_freestanding) {
                        // Freestanding images cannot call libc. Load the whole value
                        // before storing so self-assignment remains well-defined.
                        const auto word = "i"+std::to_string(8*i.immediate);
                        const auto value = "%record"+std::to_string(b)+"_"+std::to_string(j);
                        emit(value+" = load "+word+", ptr "+a(1)+", align 1");
                        emit("store "+word+" "+value+", ptr "+a(0)+", align 1");
                    } else {
                        emit("call void @llvm.memcpy.p0.p0.i64(ptr "+a(0)+", ptr "+a(1)+", i64 "+
                             std::to_string(i.immediate)+", i1 false)");
                        intrinsic_declarations.insert("declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)");
                    }
                    break;
                case Op::Zero:
                    // A scalar store exposes overwritten initialization to SROA and DSE.
                    // Larger blocks still use BoltRuntime on CRT-free AOT targets.
                    if (i.immediate <= 32) {
                        for (std::size_t pos=0; pos<i.immediate;) {
                            const auto remaining=i.immediate-pos;
                            const std::size_t bytes=remaining>=8 ? 8 : remaining>=4 ? 4 : remaining>=2 ? 2 : 1;
                            std::string ptr=a(0);
                            if (pos) {
                                ptr="%zero."+std::to_string(b)+"."+std::to_string(j)+"."+std::to_string(pos);
                                emit(ptr+" = getelementptr i8, ptr "+a(0)+", i64 "+std::to_string(pos));
                            }
                            emit("store i"+std::to_string(bytes*8)+" 0, ptr "+ptr+", align 1");
                            pos+=bytes;
                        }
                    } else if (windows_freestanding || budgets) {
                        emit("call void @llvm.memset.inline.p0.i64(ptr " + a(0) + ", i8 0, i64 " +
                             std::to_string(i.immediate) + ", i1 false)");
                        intrinsic_declarations.insert("declare void @llvm.memset.inline.p0.i64(ptr, i8, i64 immarg, i1 immarg)");
                    } else if (m.runtime_free() || (release && triple.find("linux") != std::string::npos)) {
                        emit("call void @llvm.memset.p0.i64(ptr " + a(0) + ", i8 0, i64 " +
                             std::to_string(i.immediate) + ", i1 false)");
                        intrinsic_declarations.insert("declare void @llvm.memset.p0.i64(ptr, i8, i64, i1 immarg)");
                    } else {
                        emit("call void @br_zero(ptr " + a(0) + ", i64 " + std::to_string(i.immediate) + ")");
                        intrinsic_declarations.insert("declare void @br_zero(ptr, i64)");
                    }
                    break;
                case Op::Add:binary(i.text=="nsw" ? "add nsw" : "add");break;case Op::Sub:binary("sub");break;case Op::Mul:binary("mul");break;
                case Op::SDiv:binary("sdiv");break;case Op::UDiv:binary("udiv");break;case Op::SRem:binary("srem");break;case Op::URem:binary("urem");break;
                case Op::And:binary("and");break;case Op::Or:binary("or");break;case Op::Xor:binary("xor");break;case Op::Shl:binary("shl");break;case Op::LShr:binary("lshr");break;case Op::AShr:binary("ashr");break;
                case Op::FAdd:binary("fadd");break;case Op::FSub:binary("fsub");break;case Op::FMul:binary("fmul");break;case Op::FDiv:binary("fdiv");break;
                case Op::FNeg:emit(dest+"fneg "+typed(i.args[0]));break;
                case Op::FSqrt: {
                    const auto intrinsic = ty == Type::F64 ? "@llvm.sqrt.f64" : "@llvm.sqrt.f32";
                    emit(dest+"call "+name(ty)+" "+intrinsic+"("+typed(i.args[0])+")");
                    intrinsic_declarations.insert(std::string("declare ")+name(ty)+" "+intrinsic+"("+name(ty)+")");
                    break;
                }
                case Op::CheckedAdd:case Op::CheckedSub:case Op::CheckedMul:{
                    auto intrinsic="llvm."+i.text+(i.op==Op::CheckedAdd?"add":i.op==Op::CheckedSub?"sub":"mul")+".with.overflow."+name(ty);
                    auto pair="{ "+std::string(name(ty))+", i1 }";
                    auto tmp="%pair"+std::to_string(i.result);
                    emit(tmp+" = call "+pair+" @"+intrinsic+"("+typed(i.args[0])+", "+typed(i.args[1])+")");
                    emit(dest+"extractvalue "+pair+" "+tmp+", 0");
                    auto flag="%ov"+std::to_string(i.overflow);
                    emit(flag+" = extractvalue "+pair+" "+tmp+", 1");
                    emit(r(i.overflow)+" = zext i1 "+flag+" to i32");
                    intrinsic_declarations.insert("declare "+pair+" @"+intrinsic+"("+name(ty)+", "+name(ty)+")");break;}
                case Op::Compare:{auto tmp="%cmp"+std::to_string(i.result);emit(tmp+" = "+(floating(f.registers[i.args[0]])?"fcmp ":"icmp ")+i.text+" "+typed(i.args[0])+", "+a(1));emit(dest+"zext i1 "+tmp+" to i32");break;}
                case Op::Select: {
                    const auto flag="%sel."+std::to_string(i.result);
                    emit(flag+" = icmp ne "+typed(i.args[0])+", 0");
                    emit(dest+"select i1 "+flag+", "+typed(i.args[1])+", "+typed(i.args[2])+", !unpredictable !{}");
                    break;
                }
                case Op::Cast:emit(dest+i.text+" "+typed(i.args[0])+" to "+name(ty));break;
                case Op::Call:case Op::CallIndirect:{
                    if (i.storage_move_stride && release && (i.text=="br_memory_move" ? !windows_freestanding : windows_x64_probe)) {
                        const auto first=i.args.size()-3;
                        const auto args="ptr "+a(first)+", ptr "+a(first+1)+", i64 "+a(first+2);
                        if (triple.starts_with("x86_64") && triple.find("windows")!=std::string::npos) {
                            storage_move_strides.insert(i.storage_move_stride);
                            if (windows_freestanding) storage_move_fallback=i.text;
                            const auto displacement=known_storage_displacement(f.blocks[b],j);
                            auto delta=std::to_string(displacement);
                            if (!displacement) {
                                const auto id=std::to_string(b)+"."+std::to_string(j);
                                delta="%delta."+id;
                                emit("%dst."+id+" = ptrtoint ptr "+a(first)+" to i64");
                                emit("%src."+id+" = ptrtoint ptr "+a(first+1)+" to i64");
                                emit(delta+" = sub i64 %dst."+id+", %src."+id);
                            }
                            emit("call void @bolt.storage.move."+std::to_string(i.storage_move_stride)+"(ptr "+a(0)+", "+args+", i64 "+delta+")");
                        } else emit("call void @llvm.memmove.p0.p0.i64("+args+", i1 false)");
                        if (!windows_freestanding)
                            intrinsic_declarations.insert("declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)");
                        break;
                    }
                    auto callee = i.op == Op::Call ? sym(i.text) : a(0);
                    if (budgets && i.op == Op::Call &&
                        (i.text=="br_tick" || i.text=="br_enter" || i.text=="br_leave"))
                        callee="@bolt.budget."+i.text.substr(3);

                    if (i.op == Op::Call && !memory_wrappers.empty() && memory_wrappers.contains(i.text))
                        callee = "@bolt.memory." + i.text.substr(10);
                    const External* c_abi=nullptr;
                    if(i.op==Op::Call && !darwin_externals.empty()){
                        auto decl=darwin_externals.find(i.text);
                        if(decl!=darwin_externals.end())c_abi=decl->second;
                    }
                    if(c_abi && c_abi->name=="fcntl" && c_abi->parameters.size()==3)
                        callee="(i32, i32, ...) "+callee;
                    if(i.op==Op::Call && posix_open && i.text==posix_open->name)
                        callee="(ptr, i32, ...) "+callee;
                    std::string s=dest+"call "+(c_abi?extension(ty,c_abi->signed_result):"")+name(ty)+" "+callee+"(";
                    auto start=i.op==Op::Call?0u:1u;
                    for(std::size_t n=start;n<i.args.size();++n){
                        if(n>start)s+=", ";
                        const auto arg=i.args[n];
                        if(c_abi)s+=std::string(name(f.registers[arg]))+" "+extension(f.registers[arg],c_abi->signed_argument(n))+r(arg);
                        else s+=typed(arg);
                    }
                    const bool diagnostic = i.op == Op::Call && (i.text == "br_frame" || i.text == "br_fail");
                    if (release && f.blocks[b].cold) emit(s + ") noinline cold");
                    else emit(s + (diagnostic ? ") cold" : ")"));
                    break;}
                case Op::Jump: {
                    std::string hint;
                    if (release && triple.starts_with("x86_64") && i.immediate) {
                        const auto count=md(i.immediate == 1 ? "!{!\"llvm.loop.unroll.disable\"}" :
                            "!{!\"llvm.loop.unroll.count\", i32 "+std::to_string(i.immediate)+"}");
                        const auto loop=metadata.size();
                        md("distinct !{!"+std::to_string(loop)+", !"+std::to_string(count)+"}");
                        hint=", !llvm.loop !"+std::to_string(loop);
                    }
                    emit("br label %b"+std::to_string(i.yes)+hint);
                    break;
                }
                case Op::Branch: {
                    auto tmp = "%branch"+std::to_string(b)+"_"+std::to_string(j);
                    emit(tmp+" = icmp ne "+typed(i.args[0])+", 0");
                    std::string hint;
                    // The generic x86-64 target has 128-bit integer SIMD.
                    // Widen four i32 inputs, with two independent sums; retain
                    // the scalar replay and LLVM's normal choices elsewhere.
                    if (release && triple.starts_with("x86_64") && i.immediate == 4) {
                        auto enabled = md("!{!\"llvm.loop.vectorize.enable\", i1 true}");
                        auto width = md("!{!\"llvm.loop.vectorize.width\", i32 4}");
                        auto interleave = md("!{!\"llvm.loop.interleave.count\", i32 2}");
                        auto id = metadata.size();
                        md("distinct !{!" + std::to_string(id) + ", !" + std::to_string(enabled) +
                           ", !" + std::to_string(width) + ", !" + std::to_string(interleave) + "}");
                        hint = ", !llvm.loop !" + std::to_string(id);
                    }
                    if (release && i.text=="unlikely")
                        hint=", !prof !"+std::to_string(md("!{!\"branch_weights\", i32 1, i32 3}"));
                    if (release && f.blocks[i.yes].cold!=f.blocks[i.no].cold) {
                        const char* weights=f.blocks[i.yes].cold ? "i32 1, i32 512" : "i32 512, i32 1";
                        hint=", !prof !"+std::to_string(md(std::string("!{!\"branch_weights\", ")+weights+"}"));
                    }
                    emit("br i1 "+tmp+", label %b"+std::to_string(i.yes)+", label %b"+std::to_string(i.no)+hint);
                    break;
                }
                case Op::Return:emit(i.args.empty()?"ret void":"ret "+typed(i.args[0]));break;
                }
            }
            o << block_text;
        }
        o<<"}\n";
    }
    if (!storage_move_strides.empty()) {
        emit_medium_storage_shift(o,false);
        emit_medium_storage_shift(o,true);
        for (auto stride : storage_move_strides) {
            if (stride>=4) {
                emit_large_storage_shift(o,stride,false);
                emit_large_storage_shift(o,stride,true);
            }
            emit_short_storage_move(o,stride,storage_move_fallback);
        }
    }
    for(const auto& s:intrinsic_declarations)o<<s<<'\n';
    if (debug) {
        const auto flags = md("!{i32 2, !\"Debug Info Version\", i32 3}");
        o << "!llvm.dbg.cu = !{!" << compile_unit << "}\n!llvm.module.flags = !{!" << flags;
        if (triple.find("windows") != std::string::npos) {
            const auto codeview = md("!{i32 2, !\"CodeView\", i32 1}");
            o << ", !" << codeview;
        }
        o << "}\n";
    }
    for(std::size_t i=0;i<metadata.size();++i)o<<'!'<<i<<" = "<<metadata[i]<<'\n';
    return std::move(o).str();
}
} // namespace bolt::ir
