#pragma once
#include <bolt/native/IR.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bolt::native_backend::detail {
struct Bytes : std::vector<std::uint8_t> {
    void u8(std::uint8_t n) { push_back(n); }
    void u16(std::uint16_t n) { for(unsigned j=0;j<2;++j)u8(static_cast<std::uint8_t>(n>>(j*8))); }
    void u32(std::uint32_t n) { for(unsigned j=0;j<4;++j)u8(static_cast<std::uint8_t>(n>>(j*8))); }
    void u64(std::uint64_t n) { for(unsigned j=0;j<8;++j)u8(static_cast<std::uint8_t>(n>>(j*8))); }
    void align(std::size_t a,std::uint8_t fill=0) { while(size()%a)u8(fill); }
    void patch(std::size_t at,std::uint64_t n,unsigned count) { if(at+count>size())throw std::runtime_error("native relocation outside image");for(unsigned j=0;j<count;++j)(*this)[at+j]=static_cast<std::uint8_t>(n>>(j*8)); }
    void text(const std::string& s) { insert(end(),s.begin(),s.end());u8(0); }
};
enum class RelocKind { Relative32, Absolute64, ArmPage21, ArmLo12 };
struct Definition { std::string name; unsigned section=0; std::uint64_t offset=0,size=0; bool exported=false,function=false; };
struct Fixup { unsigned section=0; std::size_t offset=0; RelocKind kind{}; std::string symbol; std::int64_t addend=0; bool branch=false; };
enum class Machine { X86_64, AArch64 };
struct WindowsUnwind {
    std::size_t begin, end;
    Bytes info;
};
struct Image { Machine machine=Machine::X86_64; Bytes text,data; std::vector<Definition> symbols; std::vector<Fixup> fixups; std::vector<WindowsUnwind> unwind; };
class DebugRecorder;
struct DwarfSections;
enum class X64ABI { SystemV, Windows };
Image x64_image(const ir::Module&,X64ABI abi=X64ABI::SystemV,DebugRecorder* debug=nullptr);
enum class ArmABI { AAPCS64, Apple, Windows };
enum class ArmAddressing { PageRelative, JitLiteral };
Image aarch64_image(const ir::Module&,ArmABI abi=ArmABI::AAPCS64,ArmAddressing addresses=ArmAddressing::PageRelative,DebugRecorder* debug=nullptr);
void apply_arm_address_fixup(std::uint8_t* at,RelocKind kind,std::uintptr_t target,std::uintptr_t location);
std::vector<std::uint8_t> elf_object(const Image&,DwarfSections* debug=nullptr);
std::vector<std::uint8_t> coff_object(const Image&);
std::vector<std::uint8_t> macho_object(const Image&,std::uint32_t deployment);
} // namespace bolt::native_backend::detail
