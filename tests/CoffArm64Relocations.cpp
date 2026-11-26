#include "../src/native/CoffArm64Link.hpp"
#include <array>
#include <iostream>

int main() {
    using namespace bolt::native_backend::coff_arm64;
    alignas(4096) std::array<std::uint8_t,8192> text{};
    auto* pc=text.data()+4092;
    auto address=reinterpret_cast<std::uintptr_t>(pc);
    auto page=address&~std::uintptr_t(4095);
    auto adr=[](std::int32_t bytes){
        auto n=static_cast<std::uint32_t>(bytes)&0x1fffff;
        return 0x90000007u|((n&3)<<29)|((n&0x1ffffc)<<3);
    };
    try {
        // Both an embedded positive and negative byte addend cross a page boundary.
        write(pc,adr(40));relocate(pc,4,page+8192-8,page,0,1);
        require(read<std::uint32_t>(pc)==adr(2),"positive ADRP page carry");
        write(pc,adr(-16));relocate(pc,4,page+8,page,0,1);
        require(read<std::uint32_t>(pc)==adr(-1),"negative ADRP page carry");
        write(pc,0xf9400000u|(3u<<10)); // 64-bit LDR, implicit 24-byte addend
        relocate(pc,7,page+40,page,0,1);
        require(read<std::uint32_t>(pc)==(0xf9400000u|(8u<<10)),"scaled LDR addend");
        write(pc,0x3dc00000u|(2u<<10)); // 128-bit LDR, 32-byte addend
        relocate(pc,7,page+16,page,0,1);
        require(read<std::uint32_t>(pc)==(0x3dc00000u|(3u<<10)),"SIMD LDR addend");
        write(pc,0x94000000u);relocate(pc,3,address-8,page,0,1);
        require(read<std::uint32_t>(pc)==0x97fffffeu,"backward branch");
        write(pc,0u);relocate(pc,2,page+4096,page,0,1);
        require(read<std::uint32_t>(pc)==4096,"unwind/image RVA");
        std::cout<<"PASS ARM64 COFF page carry, scaled addresses, branch and Windows RVA\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
