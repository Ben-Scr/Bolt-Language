#include "Object.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>

namespace bolt::native_backend::detail {
namespace {
std::uint32_t field32(std::size_t value) {
    if(value>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("Windows COFF exceeds 4 GiB");
    return static_cast<std::uint32_t>(value);
}

struct Section {
    std::string name;
    std::uint32_t flags;
    Bytes content,relocations;
};

void relocation(Section& section,std::size_t offset,std::uint32_t symbol,std::uint16_t type) {
    section.relocations.u32(field32(offset));
    section.relocations.u32(symbol);
    section.relocations.u16(type);
}
}

std::vector<std::uint8_t> coff_object(const Image& image) {
    // IMAGE_REL_ARM64_* constants from the PE/COFF specification. Addends in
    // this format are stored at the relocation site, including section RVAs.
    const bool arm=image.machine==Machine::AArch64;
    const std::uint16_t addr32nb=arm?2:3,addr64=arm?14:1;
    constexpr std::uint16_t pagebase_rel21=4,pageoffset_12a=6,amd64_rel32=4;
    std::array<Section,4> sections={{{".text",0x60500020u,image.text,{}},
                                    {".data",0xc0400040u,image.data,{}},
                                    {".pdata",0x40300040u,{},{}},
                                    {".xdata",0x40300040u,{},{}}}};
    auto& text=sections[0];auto& pdata=sections[2];auto& xdata=sections[3];
    Bytes symbols,strings;strings.u32(0);
    std::map<std::string,std::uint32_t> indices;
    auto add_symbol=[&](const Definition& symbol){
        auto index=field32(symbols.size()/18);
        if(symbol.name.size()<=8) {
            const auto start=symbols.size();
            symbols.insert(symbols.end(),symbol.name.begin(),symbol.name.end());
            symbols.resize(start+8);
        }else {
            symbols.u32(0);symbols.u32(field32(strings.size()));strings.text(symbol.name);
        }
        symbols.u32(field32(symbol.offset));symbols.u16(static_cast<std::uint16_t>(symbol.section));
        symbols.u16(symbol.function?0x20u:0u);
        symbols.u8(!symbol.section||symbol.exported?2:3);symbols.u8(0);
        return index;
    };
    for(const auto& symbol:image.symbols)indices.emplace(symbol.name,add_symbol(symbol));
    const auto text_index=add_symbol({".text",1,0,text.content.size(),false,false});
    const auto unwind_index=add_symbol({".xdata",4,0,0,false,false});
    for(const auto& fixup:image.fixups) {
        auto& section=sections.at(fixup.section-1);
        auto symbol=indices.at(fixup.symbol);
        if(fixup.kind==RelocKind::Absolute64) {
            section.content.patch(fixup.offset,static_cast<std::uint64_t>(fixup.addend),8);
            relocation(section,fixup.offset,symbol,addr64);
        }else if(!arm && fixup.kind==RelocKind::Relative32) {
            section.content.patch(fixup.offset,static_cast<std::uint64_t>(fixup.addend),4);
            relocation(section,fixup.offset,symbol,amd64_rel32);
        }else if(arm && (fixup.kind==RelocKind::ArmPage21||fixup.kind==RelocKind::ArmLo12)&&fixup.addend==0) {
            relocation(section,fixup.offset,symbol,fixup.kind==RelocKind::ArmPage21?pagebase_rel21:pageoffset_12a);
        }else throw std::runtime_error("unsupported Windows COFF relocation");
    }
    for(const auto& record:image.unwind) {
        auto at=pdata.content.size();
        pdata.content.u32(field32(record.begin));
        relocation(pdata,at,text_index,addr32nb);
        // AMD64 records have a separate EndAddress. ARM64 keeps the function
        // length in its unwind header and only stores BeginAddress/UnwindData.
        if(!arm) {
            relocation(pdata,pdata.content.size(),text_index,addr32nb);
            pdata.content.u32(field32(record.end));
        }
        relocation(pdata,pdata.content.size(),unwind_index,addr32nb);
        pdata.content.u32(field32(xdata.content.size()));
        xdata.content.insert(xdata.content.end(),record.info.begin(),record.info.end());
    }
    strings.patch(0,field32(strings.size()),4);
    Bytes out;
    out.u16(arm?0xaa64:0x8664);out.u16(static_cast<std::uint16_t>(sections.size()));out.u32(0);out.u32(0);
    out.u32(field32(symbols.size()/18));out.u16(0);out.u16(0);
    out.resize(20+40*sections.size());
    for(std::size_t i=0;i<sections.size();++i) {
        auto& section=sections[i];
        const auto header=20+40*i;
        std::copy(section.name.begin(),section.name.end(),out.begin()+static_cast<std::ptrdiff_t>(header));
        out.align(4);
        out.patch(header+16,field32(section.content.size()),4);
        if(!section.content.empty()) {
            out.patch(header+20,field32(out.size()),4);
            out.insert(out.end(),section.content.begin(),section.content.end());
        }
        const auto relocations=section.relocations.size()/10;
        if(relocations) {
            out.align(4);out.patch(header+24,field32(out.size()),4);
            if(relocations>=65535) {
                section.flags|=0x01000000u; // IMAGE_SCN_LNK_NRELOC_OVFL
                out.u32(field32(relocations+1));out.u32(0);out.u16(0);
            }
            out.insert(out.end(),section.relocations.begin(),section.relocations.end());
            out.patch(header+32,std::min<std::size_t>(65535,relocations),2);
        }
        out.patch(header+36,section.flags,4);
    }
    out.align(4);out.patch(8,field32(out.size()),4);
    out.insert(out.end(),symbols.begin(),symbols.end());out.insert(out.end(),strings.begin(),strings.end());
    return out;
}
}
