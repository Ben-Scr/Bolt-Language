#include "Dwarf.hpp"
#include <algorithm>
#include <limits>

namespace bolt::native_backend::detail {
namespace {
using Section=DwarfSections::Section;

void uleb(Bytes& out,std::uint64_t value) {
    do {
        auto low=static_cast<std::uint8_t>(value&127);value>>=7;
        out.u8(static_cast<std::uint8_t>(low|(value?128:0)));
    }while(value);
}
void sleb(Bytes& out,std::int64_t value) {
    for(;;) {
        auto low=static_cast<std::uint8_t>(value&127);
        value>>=7;
        const bool done=(value==0 && !(low&64)) || (value==-1 && (low&64));
        out.u8(static_cast<std::uint8_t>(low|(done?0:128)));
        if(done)return;
    }
}
void length32(Bytes& bytes,std::size_t field,std::size_t start) {
    const auto size=bytes.size()-start;
    if(size>=0xfffffff0u)throw std::runtime_error("Fast debug information exceeds DWARF32 limits");
    bytes.patch(field,size,4);
}
void reference(Bytes& bytes,std::vector<DwarfSections::Relocation>& rel,Section target,std::uint64_t addend,unsigned width) {
    rel.push_back({bytes.size(),target,addend,width});
    bytes.resize(bytes.size()+width);
}

enum : unsigned {
    dw_tag_compile_unit=0x11,dw_tag_subprogram=0x2e,
    dw_at_name=0x03,dw_at_stmt_list=0x10,dw_at_low_pc=0x11,dw_at_high_pc=0x12,
    dw_at_producer=0x25,dw_at_decl_column=0x39,dw_at_decl_file=0x3a,dw_at_decl_line=0x3b,
    dw_at_linkage_name=0x6e,
    dw_form_addr=0x01,dw_form_data8=0x07,dw_form_string=0x08,dw_form_udata=0x0f,dw_form_sec_offset=0x17
};
void abbreviation(Bytes& out,unsigned code,unsigned tag,bool children,
                  std::initializer_list<std::pair<unsigned,unsigned>> attributes) {
    uleb(out,code);uleb(out,tag);out.u8(children);
    for(auto [key,form]:attributes){uleb(out,key);uleb(out,form);}
    out.u16(0);
}
}

void DebugRecorder::begin(const ir::Function& function,std::size_t address) {
    functions.push_back({function.display_name.empty()?function.name:function.display_name,function.name,address,address,{}});
    position(function.at,address);
}
void DebugRecorder::position(const Location& at,std::size_t address) {
    if(at.file.empty() || at.file.front()=='<' || at.line<=0)return; // compiler-created instructions
    const auto column=std::max(at.column,0);
    auto& rows=functions.back().positions;
    std::size_t file;
    if(!rows.empty() && files[rows.back().file-1]==at.file) {
        // Lowering commonly emits several operations for a single expression.
        const auto& last=rows.back();
        if(last.line==at.line && last.column==column)return;
        file=last.file;
    }else {
        auto [it,inserted]=file_numbers.try_emplace(at.file,files.size()+1);
        if(inserted)files.push_back(at.file);
        file=it->second;
    }
    const DebugPosition pos{address,file,at.line,column};
    if(!rows.empty() && rows.back().address==address){rows.back()=pos;return;}
    rows.push_back(pos);
}
void DebugRecorder::end(std::size_t address) {
    auto& function=functions.back();
    function.end=address;
    if(function.positions.empty())functions.pop_back();
}

DwarfSections DebugRecorder::encode() const {
    DwarfSections result;
    if(functions.empty())return result;
    abbreviation(result.abbrev,1,dw_tag_compile_unit,true,{{dw_at_name,dw_form_string},{dw_at_producer,dw_form_string},
        {dw_at_stmt_list,dw_form_sec_offset},{dw_at_low_pc,dw_form_addr},{dw_at_high_pc,dw_form_data8}});
    abbreviation(result.abbrev,2,dw_tag_subprogram,false,{{dw_at_name,dw_form_string},{dw_at_linkage_name,dw_form_string},
        {dw_at_decl_file,dw_form_udata},{dw_at_decl_line,dw_form_udata},{dw_at_decl_column,dw_form_udata},
        {dw_at_low_pc,dw_form_addr},{dw_at_high_pc,dw_form_data8}});
    result.abbrev.u8(0);
    auto& info=result.info;
    info.u32(0);info.u16(4);
    reference(info,result.info_relocations,Section::Abbrev,0,4);
    info.u8(8);uleb(info,1);info.text(files.front());info.text("Bolt Fast");
    reference(info,result.info_relocations,Section::Line,0,4);
    reference(info,result.info_relocations,Section::Text,functions.front().begin,8);
    info.u64(functions.back().end-functions.front().begin);
    auto& line=result.line;
    line.u32(0);line.u16(4);line.u32(0);
    const auto header=line.size();
    // DWARF 4 section 6.2.4. One-byte minimum instruction length admits both ISAs.
    constexpr int line_base=-3,line_range=7,opcode_base=13;
    line.u8(1);line.u8(1);line.u8(1);line.u8(static_cast<std::uint8_t>(line_base));line.u8(line_range);line.u8(opcode_base);
    for(auto n:{0,1,1,1,1,0,0,0,1,0,0,1})line.u8(static_cast<std::uint8_t>(n));
    line.u8(0); // Include directories; file entries retain the compiler's path.
    for(const auto& path:files){line.text(path);uleb(line,0);uleb(line,0);uleb(line,0);}
    line.u8(0);length32(line,6,header);
    for(const auto& fn:functions) {
        const auto& declaration=fn.positions.front();
        uleb(info,2);info.text(fn.name);info.text(fn.linkage);
        uleb(info,declaration.file);uleb(info,declaration.line);uleb(info,declaration.column);
        reference(info,result.info_relocations,Section::Text,fn.begin,8);info.u64(fn.end-fn.begin);

        line.u8(0);uleb(line,9);line.u8(2); // DW_LNE_set_address
        reference(line,result.line_relocations,Section::Text,fn.begin,8);
        DebugPosition previous{fn.begin,1,1,0};
        for(const auto& pos:fn.positions) {
            if(pos.file!=previous.file){line.u8(4);uleb(line,pos.file);}
            if(pos.column!=previous.column){line.u8(5);uleb(line,pos.column);}
            auto delta=static_cast<std::int64_t>(pos.line)-previous.line;
            if(delta<line_base || delta>=line_base+line_range){
                line.u8(3);sleb(line,delta);delta=0; // DW_LNS_advance_line
            }
            auto advance=pos.address-previous.address;
            const auto special=static_cast<unsigned>(opcode_base+delta-line_base);
            if(advance>(255u-special)/line_range){line.u8(2);uleb(line,advance);advance=0;}
            // A special opcode advances both columns of the matrix (PC and
            // source line) then commits a row. It covers nearby statements in one byte.
            line.u8(static_cast<std::uint8_t>(special+advance*line_range));
            previous=pos;
        }
        if(fn.end!=previous.address){line.u8(2);uleb(line,fn.end-previous.address);}
        line.u8(0);uleb(line,1);line.u8(1); // DW_LNE_end_sequence resets the state
    }
    info.u8(0);length32(info,0,4);length32(line,0,4);
    return result;
}
}
