#include "Dwarf.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace bolt::native_backend::detail {
namespace {
using namespace ir;

// Fixed-arity calls use independent banks of eight integer and floating-point registers.
struct Argument {
    unsigned reg;
    bool on_stack;
    bool fp;
    std::size_t offset;
};

std::vector<Argument> classify_arguments(const std::vector<Type>& types, std::size_t& stack_bytes, ArmABI abi) {
    std::vector<Argument> arguments;
    unsigned integers=0, floats=0;
    stack_bytes=0;
    for (Type type:types) {
        bool fp=floating(type);
        auto& next=fp?floats:integers;
        if (next<8) arguments.push_back({next++,false,fp,0});
        else {
            // On Darwin fixed-arity stack arguments use their natural size/alignment.
            const unsigned slot=abi==ArmABI::Apple?width(type):8;
            stack_bytes=(stack_bytes+slot-1)/slot*slot;
            arguments.push_back({0,true,fp,stack_bytes});
            stack_bytes+=slot;
        }
    }
    stack_bytes=(stack_bytes+7)/8*8;
    return arguments;
}

constexpr unsigned sp=31, zero=31, fp=29;
constexpr unsigned result=9, left=10, right=11, scratch=12, address_tmp=15, call_target=16;
constexpr unsigned eq=0, ne=1, hs=2, lo=3, mi=4, vs=6, vc=7, hi=8, ls=9, ge=10, lt=11, gt=12, le=13;

std::uint32_t registers(std::uint32_t base, unsigned dest, unsigned a, unsigned b) {
    return base | dest | (a<<5) | (b<<16);
}

std::size_t align_size(std::size_t n, std::size_t alignment) {
    return (n+alignment-1)/alignment*alignment;
}

class ArmEncoder {
    const Module& module;
    const ArmABI abi;
    const ArmAddressing addresses;
    DebugRecorder* debug;
    Image image;
    std::map<std::string,const External*> externals;
    std::map<std::string,std::string> imports;
    const Function* function=nullptr;
    std::size_t outgoing=0;
    std::map<Reg,std::size_t> allocations;
    std::vector<std::size_t> labels;
    std::vector<std::pair<std::size_t,Label>> branches;
    std::vector<std::size_t> windows_returns;

    void emit(std::uint32_t instruction) { image.text.u32(instruction); }
    Type type(Reg r) const { return function->registers.at(r); }
    std::size_t home(Reg r) const { return outgoing+(r-1)*8ull; }

    void immediate(unsigned dest,std::uint64_t value) {
        emit(0xd2800000u | (static_cast<unsigned>(value & 0xffff)<<5) | dest); // movz
        for (unsigned part=1;part<4;++part) {
            auto field=static_cast<unsigned>((value>>(16*part))&0xffff);
            if (field) emit(0xf2800000u | (part<<21) | (field<<5) | dest);       // movk
        }
    }

    void address_offset(unsigned dest,unsigned base,std::size_t offset) {
        if (offset<(1u<<24)) {
            unsigned upper=static_cast<unsigned>(offset>>12),low=static_cast<unsigned>(offset&4095);
            if (upper) {
                emit(0x91400000u | (upper<<10) | (base<<5) | dest);             // add #n, lsl #12
                base=dest;
            }
            if (low || !upper) emit(0x91000000u | (low<<10) | (base<<5) | dest);
        } else {
            immediate(dest,offset);
            emit(registers(0x8b206000u,dest,base,dest));                        // add Xd, Xn|SP, Xm, uxtx
        }
    }

    void memory(bool write,unsigned reg,unsigned base,std::size_t offset,unsigned size,bool floating_reg=false,bool sign=false) {
        if (offset%size || offset/size>4095) {
            address_offset(address_tmp,base,offset);
            base=address_tmp;
            offset=0;
        }
        unsigned size_log=size==8?3:size==4?2:size==2?1:0;
        std::uint32_t opcode=0x39000000u | (size_log<<30);
        if (floating_reg) opcode|=0x04000000u;
        if (!write) opcode|=(sign && size<8)?0x00800000u:0x00400000u;
        emit(opcode | (static_cast<unsigned>(offset/size)<<10) | (base<<5) | reg);
    }
    void load(unsigned dest,Reg r,bool sign=false) { memory(false,dest,sp,home(r),width(type(r)),false,sign); }
    void save(Reg r,unsigned src=result) { memory(true,src,sp,home(r),8); }
    void load_fp(unsigned dest,Reg r) { memory(false,dest,sp,home(r),width(type(r)),true); }
    void save_fp(Reg r,unsigned src=0) { memory(true,src,sp,home(r),width(type(r)),true); }

    void symbol_address(unsigned dest,const std::string& symbol) {
        if (addresses==ArmAddressing::JitLiteral) {
            // JIT code and RW data can be anywhere in the process. A PC-relative
            // literal load through an embedded pointer needs no near-data arena.
            // The branch skips the literal. Its relocation is applied before RX.
            emit(0x58000040u | dest);                                           // ldr Xt, .+8
            emit(0x14000003u);                                                  // b .+12
            image.fixups.push_back({1,image.text.size(),RelocKind::Absolute64,symbol,0});
            image.text.u64(0);
            return;
        }
        auto target=symbol;
        if (externals.contains(symbol)) {
            auto [it,inserted]=imports.try_emplace(symbol,".bolt.arm.import."+std::to_string(imports.size()));
            target=it->second;
            if (inserted) {
                image.data.align(8);
                auto at=image.data.size();
                image.symbols.push_back({target,2,at,8,false,false});
                image.fixups.push_back({2,at,RelocKind::Absolute64,symbol,0});
                image.data.u64(0);
            }
        }
        image.fixups.push_back({1,image.text.size(),RelocKind::ArmPage21,target,0});
        emit(0x90000000u | dest);                                               // adrp
        image.fixups.push_back({1,image.text.size(),RelocKind::ArmLo12,target,0});
        emit(0x91000000u | (dest<<5) | dest);                                   // add
        if (target!=symbol) memory(false,dest,dest,0,8);
    }

    void set_condition(unsigned dest,unsigned condition) {
        emit(0x1a9f07e0u | ((condition^1u)<<12) | dest);                       // cset Wd, cond
    }
    void cmp(unsigned a,unsigned b) { emit(registers(0xeb000000u,zero,a,b)); }
    void binary(const Instruction& i) {
        const auto op=i.op;
        bool checked=op==Op::CheckedAdd || op==Op::CheckedSub || op==Op::CheckedMul;
        bool sign=(checked && i.text=="s") || op==Op::SDiv || op==Op::SRem || op==Op::AShr;
        const auto size=width(type(i.result));
        load(left,i.args[0],sign);
        load(right,i.args[1],sign);
        std::uint32_t operation;
        switch(op) {
        case Op::Add: case Op::CheckedAdd: operation=checked&&size==8?0xab000000u:0x8b000000u;break;
        case Op::Sub: case Op::CheckedSub: operation=checked&&size==8?0xeb000000u:0xcb000000u;break;
        case Op::Mul: case Op::CheckedMul: operation=0x9b007c00u;break;
        case Op::SDiv: case Op::SRem: operation=0x9ac00c00u;break;
        case Op::UDiv: case Op::URem: operation=0x9ac00800u;break;
        case Op::And: operation=0x8a000000u;break;
        case Op::Or: operation=0xaa000000u;break;
        case Op::Xor: operation=0xca000000u;break;
        case Op::Shl: operation=size==8?0x9ac02000u:0x1ac02000u;break;
        case Op::LShr: operation=size==8?0x9ac02400u:0x1ac02400u;break;
        case Op::AShr: operation=size==8?0x9ac02800u:0x1ac02800u;break;
        default: throw std::runtime_error("unexpected ARM64 integer operation");
        }
        emit(registers(operation,result,left,right));
        if (op==Op::SRem || op==Op::URem)
            emit(registers(0x9b008000u | (left<<10),result,result,right));      // dividend - quotient * divisor
        save(i.result);
        if (!checked) return;
        unsigned overflow=ne;
        if (size<8) {
            // All narrow add/sub/mul operands fit in a 64-bit intermediate,
            // including the full product of two unsigned 32-bit inputs.
            load(scratch,i.result,sign);
            cmp(result,scratch);
        } else if (op==Op::CheckedMul) {
            emit(registers(sign?0x9b407c00u:0x9bc07c00u,scratch,left,right));   // smulh / umulh
            if (sign) emit(registers(0xeb80fc00u,zero,scratch,result));         // cmp high, low, asr #63
            else cmp(scratch,zero);
        } else overflow=sign?vs:(op==Op::CheckedAdd?hs:lo);
        set_condition(scratch,overflow);
        save(i.overflow,scratch);
    }

    void floating_op(const Instruction& i) {
        const std::uint32_t precision=type(i.result)==Type::F64?0x00400000u:0;
        load_fp(0,i.args[0]);
        if (i.op==Op::FNeg) emit(0x1e214000u | precision);
        else if (i.op==Op::FSqrt) emit(0x1e21c000u | precision); // fsqrt s0,s0 or d0,d0
        else {
            load_fp(1,i.args[1]);
            const std::uint32_t op=i.op==Op::FAdd?0x1e202800u:i.op==Op::FSub?0x1e203800u:
                                   i.op==Op::FMul?0x1e200800u:0x1e201800u;
            emit(registers(op|precision,0,0,1));
        }
        save_fp(i.result);
    }
    void compare(const Instruction& i) {
        const auto& p=i.text;
        unsigned condition;
        if (floating(type(i.args[0]))) {
            load_fp(0,i.args[0]);load_fp(1,i.args[1]);
            emit(0x1e212000u | (type(i.args[0])==Type::F64?0x00400000u:0));     // fcmp S0,S1 or D0,D1
            // FCMP produces NZCV=0011 for unordered. MI and LS exclude NaNs.
            if (p=="oeq") condition=eq;
            else if (p=="une" || p=="one") condition=ne;
            else if (p=="olt") condition=mi;
            else if (p=="ole") condition=ls;
            else if (p=="ogt") condition=gt;
            else if (p=="oge") condition=ge;
            else throw std::runtime_error("unsupported ARM64 floating predicate: "+p);
        } else {
            bool sign=!p.empty() && p.front()=='s';
            load(left,i.args[0],sign);load(right,i.args[1],sign);
            cmp(left,right);
            if (p=="eq") condition=eq;
            else if (p=="ne") condition=ne;
            else if (p=="slt") condition=lt;
            else if (p=="sle") condition=le;
            else if (p=="sgt") condition=gt;
            else if (p=="sge") condition=ge;
            else if (p=="ult") condition=lo;
            else if (p=="ule") condition=ls;
            else if (p=="ugt") condition=hi;
            else if (p=="uge") condition=hs;
            else throw std::runtime_error("unsupported ARM64 integer predicate: "+p);
        }
        set_condition(result,condition);
        if (p=="one") {
            set_condition(scratch,vc);
            emit(registers(0x8a000000u,result,result,scratch));
        }
        save(i.result);
    }
    void cast(const Instruction& i) {
        if (i.text=="fpext" || i.text=="fptrunc") {
            load_fp(0,i.args[0]);emit(i.text=="fpext"?0x1e22c000u:0x1e624000u);
            save_fp(i.result);
        } else if (i.text=="fptosi" || i.text=="fptoui") {
            load_fp(0,i.args[0]);
            // FCVTZS/FCVTZU Xd, S0/D0. The IR guard checked the target width.
            emit((i.text=="fptosi" ? 0x9e380000u : 0x9e390000u) |
                 (type(i.args[0])==Type::F64 ? 0x00400000u : 0) | result);
            save(i.result);
        } else if (i.text=="uitofp" || i.text=="sitofp") {
            load(result,i.args[0],i.text=="sitofp");
            emit((i.text=="sitofp"?0x9e220000u:0x9e230000u) |
                 (type(i.result)==Type::F64?0x00400000u:0) | (result<<5));
            save_fp(i.result);
        } else if (i.text=="sext" || i.text=="zext" || i.text=="trunc" ||
                   i.text=="bitcast" || i.text=="ptrtoint" || i.text=="inttoptr") {
            load(result,i.args[0],i.text=="sext");
            save(i.result);
        } else throw std::runtime_error("unsupported ARM64 cast: "+i.text);
    }
    void call(const Instruction& i) {
        bool indirect=i.op==Op::CallIndirect;
        std::vector<Type> types;
        for (auto it=i.args.begin()+indirect;it!=i.args.end();++it) types.push_back(type(*it));
        std::size_t stack_bytes=0;
        auto arguments=classify_arguments(types,stack_bytes,abi);
        const auto callee=externals.find(i.text);
        for (std::size_t n=0;n<arguments.size();++n) {
            auto r=i.args[n+indirect];
            const auto& arg=arguments[n];
            if (arg.on_stack) {
                load(scratch,r);
                memory(true,scratch,sp,arg.offset,abi==ArmABI::Apple?width(type(r)):8);
            } else if (arg.fp) load_fp(arg.reg,r);
            else load(arg.reg,r,abi==ArmABI::Apple && callee!=externals.end() && callee->second->signed_argument(n));
        }
        if (indirect) load(call_target,i.args[0]);
        else symbol_address(call_target,i.text);
        emit(0xd63f0000u | (call_target<<5));                                   // blr
        if (i.result) {
            if (floating(type(i.result))) save_fp(i.result);
            else save(i.result,0);
        }
    }
    void jump(Label label) {
        branches.emplace_back(image.text.size(),label);
        emit(0x14000000u);
    }
    void zero_block(const Instruction& i) {
        load(result,i.args[0]);
        auto size=i.immediate;
        if (size>=64) {
            immediate(scratch,size/8);
            emit(0xf8008400u | (result<<5) | zero);                             // str xzr,[Xn],#8
            emit(0xd1000400u | (scratch<<5) | scratch);                         // sub #1
            emit(0xb5ffffc0u | scratch);                                       // cbnz Xn, .-8
            size%=8;
        }
        std::size_t offset=0;
        for (unsigned unit:{8u,4u,2u,1u}) {
            while (size>=unit) { memory(true,zero,result,offset,unit);offset+=unit;size-=unit; }
        }
    }

    void instruction(const Instruction& i) {
        switch(i.op) {
        case Op::Constant: immediate(result,i.immediate);save(i.result);break;
        case Op::Address: symbol_address(result,i.text);save(i.result);break;
        case Op::Alloca: address_offset(result,sp,allocations.at(i.result));save(i.result);break;
        case Op::Offset:
            load(left,i.args[0]);
            if (i.args.size()==1 && i.immediate < (1u<<24)) {
                address_offset(result,left,i.immediate);
            } else {
                if (i.args.size()>1) load(right,i.args[1]);else immediate(right,i.immediate);
                emit(registers(0x8b000000u,result,left,right));
            }
            save(i.result);break;
        case Op::AtomicAdd: case Op::AtomicCompareExchange: {
            load(left,i.args[0]);load(right,i.args[1]);
            const bool cas=i.op==Op::AtomicCompareExchange;
            if(cas)load(scratch,i.args[2]);
            emit(0xc85ffc00u|(left<<5)|result); // ldaxr
            if(cas){
                emit(registers(0xeb000000u,zero,result,right)); // cmp
                emit(0x54000061u); // b.ne past stlxr and retry
            } else emit(registers(0x8b000000u,scratch,result,right));
            emit(0xc800fc00u|(address_tmp<<16)|(left<<5)|scratch); // stlxr
            emit(0x35000000u|((cas?0x7fffcu:0x7fffdu)<<5)|address_tmp); // cbnz retry
            save(i.result);break;
        }
        case Op::Load: load(left,i.args[0]);memory(false,result,left,0,width(type(i.result)));save(i.result);break;
        case Op::Store: load(left,i.args[0]);load(right,i.args[1]);memory(true,right,left,0,width(type(i.args[1])));break;
        case Op::CopyElement:
            load(left,i.args[0]);load(right,i.args[1]);
            for (unsigned pos=0;pos<i.immediate;) {
                const unsigned bytes=i.immediate-pos>=8 ? 8 : 1;
                memory(false,result,right,pos,bytes);memory(true,result,left,pos,bytes);
                pos+=bytes;
            }
            break;
        case Op::Zero: zero_block(i);break;
        case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv: case Op::FNeg: case Op::FSqrt: floating_op(i);break;
        case Op::Compare: compare(i);break;
        case Op::Cast: cast(i);break;
        case Op::Call: case Op::CallIndirect: call(i);break;
        case Op::Jump: jump(i.yes);break;
        case Op::Branch:
            load(result,i.args[0]);emit(0xb4000040u | result);                   // cbz Xn, .+8
            jump(i.yes);jump(i.no);break;
        case Op::Return:
            if (!i.args.empty()) {
                if (floating(type(i.args[0]))) load_fp(0,i.args[0]);
                else load(0,i.args[0]);
            }
            if(abi==ArmABI::Windows) {
                windows_returns.push_back(image.text.size());emit(0x14000000u);
            }else epilogue();
            break;
        case Op::Select:
            load(result,i.args[0]);cmp(result,zero);
            load(left,i.args[1]);load(right,i.args[2]);
            emit(registers(0x9a800000u | (ne<<12),result,left,right));save(i.result);break;
        default: binary(i);break;
        }
    }

    void epilogue() {
        emit(0x910003bfu);emit(0xa8c17bfdu);emit(0xd65f03c0u); // mov sp,fp; ldp fp,lr,[sp],#16; ret
    }

    Bytes windows_frame(std::size_t frame) {
        // The Windows ARM64 unwinder accounts for each prologue instruction.
        // Probe guard pages through volatile x16, leaving sp stable until the
        // single recorded allocation. Probe instructions are unwind no-ops.
        Bytes codes;
        if(frame==0)return codes;
        const auto units=frame/16;
        if(units>0xffffffu)throw std::runtime_error("Windows ARM64 Fast stack allocation exceeds SEH range");
        const auto before=image.text.size();
        if(frame>=4096) {
            immediate(15,frame);address_offset(16,sp,0);immediate(17,frame>>12);
            emit(0xd1400610u);                         // sub x16,x16,#4096
            emit(0xf940021fu);                         // ldr xzr,[x16]
            emit(0xf1000631u);                         // subs x17,x17,#1
            emit(0x54ffffa1u);                         // b.ne .-12
            if(auto tail=static_cast<unsigned>(frame&4095)) {
                emit(0xd1000210u | (tail<<10));emit(0xf940021fu);
            }
            emit(0xcb2f63ffu);                         // sub sp,sp,x15, uxtx
        }else emit(0xd10003ffu | (static_cast<unsigned>(frame)<<10));
        if(units<32)codes.u8(static_cast<std::uint8_t>(units));
        else if(units<2048) {
            codes.u8(static_cast<std::uint8_t>(0xc0u|(units>>8)));codes.u8(static_cast<std::uint8_t>(units));
        }else {
            codes.u8(0xe0);codes.u8(static_cast<std::uint8_t>(units>>16));
            codes.u8(static_cast<std::uint8_t>(units>>8));codes.u8(static_cast<std::uint8_t>(units));
        }
        const auto probe_instructions=(image.text.size()-before)/4-1;
        codes.insert(codes.end(),probe_instructions,0xe3); // nop
        return codes;
    }

    void windows_unwind(std::size_t start,Bytes codes) {
        const auto epilogue_at=image.text.size();
        for(auto from:windows_returns) {
            auto words=(epilogue_at-from)/4;
            if(words>=(1u<<25))throw std::runtime_error("Windows ARM64 return branch exceeds 128 MiB");
            image.text.patch(from,0x14000000u|static_cast<std::uint32_t>(words),4);
        }
        epilogue();
        const auto length=(image.text.size()-start)/4;
        // The single final epilogue shares the last three unwind operations
        // with the prologue. A full xdata record also covers partial prologues.
        const auto epilogue_index=codes.size();
        codes.u8(0xe1);codes.u8(0x81);codes.u8(0xe4); // set_fp, save_fplr_x #16, end
        codes.align(4,0xe3);
        if(length>=1u<<18 || epilogue_index>=32 || codes.size()/4>=32)
            throw std::runtime_error("function exceeds Windows ARM64 Fast unwind-record limits; use LLVM");
        Bytes info;
        info.u32(static_cast<std::uint32_t>(length|(1u<<21)|(epilogue_index<<22)|((codes.size()/4)<<27)));
        info.insert(info.end(),codes.begin(),codes.end());
        image.unwind.push_back({start,image.text.size(),std::move(info)});
    }

    void compile(const Function& fn) {
        function=&fn; allocations.clear(); branches.clear(); windows_returns.clear();labels.resize(fn.blocks.size());
        outgoing=0;
        for (const auto& block:fn.blocks) for (const auto& i:block.code) {
            if (i.op!=Op::Call && i.op!=Op::CallIndirect) continue;
            std::vector<Type> types;
            for (auto it=i.args.begin()+(i.op==Op::CallIndirect);it!=i.args.end();++it) types.push_back(type(*it));
            std::size_t bytes=0;
            classify_arguments(types,bytes,abi);
            outgoing=std::max(outgoing,bytes);
        }
        std::size_t frame=outgoing+(fn.registers.size()-1)*8ull;
        for (const auto& block:fn.blocks) for (const auto& i:block.code) if (i.op==Op::Alloca) {
            if (frame>(1u<<30) || i.immediate>(1u<<30)-frame) throw std::runtime_error("ARM64 stack frame is too large");
            allocations[i.result]=frame;
            frame=align_size(frame+i.immediate,8);
        }
        frame=align_size(frame,16);
        image.text.align(16);
        const auto start=image.text.size(),definition=image.symbols.size();
        if(debug)debug->begin(fn,start);
        image.symbols.push_back({fn.name,1,start,0,fn.exported,true});
        emit(0xa9bf7bfdu);emit(0x910003fdu);                                    // stp fp,lr,[sp,#-16]!; mov fp,sp
        Bytes windows_codes;
        if(abi==ArmABI::Windows)windows_codes=windows_frame(frame);
        else if (frame>=4096) {
            // Probe large frames a page at a time. A loop keeps generated code bounded.
            immediate(scratch,frame>>12);
            emit(0xd14007ffu);                                                  // sub sp,sp,#4096
            emit(0xf90003ffu);                                                  // str xzr,[sp]
            emit(0xd1000400u | (scratch<<5) | scratch);
            emit(0xb5ffffa0u | scratch);                                       // cbnz Xn, .-12
        }
        if(abi!=ArmABI::Windows)if(auto tail=static_cast<unsigned>(frame&4095))emit(0xd10003ffu|(tail<<10));
        std::size_t incoming_stack=0;
        auto incoming=classify_arguments(fn.parameters,incoming_stack,abi);
        for (std::size_t j=0;j<incoming.size();++j) {
            auto r=static_cast<Reg>(j+1);
            const auto& arg=incoming[j];
            if (arg.on_stack) { memory(false,scratch,fp,16+arg.offset,abi==ArmABI::Apple?width(type(r)):8);save(r,scratch); }
            else if (arg.fp) save_fp(r,arg.reg);
            else save(r,arg.reg);
        }
        for (std::size_t n=0;n<fn.blocks.size();++n) {
            labels[n]=image.text.size();
            for (const auto& i:fn.blocks[n].code) {
                if(debug)debug->position(i.at,image.text.size());
                instruction(i);
            }
        }
        for (const auto& [at,label]:branches) {
            auto words=(static_cast<std::int64_t>(labels.at(label))-static_cast<std::int64_t>(at))/4;
            if (words<-(1ll<<25) || words>=(1ll<<25)) throw std::runtime_error("ARM64 branch exceeds 128 MiB");
            image.text.patch(at,0x14000000u | (static_cast<std::uint32_t>(words)&0x03ffffffu),4);
        }
        if(abi==ArmABI::Windows)windows_unwind(start,std::move(windows_codes));
        image.symbols[definition].size=image.text.size()-start;
        if(debug)debug->end(image.text.size());
    }
public:
    ArmEncoder(const Module& m,ArmABI target,ArmAddressing mode,DebugRecorder* positions):module(m),abi(target),addresses(mode),debug(positions) {}
    Image encode() {
        verify(module);
        image.machine=Machine::AArch64;
        for (const auto& e:module.externals) {
            externals.emplace(e.name,&e);
            image.symbols.push_back({e.name,0,0,0,true,true});
        }
        for (const auto& g:module.globals) {
            image.data.align(g.alignment);
            auto at=image.data.size();
            image.symbols.push_back({g.name,2,at,g.bytes.size(),false,false});
            image.data.insert(image.data.end(),g.bytes.begin(),g.bytes.end());
            for (const auto& r:g.relocations) image.fixups.push_back({2,at+r.offset,RelocKind::Absolute64,r.symbol,r.addend});
        }
        for (const auto& fn:module.functions) compile(fn);
        return std::move(image);
    }
};
}

void apply_arm_address_fixup(std::uint8_t* at,RelocKind kind,std::uintptr_t target,std::uintptr_t location) {
    std::uint32_t instruction;
    std::memcpy(&instruction,at,sizeof(instruction));
    if (kind==RelocKind::ArmPage21) {
        const auto pages=static_cast<std::int64_t>(target>>12)-
                         static_cast<std::int64_t>(location>>12);
        if (pages<-(1ll<<20) || pages>=(1ll<<20)) throw std::runtime_error("ARM64 page relocation exceeds 4 GiB");
        auto immediate=static_cast<std::uint32_t>(pages)&0x1fffff;
        instruction|=((immediate&3)<<29)|((immediate>>2)<<5);
    } else if (kind==RelocKind::ArmLo12) instruction|=static_cast<std::uint32_t>(target&0xfff)<<10;
    else throw std::runtime_error("unexpected ARM64 page relocation");
    std::memcpy(at,&instruction,sizeof(instruction));
}

Image aarch64_image(const ir::Module& module,ArmABI abi,ArmAddressing mode,DebugRecorder* debug) { return ArmEncoder(module,abi,mode,debug).encode(); }
}
