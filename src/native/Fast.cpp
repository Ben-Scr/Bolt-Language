#include <bolt/native/IR.hpp>
#include "Dwarf.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#if (defined(_M_ARM64) || defined(__aarch64__)) && !defined(_M_ARM64EC) && !defined(__arm64ec__)
#define BOLT_USE_WINDOWS_ARM64_FAST 1
#endif
#if (defined(BOLT_USE_WINDOWS_ARM64_FAST) || defined(_M_X64) || defined(__x86_64__)) && !defined(_M_ARM64EC) && !defined(__arm64ec__)
#define BOLT_USE_WINDOWS_UNWIND 1
#endif
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#include "AppleJit.hpp"
#endif

namespace bolt::native_backend {
namespace {
using namespace ir;
using namespace detail;
constexpr int ax=0,cx=1,dx=2,sp=4,bp=5,si=6,di=7,r8=8,r9=9,r10=10,r11=11;
std::size_t aligned(std::size_t v,std::size_t a){return (v+a-1)/a*a;}
struct Argument { int reg=-1; bool fp=false; std::size_t stack=0; };
std::vector<Argument> arguments(const std::vector<Type>& types,std::size_t& stack,bool windows_abi) {
    std::vector<Argument> result;unsigned integers=0,floats=0;stack=windows_abi?32:0;
    static constexpr int sysv[]={di,si,dx,cx,r8,r9};static constexpr int win[]={cx,dx,r8,r9};
    for(std::size_t j=0;j<types.size();++j){Argument a;a.fp=floating(types[j]);
        if(windows_abi){if(j<4)a.reg=a.fp?static_cast<int>(j):win[j];else{a.stack=stack;stack+=8;}}
        else if(a.fp&&floats<8)a.reg=static_cast<int>(floats++);
        else if(!a.fp&&integers<6)a.reg=sysv[integers++];
        else{a.stack=stack;stack+=8;}
        result.push_back(a);
    }
    return result;
}
class Encoder {
    const Module& module;
    const bool windows_abi;
    DebugRecorder* debug;
    Image image;
    std::map<std::string,const External*> externals;
    std::map<std::string,std::string> import_slots;
    const Function* f=nullptr;
    struct Slot {
        enum Kind { Value, LocalAddress, Constant } kind = Value;
        std::int32_t offset = 0;
        std::uint64_t bits = 0;
    };
    std::vector<Slot> slots;
    std::vector<std::size_t> uses;
    std::vector<std::size_t> labels;
    std::vector<std::pair<std::size_t,Label>> jumps;
    Bytes& code(){return image.text;}
    void byte(unsigned v){code().u8(static_cast<std::uint8_t>(v));}
    void rex(bool wide,int reg=0,int base=0){auto v=0x40|(wide?8:0)|((reg&8)?4:0)|((base&8)?1:0);if(v!=0x40)byte(static_cast<unsigned>(v));}
    void modrm(unsigned mod,int reg,int base){byte((mod<<6)|((reg&7)<<3)|(base&7));}
    void memory(int reg,int base,std::int32_t disp){
        // RBP/R13 require an explicit displacement even at offset zero.
        const unsigned mod = disp == 0 && (base & 7) != bp ? 0 : disp >= -128 && disp <= 127 ? 1 : 2;
        modrm(mod,reg,base);
        if((base&7)==sp)byte(0x24);
        if(mod==1)byte(static_cast<std::uint8_t>(disp));
        if(mod==2)code().u32(static_cast<std::uint32_t>(disp));
    }
    std::int32_t home(Reg r) const {return slots[r].offset;}
    Type type(Reg r){return f->registers.at(r);}
    void immediate(int dst,std::uint64_t n){
        const bool wide = n > std::numeric_limits<std::uint32_t>::max();
        rex(wide,0,dst);byte(0xb8+(dst&7));
        if(wide)code().u64(n);else code().u32(static_cast<std::uint32_t>(n));
    }
    void move(int dst,int src){rex(true,src,dst);byte(0x89);modrm(3,src,dst);}
    void load_memory(int dst,int base,std::int32_t disp,unsigned size,bool sign=false){
        if(size==1||size==2){rex(sign,dst,base);byte(0x0f);byte(sign?(size==1?0xbe:0xbf):(size==1?0xb6:0xb7));memory(dst,base,disp);}
        else{rex(size==8||sign,dst,base);byte(size==4&&sign?0x63:0x8b);memory(dst,base,disp);}
    }
    void store_memory(int base,std::int32_t disp,int src,unsigned size){
        if(size==2)byte(0x66);if(size==1&&src>=4&&src<8)byte(0x40);else rex(size==8,src,base);
        byte(size==1?0x88:0x89);memory(src,base,disp);
    }
    void load(int dst,Reg r,bool sign=false){
        const auto& slot = slots[r];
        const auto size = width(type(r));
        if (slot.kind == Slot::Value) {load_memory(dst,bp,slot.offset,size,sign);return;}
        if (slot.kind == Slot::LocalAddress) {lea(dst,bp,slot.offset);return;}
        auto bits = slot.bits;
        if (size < 8) {
            const auto mask = (std::uint64_t{1} << (size * 8)) - 1;
            bits &= mask;
            if (sign && bits > (mask >> 1)) bits |= ~mask;
        }
        immediate(dst,bits);
    }
    void save(Reg r,int src=ax){if(uses[r])store_memory(bp,home(r),src,8);}
    void lea(int dst,int base,std::int32_t disp){rex(true,dst,base);byte(0x8d);memory(dst,base,disp);}
    void fp_memory(bool store,int xmm,int base,std::int32_t disp,Type ty){byte(ty==Type::F32?0xf3:0xf2);rex(false,xmm,base);byte(0x0f);byte(store?0x11:0x10);memory(xmm,base,disp);}
    void fp_load(int xmm,Reg r){fp_memory(false,xmm,bp,home(r),type(r));}
    void fp_save(Reg r,int xmm=0){if(uses[r])fp_memory(true,xmm,bp,home(r),type(r));}
    void setcc(unsigned condition,int dst){if(dst>=4)rex(false,0,dst);byte(0x0f);byte(condition);modrm(3,0,dst);rex(false,dst,dst);byte(0x0f);byte(0xb6);modrm(3,dst,dst);}
    void relative(const std::string& target,bool branch=false){auto p=code().size();code().u32(0);image.fixups.push_back({1,p,RelocKind::Relative32,target,0,branch});}
    std::string imported(const std::string& target){auto i=import_slots.find(target);if(i!=import_slots.end())return i->second;
        const auto name="b_import_"+std::to_string(import_slots.size());import_slots[target]=name;image.data.align(8);auto offset=image.data.size();image.data.u64(0);image.symbols.push_back({name,2,offset,8,false,false});image.fixups.push_back({2,offset,RelocKind::Absolute64,target,0});return name;}
    void address(int dst,const std::string& name){rex(true,dst);byte(externals.contains(name)?0x8b:0x8d);modrm(0,dst,5);relative(externals.contains(name)?imported(name):name);}
    void branch(Label label,unsigned condition=0){if(condition){byte(0x0f);byte(condition);}else byte(0xe9);jumps.emplace_back(code().size(),label);code().u32(0);}
    void compare_zero(Reg r){load(ax,r);byte(0x48);byte(0x85);byte(0xc0);}
    void binary_integer(const Instruction& i,bool materialize_overflow=true){
        auto ty=type(i.args[0]);auto n=width(ty);
        const bool checked=i.op==Op::CheckedAdd||i.op==Op::CheckedSub||i.op==Op::CheckedMul;
        if(i.op==Op::SDiv||i.op==Op::SRem||i.op==Op::UDiv||i.op==Op::URem){
            bool sign=i.op==Op::SDiv||i.op==Op::SRem;load(ax,i.args[0],sign);load(cx,i.args[1],sign);
            if(sign){byte(0x48);byte(0x99);}else{byte(0x31);byte(0xd2);}byte(0x48);byte(0xf7);byte(sign?0xf9:0xf1);
            if(i.op==Op::SRem||i.op==Op::URem)move(ax,dx);save(i.result);return;
        }
        load(ax,i.args[0]);load(cx,i.args[1]);
        if(n==2)byte(0x66);rex(n==8,cx,ax);
        switch(i.op){
        case Op::Add:case Op::CheckedAdd:byte(n==1?0x00:0x01);byte(0xc8);break;
        case Op::Sub:case Op::CheckedSub:byte(n==1?0x28:0x29);byte(0xc8);break;
        case Op::Mul:case Op::CheckedMul:
            if(n<4){
                // MUL/IMUL writes the double-width product into AX or DX:AX.
                // CF and OF report unsigned or signed narrowing respectively.
                byte(n==1?0xf6:0xf7);byte(checked&&i.text=="s"?0xe9:0xe1);
            }else if(checked&&i.text=="u"){byte(0xf7);byte(0xe1);}
            else{byte(0x0f);byte(0xaf);byte(0xc1);}break;
        case Op::And:byte(n==1?0x20:0x21);byte(0xc8);break;
        case Op::Or:byte(n==1?0x08:0x09);byte(0xc8);break;
        case Op::Xor:byte(n==1?0x30:0x31);byte(0xc8);break;
        case Op::Shl:case Op::LShr:case Op::AShr:byte(n==1?0xd2:0xd3);byte(i.op==Op::Shl?0xe0:i.op==Op::LShr?0xe8:0xf8);break;
        default:throw std::runtime_error("unexpected integer operation");
        }
        // MOV does not change flags; capture arithmetic overflow before any ALU.
        if(checked && materialize_overflow && uses[i.overflow]){setcc(i.text=="s"?0x90:0x92,cx);save(i.overflow,cx);}save(i.result);
    }
    void fp_binary(const Instruction& i){
        fp_load(0,i.args[0]);fp_load(1,i.args[1]);byte(type(i.result)==Type::F32?0xf3:0xf2);byte(0x0f);
        byte(i.op==Op::FAdd?0x58:i.op==Op::FSub?0x5c:i.op==Op::FMul?0x59:0x5e);byte(0xc1);fp_save(i.result);
    }
    unsigned integer_compare(const Instruction& i){
        const auto& p=i.text;
        const bool sign=!p.empty()&&p[0]=='s';
        load(ax,i.args[0],sign);load(cx,i.args[1],sign);byte(0x48);byte(0x39);byte(0xc8);
        const unsigned cc=p=="eq"?0x94:p=="ne"?0x95:p=="slt"?0x9c:p=="sle"?0x9e:p=="sgt"?0x9f:p=="sge"?0x9d:p=="ult"?0x92:p=="ule"?0x96:p=="ugt"?0x97:p=="uge"?0x93:0;
        if(!cc)throw std::runtime_error("unsupported native integer predicate: "+p);
        return cc;
    }
    void compare(const Instruction& i){
        bool fp=floating(type(i.args[0]));const auto& p=i.text;
        if(fp){fp_load(0,i.args[0]);fp_load(1,i.args[1]);if(type(i.args[0])==Type::F64)byte(0x66);byte(0x0f);byte(0x2e);byte(0xc1);
            unsigned cc=p=="oeq"?0x94:p=="une"||p=="one"?0x95:p=="olt"?0x92:p=="ole"?0x96:p=="ogt"?0x97:p=="oge"?0x93:0;
            if(!cc)throw std::runtime_error("unsupported native float predicate: "+p);
            // SETcc and MOVZX leave the floating-point comparison flags intact.
            setcc(cc,ax);setcc(p=="une"?0x9a:0x9b,cx);byte(p=="une"?0x08:0x20);byte(0xc8);
        }else setcc(integer_compare(i),ax);
        save(i.result);
    }
    void cast(const Instruction& i){
        auto from=type(i.args[0]),to=type(i.result);
        if(i.text=="fpext"||i.text=="fptrunc") {fp_load(0,i.args[0]);byte(from==Type::F32?0xf3:0xf2);byte(0x0f);byte(0x5a);byte(0xc0);fp_save(i.result);return;}
        if (i.text=="fptosi" || i.text=="fptoui") {
            fp_load(0,i.args[0]);
            const unsigned prefix = from==Type::F32 ? 0xf3 : 0xf2;
            auto truncate = [&]{byte(prefix);byte(0x48);byte(0x0f);byte(0x2c);byte(0xc0);}; // cvttss2si/cvttsd2si rax,xmm0
            truncate();
            if (i.text=="fptoui" && to==Type::I64) {
                // For checked inputs in [2^63,2^64), signed CVTT returns INT64_MIN.
                byte(0x48);byte(0x85);byte(0xc0);byte(0x0f);byte(0x89);
                const auto done=code().size();code().u32(0);
                immediate(cx,from==Type::F32 ? 0x5f000000ull : 0x43e0000000000000ull);
                byte(0x66);byte(0x48);byte(0x0f);byte(0x6e);byte(0xc9); // movq xmm1,rcx
                byte(prefix);byte(0x0f);byte(0x5c);byte(0xc1);          // subtract exactly 2^63
                truncate();
                byte(0x48);byte(0x0f);byte(0xba);byte(0xe8);byte(63); // bts rax,63
                code().patch(done,code().size()-done-4,4);
            }
            save(i.result);return;
        }
        if(i.text=="sitofp"||i.text=="uitofp"){
            load(ax,i.args[0],i.text=="sitofp");
            auto convert=[&]{byte(to==Type::F32?0xf3:0xf2);byte(0x48);byte(0x0f);byte(0x2a);byte(0xc0);};
            if(i.text=="uitofp"&&width(from)==8){
                // For values outside signed i64, round((x>>1)|(x&1))*2. This
                // preserves the low sticky bit and the normal IEEE rounding.
                byte(0x48);byte(0x85);byte(0xc0);byte(0x0f);byte(0x89);auto positive=code().size();code().u32(0);
                move(cx,ax);byte(0x48);byte(0xd1);byte(0xe8);byte(0x48);byte(0x83);byte(0xe1);byte(1);byte(0x48);byte(0x09);byte(0xc8);convert();
                byte(to==Type::F32?0xf3:0xf2);byte(0x0f);byte(0x58);byte(0xc0);byte(0xe9);auto done=code().size();code().u32(0);
                code().patch(positive,code().size()-positive-4,4);convert();code().patch(done,code().size()-done-4,4);
            }else convert();fp_save(i.result);return;
        }
        if(i.text=="sext"){load(ax,i.args[0],true);save(i.result);return;}
        if(i.text=="zext"||i.text=="trunc"||i.text=="bitcast"||i.text=="ptrtoint"||i.text=="inttoptr") {load(ax,i.args[0]);save(i.result);return;}
        throw std::runtime_error("unsupported native cast: "+i.text);
    }
    void call(const Instruction& i){
        auto entry=externals.find(i.text);
        auto sign=[&](std::size_t n){return entry!=externals.end() && entry->second->signed_argument(n);};
        std::size_t begin=i.op==Op::CallIndirect?1:0;std::vector<Type> types;
        for(std::size_t j=begin;j<i.args.size();++j)types.push_back(type(i.args[j]));std::size_t stack=0;auto places=arguments(types,stack,windows_abi);
        // Stack arguments first: loading them must not overwrite argument registers.
        for(std::size_t j=0;j<places.size();++j)if(places[j].reg<0){load(ax,i.args[j+begin]);store_memory(sp,static_cast<std::int32_t>(places[j].stack),ax,8);}
        for(std::size_t j=0;j<places.size();++j)if(places[j].reg>=0){if(places[j].fp)fp_load(places[j].reg,i.args[j+begin]);else load(places[j].reg,i.args[j+begin],sign(j));}
        if(i.op==Op::CallIndirect){load(r11,i.args[0]);byte(0x41);byte(0xff);byte(0xd3);}
        else if(externals.contains(i.text)){byte(0xff);byte(0x15);relative(imported(i.text));}
        else{byte(0xe8);relative(i.text,true);}
        if(i.result){if(floating(type(i.result)))fp_save(i.result);else save(i.result);}
    }
    Bytes windows_prologue(std::size_t frame,std::size_t start) {
        // RBP is a regular saved register with a copy of pre-allocation RSP.
        // RSP stays fixed in the body, so Win64 can unwind using RSP alone.
        // Probe large frames without changing it until the final allocation.
        if(frame>=4096) {
            move(r10,sp);immediate(r11,frame/4096);
            const auto loop=code().size();
            byte(0x49);byte(0x81);byte(0xea);code().u32(4096); // sub r10,4096
            byte(0x49);byte(0x85);byte(0x02);                 // test [r10],rax
            byte(0x49);byte(0xff);byte(0xcb);                 // dec r11
            byte(0x75);byte(static_cast<std::uint8_t>(loop-(code().size()+1)));
            if(frame%4096) {
                byte(0x49);byte(0x81);byte(0xea);code().u32(static_cast<std::uint32_t>(frame%4096));
                byte(0x49);byte(0x85);byte(0x02);
            }
        }
        byte(0x48);byte(0x81);byte(0xec);code().u32(static_cast<std::uint32_t>(frame));
        const auto end=static_cast<std::uint8_t>(code().size()-start);
        Bytes codes;
        codes.u8(end);
        if(frame<=128) { // UWOP_ALLOC_SMALL, 8..128 bytes
            codes.u8(static_cast<std::uint8_t>(((frame/8-1)<<4)|2));
        }else if(frame/8<=65535) { // UWOP_ALLOC_LARGE, OpInfo 0
            codes.u8(1);codes.u16(static_cast<std::uint16_t>(frame/8));
        }else { // UWOP_ALLOC_LARGE, OpInfo 1
            codes.u8(0x11);codes.u32(static_cast<std::uint32_t>(frame));
        }
        codes.u8(1);codes.u8(0x50); // push rbp completed at byte 1
        Bytes info;
        info.u8(1);info.u8(end);info.u8(static_cast<std::uint8_t>(codes.size()/2));info.u8(0);
        info.insert(info.end(),codes.begin(),codes.end());info.align(4);
        return info;
    }
    void function(const Function& fn){
        f=&fn;slots.assign(fn.registers.size(),{});labels.assign(fn.blocks.size(),0);jumps.clear();code().align(16,0x90);auto start=code().size();
        if(debug)debug->begin(fn,start);
        image.symbols.push_back({fn.name,1,start,0,fn.exported,true});const auto definition=image.symbols.size()-1;
        std::size_t frame=0,outgoing=windows_abi?32:0;
        uses.assign(fn.registers.size(),0);
        for(const auto& b:fn.blocks)for(const auto& i:b.code){
            for (auto r : i.args) ++uses[r];
            if(i.op==Op::Alloca){
                if(i.immediate>0x3fffffff || frame+i.immediate>0x3fffffff)throw std::runtime_error("native stack frame exceeds supported size");
                frame=aligned(frame+static_cast<std::size_t>(i.immediate),8);
                slots[i.result]={Slot::LocalAddress,-static_cast<std::int32_t>(frame)};
            } else if(i.op==Op::Constant && !floating(type(i.result))) {
                slots[i.result]={Slot::Constant,0,i.immediate};
            }
            if(i.op==Op::Call||i.op==Op::CallIndirect){std::vector<Type> types;for(std::size_t j=i.op==Op::Call?0:1;j<i.args.size();++j)types.push_back(type(i.args[j]));std::size_t n=0;(void)arguments(types,n,windows_abi);outgoing=std::max(outgoing,n);}
        }
        // Field addresses of stack values can use the same frame-relative addressing as the local itself.
        for (const auto& block : fn.blocks) for (const auto& i : block.code) {
            if (i.op != Op::Offset || i.args.size() != 1 || i.immediate > INT32_MAX) continue;
            const auto& base = slots[i.args[0]];
            if (base.kind != Slot::LocalAddress) continue;
            const auto displacement = static_cast<std::int64_t>(base.offset) + static_cast<std::int64_t>(i.immediate);
            if (displacement <= INT32_MAX) slots[i.result] = {Slot::LocalAddress,static_cast<std::int32_t>(displacement)};
        }
        for (std::size_t r=1;r<slots.size();++r) {
            if (slots[r].kind!=Slot::Value || !uses[r]) continue;
            frame+=8;
            if(frame>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("native frame too large");
            slots[r].offset=-static_cast<std::int32_t>(frame);
        }
        frame=aligned(frame+outgoing,16);
        if(frame>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw std::runtime_error("native frame too large");
        byte(0x55);byte(0x48);byte(0x89);byte(0xe5);
        Bytes unwind;
        if(windows_abi)unwind=windows_prologue(frame,start);
        else {
            std::size_t remaining=frame;
            while(remaining>=4096){byte(0x48);byte(0x81);byte(0xec);code().u32(4096);byte(0x48);byte(0x85);byte(0x04);byte(0x24);remaining-=4096;}
            if(remaining){byte(0x48);byte(0x81);byte(0xec);code().u32(static_cast<std::uint32_t>(remaining));}
        }
        std::size_t args_stack=0;auto incoming=arguments(fn.parameters,args_stack,windows_abi);
        // Save register arguments before using RAX to copy incoming stack slots.
        for(std::size_t j=0;j<incoming.size();++j)if(incoming[j].reg>=0){auto reg=static_cast<Reg>(j+1);if(incoming[j].fp)fp_save(reg,incoming[j].reg);else save(reg,incoming[j].reg);}
        for(std::size_t j=0;j<incoming.size();++j)if(incoming[j].reg<0){load_memory(ax,bp,static_cast<std::int32_t>(16+incoming[j].stack),width(fn.parameters[j]));save(static_cast<Reg>(j+1));}
        for(std::size_t b=0;b<fn.blocks.size();++b){
            labels[b]=code().size();
            const auto& instructions=fn.blocks[b].code;
            unsigned branch_cc=0;
            for(const auto& i:instructions){
            if(debug)debug->position(i.at,code().size());
            // Both the overflow and comparison flags are live across MOV.
            // Fuse only an adjacent, single-use condition in this basic block.
            if (instructions.size()>=2 && &i==&instructions[instructions.size()-2] &&
                instructions.back().op==Op::Branch) {
                const auto condition=instructions.back().args[0];
                if (uses[condition]==1 && i.overflow==condition) {
                    binary_integer(i,false);
                    branch_cc=i.text=="s" ? 0x80 : 0x82;
                    continue;
                }
                if (uses[condition]==1 && i.result==condition && i.op==Op::Compare && !floating(type(i.args[0]))) {
                    branch_cc=integer_compare(i)-0x10; // SETcc -> Jcc
                    continue;
                }
            }
            switch(i.op){
            case Op::Constant:if(slots[i.result].kind==Slot::Value){immediate(ax,i.immediate);save(i.result);}break;
            case Op::Address:address(ax,i.text);save(i.result);break;
            case Op::Alloca:break;
            case Op::Offset:
                if (slots[i.result].kind == Slot::LocalAddress) break;
                load(ax,i.args[0]);
                if (i.args.size() == 1 && i.immediate <= INT32_MAX) {
                    lea(ax,ax,static_cast<std::int32_t>(i.immediate));
                } else {
                    if (i.args.size()>1) load(cx,i.args[1]); else immediate(cx,i.immediate);
                    byte(0x48);byte(0x01);byte(0xc8);
                }
                save(i.result);break;
            case Op::AtomicAdd:
                load(cx,i.args[0]);load(ax,i.args[1]);
                byte(0xf0);byte(0x48);byte(0x0f);byte(0xc1);memory(ax,cx,0);save(i.result);break;
            case Op::AtomicCompareExchange:
                load(cx,i.args[0]);load(ax,i.args[1]);load(dx,i.args[2]);
                byte(0xf0);byte(0x48);byte(0x0f);byte(0xb1);memory(dx,cx,0);save(i.result);break;
            case Op::Load:case Op::Store:{
                const auto& ptr = slots[i.args[0]];
                const bool local = ptr.kind==Slot::LocalAddress;
                const int base = local ? bp : ax;
                const auto displacement = local ? ptr.offset : 0;
                if(!local)load(ax,i.args[0]);
                if(i.op==Op::Load){load_memory(ax,base,displacement,width(type(i.result)));save(i.result);}
                else {load(cx,i.args[1]);store_memory(base,displacement,cx,width(type(i.args[1])));}
                break;
            }
            case Op::CopyElement:{
                load(r10,i.args[0]);load(cx,i.args[1]);
                for (unsigned pos=0;pos<i.immediate;) {
                    const unsigned bytes=i.immediate-pos>=8 ? 8 : 1;
                    load_memory(ax,cx,pos,bytes);store_memory(r10,pos,ax,bytes);
                    pos+=bytes;
                }
                break;}
            case Op::Zero:{load(r10,i.args[0]);immediate(ax,0);std::uint64_t pos=0;
                // Slots are normally small. Use a compact loop for large arrays.
                if(i.immediate>=64){immediate(cx,i.immediate/8);auto loop=code().size();store_memory(r10,0,ax,8);byte(0x49);byte(0x83);byte(0xc2);byte(8);byte(0x48);byte(0xff);byte(0xc9);byte(0x0f);byte(0x85);auto at=code().size();code().u32(static_cast<std::uint32_t>(static_cast<std::int64_t>(loop)-static_cast<std::int64_t>(at+4)));pos=i.immediate/8*8;}
                else while(pos+8<=i.immediate){store_memory(r10,static_cast<std::int32_t>(pos),ax,8);pos+=8;}
                auto tail=i.immediate>=64?0:pos;while(pos<i.immediate){store_memory(r10,static_cast<std::int32_t>(tail++),ax,1);++pos;}break;}
            case Op::Add:case Op::Sub:case Op::Mul:case Op::SDiv:case Op::UDiv:case Op::SRem:case Op::URem:case Op::And:case Op::Or:case Op::Xor:case Op::Shl:case Op::LShr:case Op::AShr:case Op::CheckedAdd:case Op::CheckedSub:case Op::CheckedMul:binary_integer(i);break;
            case Op::FAdd:case Op::FSub:case Op::FMul:case Op::FDiv:fp_binary(i);break;
            case Op::FSqrt:
                fp_load(0,i.args[0]);byte(type(i.result)==Type::F32?0xf3:0xf2);
                byte(0x0f);byte(0x51);byte(0xc0);fp_save(i.result);break; // sqrtss/sqrtsd xmm0,xmm0
            case Op::FNeg:load(ax,i.args[0]);immediate(cx,type(i.result)==Type::F32?0x80000000ull:0x8000000000000000ull);byte(0x48);byte(0x31);byte(0xc8);save(i.result);break;
            case Op::Compare:compare(i);break;
            case Op::Select:
                compare_zero(i.args[0]);load(cx,i.args[1]);load(ax,i.args[2]);
                rex(true,ax,cx);byte(0x0f);byte(0x45);modrm(3,ax,cx); // cmovne rax,rcx
                save(i.result);break;
            case Op::Cast:cast(i);break;
            case Op::Call:case Op::CallIndirect:call(i);break;
            case Op::Jump:if(i.yes!=b+1)branch(i.yes);break;
            case Op::Branch:
                if (!branch_cc) {compare_zero(i.args[0]);branch_cc=0x85;}
                if(i.yes==b+1)branch(i.no,branch_cc^1);
                else {branch(i.yes,branch_cc);if(i.no!=b+1)branch(i.no);}
                break;
            case Op::Return:
                if(!i.args.empty()){if(floating(type(i.args[0])))fp_load(0,i.args[0]);else load(ax,i.args[0]);}
                // Windows identifies epilogues by their legal opcode sequence.
                if(windows_abi){byte(0x48);byte(0x81);byte(0xc4);code().u32(static_cast<std::uint32_t>(frame));byte(0x5d);}
                else byte(0xc9);
                byte(0xc3);break;
            }
        }}
        for(const auto& [at,label]:jumps){auto delta=static_cast<std::int64_t>(labels.at(label))-static_cast<std::int64_t>(at+4);if(delta<std::numeric_limits<std::int32_t>::min()||delta>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("native branch exceeds 2 GiB");code().patch(at,static_cast<std::uint64_t>(delta),4);}
        image.symbols[definition].size=code().size()-start;
        if(windows_abi)image.unwind.push_back({start,code().size(),std::move(unwind)});
        if(debug)debug->end(code().size());
    }
public:
    Encoder(const Module& m,X64ABI abi,DebugRecorder* positions):module(m),windows_abi(abi==X64ABI::Windows),debug(positions){}
    Image run(){verify(module);
        for(const auto& e:module.externals){externals.emplace(e.name,&e);image.symbols.push_back({e.name,0,0,0,true,true});}
        for(const auto& g:module.globals){image.data.align(g.alignment);auto at=image.data.size();image.symbols.push_back({g.name,2,at,g.bytes.size(),false,false});image.data.insert(image.data.end(),g.bytes.begin(),g.bytes.end());for(const auto& r:g.relocations)image.fixups.push_back({2,at+r.offset,RelocKind::Absolute64,r.symbol,r.addend});}
        for(const auto& fn:module.functions)function(fn);return std::move(image);
    }
};
struct LinkTargets {
    struct Address { unsigned section; std::uintptr_t value; };
    std::unordered_map<std::string,Address> names;
    std::vector<Address> relocations;
    std::unordered_map<std::string,std::vector<std::size_t>> callers;

    LinkTargets(const Image& image,const std::vector<Symbol>& externals) {
        names.reserve(externals.size()+image.symbols.size());
        for(const auto& s:externals) names[s.name]={0,reinterpret_cast<std::uintptr_t>(s.address)};
        for(const auto& s:image.symbols) if(s.section) names[s.name]={s.section,static_cast<std::uintptr_t>(s.offset)};
        for(const auto& s:image.symbols) if(s.function && s.section==1) callers.try_emplace(s.name);
        relocations.reserve(image.fixups.size());
        for(const auto& fixup:image.fixups) {
            auto found=names.find(fixup.symbol);
            if(found==names.end()) throw std::runtime_error("unresolved JIT symbol: "+fixup.symbol);
            if(auto it=callers.find(fixup.symbol);it!=callers.end()) it->second.push_back(relocations.size());
            relocations.push_back(found->second);
        }
    }
};
class FastImage final : public JitImage {
    void* base=nullptr;std::size_t bytes=0;
#if defined(__APPLE__) && defined(__aarch64__)
    apple_jit::Pool* pool=nullptr;
    apple_jit::Block block{};
#endif
#ifdef BOLT_USE_WINDOWS_UNWIND
    std::vector<RUNTIME_FUNCTION> unwind;
    bool unwind_registered=false;
#endif
    std::shared_ptr<const LinkTargets> targets;
    std::unique_ptr<JitImage> methods;
    std::uintptr_t text_address=0,data_address=0;
    std::uintptr_t resolve(LinkTargets::Address a) const {
        return a.value+(a.section==1?text_address:a.section==2?data_address:0);
    }
public:
    FastImage(const Image& image,const std::vector<Symbol>& external)
        :FastImage(image,std::make_shared<LinkTargets>(image,external)) {}
    FastImage(const Image& image,std::shared_ptr<const LinkTargets> links,const ir::Module* delta=nullptr):targets(std::move(links)){
#ifdef _WIN32
        SYSTEM_INFO info;GetSystemInfo(&info);const auto page=std::size_t(info.dwPageSize);
#else
        auto query=sysconf(_SC_PAGESIZE);if(query<=0)throw std::runtime_error("could not query native page size");const auto page=static_cast<std::size_t>(query);
#endif
        const auto text_size=aligned(std::max<std::size_t>(1,image.text.size()),page);bytes=text_size+aligned(std::max<std::size_t>(1,image.data.size()),page);
#ifdef BOLT_USE_WINDOWS_UNWIND
        const auto metadata=bytes;
        std::size_t unwind_size=0;
        for(const auto& entry:image.unwind)unwind_size+=entry.info.size();
        bytes+=aligned(unwind_size,page);
        if(bytes>std::numeric_limits<DWORD>::max())throw std::runtime_error("Windows JIT image exceeds 4 GiB");
#endif
#ifdef _WIN32
        base=VirtualAlloc(nullptr,bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if(!base)throw std::runtime_error("VirtualAlloc failed for JIT code");
#elif defined(__APPLE__) && defined(__aarch64__)
        pool=&apple_jit::pool(page);
        block=pool->acquire(text_size,bytes-text_size);
        base=block.code;
#else
        base=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(base==MAP_FAILED){base=nullptr;throw std::runtime_error("mmap failed for JIT code");}
#endif
        try{
            auto* text=static_cast<std::uint8_t*>(base);
#if defined(__APPLE__) && defined(__aarch64__)
            auto staged=image.text;
            auto* write_text=staged.data();
            auto* data=block.data;
#else
            auto* write_text=text;
            auto* data=text+text_size;
            if(!image.text.empty())std::memcpy(text,image.text.data(),image.text.size());
#endif
            if(!image.data.empty())std::memcpy(data,image.data.data(),image.data.size());
            text_address=reinterpret_cast<std::uintptr_t>(text);
            data_address=reinterpret_cast<std::uintptr_t>(data);
            std::vector<LinkTargets::Address> redirected;
            if(delta) {
                std::vector<Symbol> imports;
                for(const auto& e:delta->externals) imports.push_back({e.name.c_str(),symbol(e.name)});
                methods=fast_jit(*delta,imports,{});
                redirected=targets->relocations;
                for(const auto& f:delta->functions) {
                    auto callers=targets->callers.find(f.name);
                    if(callers==targets->callers.end()) continue;
                    auto address=reinterpret_cast<std::uintptr_t>(methods->symbol(f.name));
                    for(auto index:callers->second) redirected[index]={0,address};
                }
            }
            const auto& relocations=delta?redirected:targets->relocations;
            for(std::size_t index=0;index<image.fixups.size();++index) {
                const auto& r=image.fixups[index];
                auto* at=(r.section==1?write_text:data)+r.offset;
                auto location=reinterpret_cast<std::uintptr_t>((r.section==1?text:data)+r.offset);
                auto target=resolve(relocations[index]);
                if(r.kind==RelocKind::Absolute64){auto value=target+static_cast<std::uint64_t>(r.addend);std::memcpy(at,&value,8);}
                else if(r.kind==RelocKind::ArmPage21 || r.kind==RelocKind::ArmLo12)
                    apply_arm_address_fixup(at,r.kind,target+static_cast<std::uintptr_t>(r.addend),location);
                else{auto delta=static_cast<std::int64_t>(target)-static_cast<std::int64_t>(location+4)+r.addend;if(delta<std::numeric_limits<std::int32_t>::min()||delta>std::numeric_limits<std::int32_t>::max())throw std::runtime_error("JIT relative address exceeds 2 GiB");auto n=static_cast<std::int32_t>(delta);std::memcpy(at,&n,4);}
            }
            // W^X: code is writable only during relocation and executable only
            // afterward. Mutable globals and import slots remain non-executable.
#ifdef _WIN32
            DWORD old=0;if(!VirtualProtect(base,text_size,PAGE_EXECUTE_READ,&old))throw std::runtime_error("VirtualProtect failed for JIT code");FlushInstructionCache(GetCurrentProcess(),base,text_size);
#ifdef BOLT_USE_WINDOWS_UNWIND
            if(!image.unwind.empty()) {
                auto next=metadata;
                unwind.reserve(image.unwind.size());
                for(const auto& entry:image.unwind) {
                    RUNTIME_FUNCTION fn{};
                    fn.BeginAddress=static_cast<DWORD>(entry.begin);
#if defined(_M_X64) || defined(__x86_64__)
                    fn.EndAddress=static_cast<DWORD>(entry.end);
#endif
                    fn.UnwindData=static_cast<DWORD>(next);
                    std::memcpy(text+next,entry.info.data(),entry.info.size());
                    unwind.push_back(fn);next+=entry.info.size();
                }
                if(!VirtualProtect(text+metadata,bytes-metadata,PAGE_READONLY,&old))
                    throw std::runtime_error("could not protect Windows unwind information");
                if(!RtlAddFunctionTable(unwind.data(),static_cast<DWORD>(unwind.size()),reinterpret_cast<DWORD64>(base)))
                    throw std::runtime_error("could not register Windows unwind information");
                unwind_registered=true;
            }
#endif
#elif defined(__APPLE__) && defined(__aarch64__)
            apple_jit::publish(block,staged);
#else
            if(mprotect(base,text_size,PROT_READ|PROT_EXEC)!=0)throw std::runtime_error("mprotect failed for JIT code");
            __builtin___clear_cache(reinterpret_cast<char*>(base),reinterpret_cast<char*>(base)+text_size);
#endif
        }catch(...){release();throw;}
    }
    void release(){if(!base)return;
#ifdef _WIN32
#ifdef BOLT_USE_WINDOWS_UNWIND
        if(unwind_registered){RtlDeleteFunctionTable(unwind.data());unwind_registered=false;}
#endif
        VirtualFree(base,0,MEM_RELEASE);
#elif defined(__APPLE__) && defined(__aarch64__)
        pool->release(block);
#else
        munmap(base,bytes);
#endif
        base=nullptr;
    }
    ~FastImage()override{release();}
    void* symbol(const std::string& name) const override {
        auto i=targets->names.find(name);
        if(i==targets->names.end())throw std::runtime_error("JIT export not found: "+name);
        return reinterpret_cast<void*>(resolve(i->second));
    }
};
Image fast_image(const ir::Module& module,const std::string& target,[[maybe_unused]] bool jit=false,[[maybe_unused]] DebugRecorder* debug=nullptr) {
#ifdef BOLT_USE_WINDOWS_ARM64_FAST
    return aarch64_image(module,ArmABI::Windows);
#elif defined(_WIN32)
    return x64_image(module,X64ABI::Windows);
#elif defined(__APPLE__)
    if(target.starts_with("arm64-"))
        return aarch64_image(module,ArmABI::Apple,jit?ArmAddressing::JitLiteral:ArmAddressing::PageRelative);
    return x64_image(module);
#else
#if defined(__linux__)
    if(target.starts_with("aarch64-"))return aarch64_image(module,ArmABI::AAPCS64,ArmAddressing::PageRelative,debug);
#endif
    return x64_image(module,X64ABI::SystemV,debug);
#endif
}
std::string checked_target(const Options& options){if(!fast_supported())throw std::runtime_error("fast backend is unavailable on this platform; use --backend llvm or cpp");if(options.release)throw std::runtime_error("fast backend is a development backend; use --backend llvm for release optimization");const auto target=aot_target(options.target);
    if(!fast_target_supported(target))throw std::runtime_error("no Fast code generator for "+target+"; use --backend llvm or cpp");
    return target;}
}
namespace detail {
Image x64_image(const ir::Module& module,X64ABI abi,DebugRecorder* debug) { return Encoder(module,abi,debug).run(); }
}
bool fast_supported(){
#if defined(_M_ARM64EC) || defined(__arm64ec__)
    return false;
#elif defined(BOLT_USE_WINDOWS_ARM64_FAST) || ((defined(__x86_64__) || defined(_M_X64)) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__) || defined(__FreeBSD__))) || (defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__)))
    return true;
#else
    return false;
#endif
}
bool fast_target_supported(const std::string& target) {
    return fast_supported() && (target.empty() || target=="host" || target.starts_with("x86_64-") ||
           target.starts_with("aarch64-") || target.starts_with("arm64-"));
}
std::vector<std::uint8_t> fast_object(const ir::Module& module,const Options& options) {
    const auto target=checked_target(options);
#ifdef __linux__
    if(options.debug_symbols) {
        DebugRecorder positions;
        auto image=fast_image(module,target,false,&positions);
        auto sections=positions.encode();
        return elf_object(image,&sections);
    }
#endif
    auto image=fast_image(module,target);
#ifdef _WIN32
    return coff_object(image);
#elif defined(__APPLE__)
    constexpr auto version=__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__;
    constexpr auto deployment=((version/10000)<<16)|(((version/100)%100)<<8)|(version%100);
    return macho_object(image,deployment);
#else
    return elf_object(image);
#endif
}
namespace {
class RelocatableProgram final : public FastRecipe {
    Image object;
    std::shared_ptr<const LinkTargets> targets;
public:
    RelocatableProgram(Image value,const std::vector<Symbol>& symbols)
        :object(std::move(value)),targets(std::make_shared<LinkTargets>(object,symbols)) {}
    std::unique_ptr<JitImage> link() const override { return std::make_unique<FastImage>(object,targets); }
    std::unique_ptr<JitImage> link(const ir::Module& delta) const override { return std::make_unique<FastImage>(object,targets,&delta); }
};
}
std::shared_ptr<const FastRecipe> fast_recipe(const ir::Module& module,const std::vector<Symbol>& symbols,const Options& options) {
    const auto target=checked_target(options);
    if(target!=host_target())throw std::runtime_error("JIT compilation requires the host target");
    return std::make_shared<RelocatableProgram>(fast_image(module,target,true),symbols);
}
std::unique_ptr<JitImage> fast_jit(const ir::Module& module,const std::vector<Symbol>& symbols,const Options& options){
    const auto target=checked_target(options);
    if(target!=host_target())throw std::runtime_error("JIT compilation requires the host target");
    return std::make_unique<FastImage>(fast_image(module,target,true),symbols);
}
} // namespace bolt::native_backend
