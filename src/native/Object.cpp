#include "Dwarf.hpp"
#include <array>
#include <algorithm>
#include <limits>
#include <map>

namespace bolt::native_backend::detail {
namespace {
std::uint32_t narrow(std::size_t n) {
    if(n>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("native object exceeds 4 GiB");
    return static_cast<std::uint32_t>(n);
}
void append(Bytes& output,const Bytes& input){output.insert(output.end(),input.begin(),input.end());}
std::vector<Definition> ordered_symbols(const Image& image){
    auto symbols=image.symbols;
    std::stable_sort(symbols.begin(),symbols.end(),[](const auto& a,const auto& b){return (a.section!=0&&!a.exported)>(b.section!=0&&!b.exported);});
    return symbols;
}
}
std::vector<std::uint8_t> elf_object(const Image& image,DwarfSections* debug) {
    // ELF64 little-endian relocatable. Keep external addresses in writable
    // data slots; executable text therefore remains position-independent.
    struct Section { std::string name; std::uint32_t type=1; std::uint64_t flags=0,alignment=1,entry=0; std::uint32_t link=0,info=0; Bytes bytes; std::uint64_t offset=0; };
    const bool have_debug=debug && !debug->line.empty();
    std::vector<Section> sections(have_debug?14:9);
    sections[0].type=0;
    sections[1]={".text",1,6,16,0,0,0,image.text,0};
    sections[2]={".data",1,3,8,0,0,0,image.data,0};
    sections[3]={".rela.text",4,0,8,24,5,1,{},0};
    sections[4]={".rela.data",4,0,8,24,5,2,{},0};
    sections[5]={".symtab",2,0,8,24,6,1,{},0};
    sections[6]={".strtab",3,0,1,0,0,0,{},0};
    sections[7]={".shstrtab",3,0,1,0,0,0,{},0};
    sections[8]={".note.GNU-stack",1,0,1,0,0,0,{},0};
    if(have_debug) {
        sections[9]={".debug_abbrev",1,0,1,0,0,0,std::move(debug->abbrev),0};
        sections[10]={".debug_info",1,0,1,0,0,0,std::move(debug->info),0};
        sections[11]={".debug_line",1,0,1,0,0,0,std::move(debug->line),0};
        sections[12]={".rela.debug_info",4,0,8,24,5,10,{},0};
        sections[13]={".rela.debug_line",4,0,8,24,5,11,{},0};
    }
    auto& strings=sections[6].bytes;strings.u8(0);
    auto& table=sections[5].bytes;table.resize(24);
    if(have_debug) {
        // Reference section symbols so linked objects can freely concatenate
        // their .text, .debug_abbrev and .debug_line sections.
        std::array<std::uint32_t,3> indices{};
        unsigned i=0;
        for(unsigned section:{1u,9u,11u}) {
            indices[i++]=narrow(table.size()/24);
            table.u32(0);table.u8(3);table.u8(0);table.u16(static_cast<std::uint16_t>(section));
            table.u64(0);table.u64(0);++sections[5].info;
        }
        auto relocate=[&](const auto& entries,Bytes& bytes) {
            for(const auto& r:entries) {
                const bool absolute64=r.width==8;
                const unsigned type=image.machine==Machine::AArch64?(absolute64?257:258):(absolute64?1:10);
                const auto symbol=indices.at(static_cast<std::size_t>(r.target));
                bytes.u64(r.offset);bytes.u64((std::uint64_t(symbol)<<32)|type);bytes.u64(r.addend);
            }
        };
        relocate(debug->info_relocations,sections[12].bytes);
        relocate(debug->line_relocations,sections[13].bytes);
    }
    std::map<std::string,std::uint32_t> indices;
    auto symbols=ordered_symbols(image);
    for(const auto& s:symbols){
        indices[s.name]=narrow(table.size()/24);
        const auto name=narrow(strings.size());strings.text(s.name);
        table.u32(name);bool global=s.section==0||s.exported;
        table.u8(static_cast<std::uint8_t>((global?16:0)|(s.function?2:1)));
        table.u8(0);table.u16(static_cast<std::uint16_t>(s.section));table.u64(s.offset);table.u64(s.size);
        if(!global)++sections[5].info;
    }
    for(const auto& r:image.fixups){
        auto& bytes=sections.at(r.section+2).bytes;
        bytes.u64(r.offset);
        std::uint32_t type;
        if (image.machine==Machine::AArch64) {
            switch(r.kind) {
            case RelocKind::Absolute64: type=257;break; // R_AARCH64_ABS64
            case RelocKind::ArmPage21: type=275;break;  // R_AARCH64_ADR_PREL_PG_HI21
            case RelocKind::ArmLo12: type=277;break;    // R_AARCH64_ADD_ABS_LO12_NC
            default: throw std::runtime_error("unexpected ARM64 ELF relocation");
            }
        } else type=r.kind==RelocKind::Absolute64?1u:2u;
        bytes.u64((std::uint64_t(indices.at(r.symbol))<<32)|type);
        bytes.u64(static_cast<std::uint64_t>(r.addend+(r.kind==RelocKind::Relative32?-4:0)));
    }
    auto& names=sections[7].bytes;names.u8(0);std::vector<std::uint32_t> name_offsets(sections.size());
    for(std::size_t i=1;i<sections.size();++i){name_offsets[i]=narrow(names.size());names.text(sections[i].name);}
    Bytes output;output.resize(64);output[0]=0x7f;output[1]='E';output[2]='L';output[3]='F';output[4]=2;output[5]=1;output[6]=1;
#if defined(__FreeBSD__)
    output[7]=9; // ELFOSABI_FREEBSD, matching the host Clang object format.
#endif
    output.patch(16,1,2);output.patch(18,image.machine==Machine::AArch64?183:62,2);output.patch(20,1,4);output.patch(52,64,2);output.patch(58,64,2);output.patch(60,sections.size(),2);output.patch(62,7,2);
    for(std::size_t j=1;j<sections.size();++j){auto& s=sections[j];output.align(static_cast<std::size_t>(s.alignment));s.offset=output.size();append(output,s.bytes);}
    output.align(8);output.patch(40,output.size(),8);
    for(std::size_t j=0;j<sections.size();++j){const auto& s=sections[j];output.u32(name_offsets[j]);output.u32(s.type);output.u64(s.flags);output.u64(0);output.u64(s.offset);output.u64(s.bytes.size());output.u32(s.link);output.u32(s.info);output.u64(j?s.alignment:0);output.u64(s.entry);}
    return output;
}
// Mach-O uses a symbol index even for relocations to named local
// definitions. Only the symbol table's N_EXT bit controls external visibility.
std::vector<std::uint8_t> macho_object(const Image& image, std::uint32_t deployment) {
    constexpr std::uint32_t header_size=32, segment_command_size=72, section_size=80;
    constexpr std::uint32_t segment_size=segment_command_size+2*section_size;
    constexpr std::uint32_t commands_size=segment_size+24+24+80;
    struct Section {
        const char* name;
        const char* segment;
        Bytes bytes;
        unsigned alignment,flags;
        std::uint64_t address=0;
        std::uint32_t file_offset=0,reloc_offset=0;
        std::vector<const Fixup*> relocations;
    };
    Section sections[]={{"__text","__TEXT",image.text,4,0x80000400,0,0,0,{}},
                        {"__data","__DATA",image.data,3,0,0,0,0,{}}};
    auto symbols=image.symbols;
    auto group=[](const Definition& symbol){return symbol.section?(symbol.exported?1:0):2;};
    std::stable_sort(symbols.begin(),symbols.end(),[&](const auto& a,const auto& b){return group(a)<group(b);});
    std::map<std::string,std::uint32_t> indices;
    for(std::size_t i=0;i<symbols.size();++i){
        if(i>=0x1000000)throw std::runtime_error("Mach-O object exceeds 24-bit relocation symbol index");
        indices.emplace(symbols[i].name,narrow(i));
    }
    for(const auto& relocation:image.fixups){
        auto& section=sections[relocation.section-1];
        section.relocations.push_back(&relocation);
        if(relocation.kind==RelocKind::Relative32 || relocation.kind==RelocKind::Absolute64) {
            const auto width=relocation.kind==RelocKind::Absolute64?8u:4u;
            section.bytes.patch(relocation.offset,static_cast<std::uint64_t>(relocation.addend),width);
        } else if(relocation.addend!=0) {
            throw std::runtime_error("Mach-O ARM64 page relocations require an unmodified symbol address");
        }
    }
    Bytes out;
    out.resize(header_size+commands_size);
    out.align(16);
    const auto payload_start=narrow(out.size());
    for(auto& section:sections){
        out.align(std::size_t(1)<<section.alignment);
        section.file_offset=narrow(out.size());
        section.address=section.file_offset-payload_start;
        append(out,section.bytes);
    }
    const auto payload_size=narrow(out.size())-payload_start;
    out.align(8);
    for(auto& section:sections){
        std::sort(section.relocations.begin(),section.relocations.end(),[](auto a,auto b){return a->offset>b->offset;});
        if(!section.relocations.empty())section.reloc_offset=narrow(out.size());
        for(const auto* relocation:section.relocations){
            // X86_64_RELOC_SIGNED/BRANCH or ARM64_RELOC_PAGE21/PAGEOFF12.
            // Both architectures use UNSIGNED for 64-bit absolute addresses.
            const auto kind=relocation->kind;
            const bool relative=kind==RelocKind::Relative32 || kind==RelocKind::ArmPage21;
            const unsigned length=kind==RelocKind::Absolute64?3u:2u;
            const unsigned type=kind==RelocKind::Relative32?(relocation->branch?2u:1u):
                                kind==RelocKind::ArmPage21?3u:kind==RelocKind::ArmLo12?4u:0u;
            out.u32(narrow(relocation->offset));
            out.u32(indices.at(relocation->symbol)|(relative?1u<<24:0u)|
                    (length<<25)|(1u<<27)|(type<<28));
        }
    }
    const auto symoff=narrow(out.size());
    Bytes strings;
    strings.u8(0);
    unsigned counts[3]={};
    for(const auto& symbol:symbols){
        out.u32(narrow(strings.size()));
        strings.text("_"+symbol.name);
        const auto category=group(symbol);
        ++counts[category];
        out.u8(static_cast<std::uint8_t>((symbol.section?0x0e:0)|(category?1:0)));
        out.u8(static_cast<std::uint8_t>(symbol.section));
        out.u16(0);
        out.u64(symbol.section?sections[symbol.section-1].address+symbol.offset:0);
    }
    const auto stroff=narrow(out.size());
    append(out,strings);

    Bytes commands;
    commands.u32(0xfeedfacf); // MH_MAGIC_64
    const bool arm=image.machine==Machine::AArch64;
    commands.u32(arm?0x0100000c:0x01000007);commands.u32(arm?0:3);
    commands.u32(1);commands.u32(4);commands.u32(commands_size);commands.u32(0);commands.u32(0);
    auto name16=[&](const char* name){
        const auto pos=commands.size();
        commands.text(name);
        commands.resize(pos+16,0);
    };
    commands.u32(0x19);commands.u32(segment_size);name16(""); // LC_SEGMENT_64
    commands.u64(0);commands.u64(payload_size);commands.u64(payload_start);commands.u64(payload_size);
    commands.u32(7);commands.u32(7);commands.u32(2);commands.u32(0);
    for(const auto& section:sections){
        name16(section.name);name16(section.segment);
        commands.u64(section.address);commands.u64(section.bytes.size());
        commands.u32(section.file_offset);commands.u32(section.alignment);commands.u32(section.reloc_offset);
        commands.u32(narrow(section.relocations.size()));commands.u32(section.flags);
        commands.u32(0);commands.u32(0);commands.u32(0);
    }
    commands.u32(0x32);commands.u32(24);commands.u32(1);commands.u32(deployment);commands.u32(0);commands.u32(0);
    commands.u32(2);commands.u32(24);commands.u32(symoff);commands.u32(narrow(symbols.size()));
    commands.u32(stroff);commands.u32(narrow(strings.size()));
    commands.u32(0x0b);commands.u32(80); // LC_DYSYMTAB: local, defined external, undefined external
    unsigned start=0;
    for(const auto count:counts){commands.u32(start);commands.u32(count);start+=count;}
    commands.resize(commands.size()+48,0);
    std::copy(commands.begin(),commands.end(),out.begin());
    return out;
}
} // namespace bolt::native_backend::detail
