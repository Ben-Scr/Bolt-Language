#pragma once
// In-process linker for a single LLVM-generated Windows ARM64 COFF object.
// The format and relocation constants follow Microsoft's PE/COFF specification.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bolt::native_backend::coff_arm64 {
using Bytes=std::span<const std::uint8_t>;
inline void require(bool condition,const char* what) {
    if(!condition)throw std::runtime_error(std::string("ARM64 COFF JIT: ")+what);
}
inline Bytes slice(Bytes bytes,std::size_t at,std::size_t size) {
    require(at<=bytes.size() && size<=bytes.size()-at,"truncated record");
    return bytes.subspan(at,size);
}
template<class T> T read(const std::uint8_t* p) {T value;std::memcpy(&value,p,sizeof(value));return value;}
template<class T> void write(std::uint8_t* p,T value) {std::memcpy(p,&value,sizeof(value));}
inline std::size_t aligned(std::size_t v,std::size_t alignment) {
    require(alignment && (alignment&(alignment-1))==0,"invalid alignment");
    require(v<=std::numeric_limits<std::size_t>::max()-(alignment-1),"image is too large");
    return (v+alignment-1)&~(alignment-1);
}
inline std::int64_t sext(std::uint32_t value,unsigned bits) {
    return (value&(1u<<(bits-1)))?static_cast<std::int64_t>(value)-(std::int64_t{1}<<bits):value;
}
inline std::uint32_t signed_bits(std::int64_t value,unsigned bits) {
    auto limit=std::int64_t{1}<<(bits-1);
    require(value>=-limit && value<limit,"instruction displacement is out of range");
    return static_cast<std::uint32_t>(value)&((1u<<bits)-1);
}

struct Region {
    std::size_t offset=0,size=0;
    bool executable=false,writable=false;
};
struct Section {
    std::string name;
    Region region;
    Bytes raw,relocations;
    bool mapped=false;
};
struct Symbol {
    std::string name;
    std::uint32_t offset=0;
    std::int16_t section=0;
    std::uint32_t alias=UINT32_MAX;
    bool present=false;
};
struct Layout {
    std::vector<Section> sections;
    std::vector<Symbol> symbols;
    std::unordered_map<std::string,std::size_t> definitions;
    Region thunks,imports;
    std::size_t bytes=0,page=0;

    Layout(Bytes object,std::size_t page_size):page(page_size) {
        auto head=slice(object,0,20).data();
        require(read<std::uint16_t>(head)==0xaa64 && !read<std::uint16_t>(head+16),"expected a regular ARM64 COFF object");
        auto section_count=read<std::uint16_t>(head+2);
        auto sym_at=read<std::uint32_t>(head+8),sym_count=read<std::uint32_t>(head+12);
        auto table=slice(object,sym_at,static_cast<std::size_t>(sym_count)*18);
        auto str_at=static_cast<std::size_t>(sym_at)+table.size();
        auto str_size=read<std::uint32_t>(slice(object,str_at,4).data());
        require(str_size>=4,"invalid COFF string table");
        auto strings=slice(object,str_at,str_size);
        auto long_name=[&](std::uint32_t offset) {
            require(offset>=4&&offset<strings.size(),"bad string table index");
            const char* start=reinterpret_cast<const char*>(strings.data()+offset);
            const char* end=static_cast<const char*>(std::memchr(start,0,strings.size()-offset));
            require(end!=nullptr,"unterminated symbol name");return std::string(start,end);
        };
        auto short_name=[](const std::uint8_t* field){
            std::size_t count=0;while(count<8&&field[count])++count;
            return std::string(reinterpret_cast<const char*>(field),count);
        };
        sections.reserve(section_count);
        for(std::size_t i=0;i<section_count;++i) {
            const auto* h=slice(object,20+i*40,40).data();
            auto& section=sections.emplace_back();section.name=short_name(h);
            if(!section.name.empty()&&section.name.front()=='/'){
                auto offset=std::stoul(section.name.substr(1));
                require(offset<=UINT32_MAX,"section name index overflows");
                section.name=long_name(static_cast<std::uint32_t>(offset));
            }
            auto flags=read<std::uint32_t>(h+36);
            // Linker directives, debugging records and address-significance tables do not occupy process memory.
            section.mapped=(flags&0xe0)!=0 && !(flags&(0x200|0x800|0x02000000));
            if(!section.mapped)continue;
            auto count=read<std::uint16_t>(h+32);
            auto reloc_at=read<std::uint32_t>(h+24);
            std::uint32_t n=count;
            if(flags&0x01000000){
                require(count==UINT16_MAX,"invalid extended relocation count");
                auto overflow=slice(object,reloc_at,10);
                n=read<std::uint32_t>(overflow.data());
                require(n>0,"invalid relocation count");
                section.relocations=slice(object,static_cast<std::size_t>(reloc_at)+10,static_cast<std::size_t>(n-1)*10);
            }else section.relocations=slice(object,reloc_at,static_cast<std::size_t>(n)*10);
            auto size=read<std::uint32_t>(h+16),raw_at=read<std::uint32_t>(h+20);
            if(raw_at&&size)section.raw=slice(object,raw_at,size);
            auto order=(flags>>20)&15;
            require(order<15,"invalid COFF section alignment");
            auto alignment=std::max(page,order?std::size_t{1}<<(order-1):1);
            bytes=aligned(bytes,alignment);
            section.region={bytes,size,(flags&0x20000000)!=0,(flags&0x80000000)!=0};
            require(!(section.region.executable&&section.region.writable),"writable executable section");
            bytes+=size;
        }
        symbols.resize(sym_count);
        for(std::size_t i=0;i<sym_count;){
            const auto* s=table.data()+i*18;auto& symbol=symbols[i];
            symbol.name=read<std::uint32_t>(s)?short_name(s):long_name(read<std::uint32_t>(s+4));
            symbol.offset=read<std::uint32_t>(s+8);symbol.section=read<std::int16_t>(s+12);symbol.present=true;
            auto auxiliary=s[17];require(auxiliary<sym_count-i,"invalid auxiliary symbol count");
            if(s[16]==105&&auxiliary)symbol.alias=read<std::uint32_t>(s+18);
            if(symbol.section>0){
                require(static_cast<std::size_t>(symbol.section)<=sections.size(),"invalid symbol section");
                if(s[16]==2)definitions.try_emplace(symbol.name,i);
            }
            i+=1+auxiliary;
        }
        // An undefined function may need a branch island and a read-only import slot.
        // Compact slots keep import overhead proportional to actual undefined symbols.
        for(const auto& sym:symbols)if(sym.present&&sym.section==0)++undefined_count;
        bytes=aligned(bytes,page);thunks={bytes,undefined_count*16,true,false};
        bytes=aligned(bytes+thunks.size,page);imports={bytes,undefined_count*8,false,false};
        bytes=aligned(bytes+imports.size,page);
        require(bytes&&bytes<=UINT32_MAX,"image exceeds Windows RVA range");
    }
private:
    std::size_t undefined_count=0;
};

inline void relocate(std::uint8_t* site,std::uint16_t kind,std::uintptr_t target,std::uintptr_t image,
                     std::uint32_t section_offset,std::uint16_t section_number) {
    const auto pc=reinterpret_cast<std::uintptr_t>(site);
    if(kind==0)return;
    if(kind==14){write(site,read<std::uint64_t>(site)+target);return;}
    if(kind==13){write(site,static_cast<std::uint16_t>(read<std::uint16_t>(site)+section_number));return;}
    auto ins=read<std::uint32_t>(site);
    switch(kind) {
    case 1:case 2:case 8: {
        auto value=(kind==1?target:kind==2?target-image:section_offset)+static_cast<std::uint64_t>(ins);
        require(value<=UINT32_MAX,"32-bit relocation overflow");
        ins=static_cast<std::uint32_t>(value);break;
    }
    case 3:case 15:case 16:{ // B/BL, conditional branch, test-and-branch.
        unsigned bits=kind==3?26:kind==15?19:14,shift=kind==3?0:5;
        auto mask=(1u<<bits)-1;
        auto d=static_cast<std::int64_t>(target)-static_cast<std::int64_t>(pc)+sext((ins>>shift)&mask,bits)*4;
        require(d%4==0,"unaligned ARM64 branch");ins=(ins&~(mask<<shift))|(signed_bits(d/4,bits)<<shift);break;
    }
    case 4:case 5:{ // ADRP and ADR use interleaved immlo and immhi fields.
        auto imm=((ins>>29)&3)|((ins>>3)&0x1ffffc);
        const auto s=static_cast<std::int64_t>(target),p=static_cast<std::int64_t>(pc);
        // COFF encodes the input addend in bytes even for ADRP, whose final immediate is in pages.
        const auto adjusted=s+sext(imm,21);
        auto delta=kind==4?(adjusted>>12)-(p>>12):adjusted-p;
        auto result=signed_bits(delta,21);
        ins=(ins&~0x60ffffe0u)|((result&3)<<29)|((result&0x1ffffc)<<3);break;
    }
    case 6:case 7:case 9:case 10:case 11:{
        bool memory=kind==7||kind==11;
        unsigned scale=0;
        if(memory) {
            scale=ins>>30;
            if((ins&0x04800000)==0x04800000)scale+=4; // SIMD LDR/STR Q is scaled by 16.
        }else require((ins&(1u<<22))==0 || kind==10,"unexpected shifted low12 add");
        auto addend=((ins>>10)&0xfffu)<<scale;
        auto value=kind>=9?section_offset:target;
        if(kind==10){value>>=12;require(value<=4095,"section-relative high12 overflow");}
        value+=addend;
        auto offset=value&4095u;
        require((offset&((1u<<scale)-1))==0,"unaligned load/store relocation");
        ins=(ins&~0x3ffc00u)|(static_cast<std::uint32_t>(offset>>scale)<<10);break;
    }
    case 17:{
        auto d=static_cast<std::int64_t>(target)-static_cast<std::int64_t>(pc+4)+static_cast<std::int32_t>(ins);
        require(d>=INT32_MIN&&d<=INT32_MAX,"REL32 out of range");ins=static_cast<std::uint32_t>(d);break;
    }
    default:throw std::runtime_error("ARM64 COFF JIT: unsupported relocation "+std::to_string(kind));
    }
    write(site,ins);
}

template<class Lookup>
void link(const Layout& layout,std::uint8_t* memory,Lookup lookup) {
    for(const auto& section:layout.sections)if(section.mapped&&!section.raw.empty())
        std::memcpy(memory+section.region.offset,section.raw.data(),section.raw.size());
    std::vector<std::uintptr_t> bound(layout.symbols.size());
    std::size_t used=0;
    auto image=reinterpret_cast<std::uintptr_t>(memory);
    for(const auto& section:layout.sections){
        if(!section.mapped)continue;
        for(std::size_t r=0;r<section.relocations.size();r+=10){
            auto relocation=section.relocations.data()+r;
            auto offset=read<std::uint32_t>(relocation),index=read<std::uint32_t>(relocation+4);
            auto kind=read<std::uint16_t>(relocation+8);
            if(!kind)continue;
            unsigned width=kind==14?8:kind==13?2:4;
            require(offset<=section.region.size && width<=section.region.size-offset,"relocation outside section");
            auto* site=memory+section.region.offset+offset;
            require(index<layout.symbols.size()&&layout.symbols[index].present,"relocation references invalid symbol");
            const auto* symbol=&layout.symbols[index];
            if(symbol->alias!=UINT32_MAX){
                index=symbol->alias;
                require(index<layout.symbols.size()&&layout.symbols[index].present,"invalid weak external alias");
                symbol=&layout.symbols[index];
            }
            std::uintptr_t target=0;
            if(symbol->section>0){
                const auto& dest=layout.sections[symbol->section-1];
                require(dest.mapped && symbol->offset<=dest.region.size,"symbol outside mapped section");
                target=image+dest.region.offset+symbol->offset;
            }else if(symbol->section==-1)target=symbol->offset;
            else {
                require(symbol->section==0&&!symbol->offset,"unsupported undefined data symbol");
                if(!bound[index]){
                    const auto& name=symbol->name;const bool import=name.starts_with("__imp_");
                    auto address=lookup(import?name.substr(6):name);
                    if(!address)throw std::runtime_error("ARM64 COFF JIT: unresolved native symbol "+name);
                    if(import){
                        auto slot=memory+layout.imports.offset+used*8;
                        write(slot,static_cast<std::uint64_t>(address));bound[index]=reinterpret_cast<std::uintptr_t>(slot);
                    }else{
                        auto stub=memory+layout.thunks.offset+used*16;
                        write(stub,0x58000050u);write(stub+4,0xd61f0200u); // ldr x16, #8; br x16
                        write(stub+8,static_cast<std::uint64_t>(address));bound[index]=reinterpret_cast<std::uintptr_t>(stub);
                    }
                    ++used;
                }
                target=bound[index];
            }
            relocate(site,kind,target,image,symbol->offset,static_cast<std::uint16_t>(symbol->section));
        }
    }
}
} // namespace bolt::native_backend::coff_arm64
