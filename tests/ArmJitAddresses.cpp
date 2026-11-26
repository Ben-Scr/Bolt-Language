#include "../src/native/JitCall.hpp"
// Verify the code form used when Apple's MAP_JIT text and writable data can
// be more than four GiB apart. Linux/AArch64 also executes it across that gap.
#include "../src/native/Object.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#if defined(__aarch64__) && defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using namespace bolt::ir;
using namespace bolt::native_backend::detail;

Image make_image() {
    Module module;
    module.globals.push_back({"payload",{40,0,0,0},false,4,{}});
    module.externals.push_back({"add_two",Type::I32,{Type::I32},true});
    Function fn;
    fn.name="check_distance";fn.exported=true;fn.result=Type::I32;
    fn.registers={Type::Void,Type::Ptr,Type::I32,Type::I32};
    fn.blocks.emplace_back();
    Instruction address;address.op=Op::Address;address.result=1;address.text="payload";
    Instruction load;load.op=Op::Load;load.result=2;load.args={1};
    Instruction call;call.op=Op::Call;call.result=3;call.text="add_two";call.args={2};
    Instruction ret;ret.op=Op::Return;ret.args={3};
    fn.blocks[0].code={address,load,call,ret};
    module.functions.push_back(std::move(fn));
    return aarch64_image(module,ArmABI::Apple,ArmAddressing::JitLiteral);
}

void verify_literals(const Image& image) {
    if(image.fixups.size()!=2)throw std::runtime_error("expected a data and a foreign-code relocation");
    for(const auto& fix:image.fixups) {
        if(fix.kind!=RelocKind::Absolute64||fix.section!=1||fix.offset<8||fix.offset+8>image.text.size())
            throw std::runtime_error("Apple JIT address has a range-limited relocation");
        std::uint32_t load=0,skip=0;
        std::memcpy(&load,image.text.data()+fix.offset-8,4);
        std::memcpy(&skip,image.text.data()+fix.offset-4,4);
        if((load&~31u)!=0x58000040u||skip!=0x14000003u)
            throw std::runtime_error("Apple JIT literal is not loaded or skipped correctly");
    }
}

void verify_field_offset() {
    Module module;
    Function fn;
    fn.name="field_address";fn.result=Type::Ptr;fn.parameters={Type::Ptr};
    fn.registers={Type::Void,Type::Ptr,Type::Ptr};
    fn.blocks.emplace_back();
    fn.blocks[0].code={{.op=Op::Offset,.result=2,.args={1},.immediate=0x123456},
                       {.op=Op::Return,.args={2}}};
    module.functions.push_back(std::move(fn));
    const auto image=aarch64_image(module,ArmABI::Apple,ArmAddressing::JitLiteral);
    for (std::size_t i=0; i+8<=image.text.size(); i+=4) {
        std::uint32_t high,low;
        std::memcpy(&high,image.text.data()+i,4);
        std::memcpy(&low,image.text.data()+i+4,4);
        if ((high & 0xfffffc00u)==(0x91400000u|(0x123u<<10)) &&
            (low & 0xfffffc00u)==(0x91000000u|(0x456u<<10)) &&
            ((low>>5)&31u)==(high&31u)) return;
    }
    throw std::runtime_error("field offset lost its high or low immediate");
}

#if defined(__aarch64__) && defined(__linux__)
extern "C" int add_two(int value) {return value+2;}

void execute(const Image& image) {
    const auto page=static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    auto allocate=[page](std::uintptr_t hint){
        auto* region=mmap(reinterpret_cast<void*>(hint),page,PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        if(region==MAP_FAILED)throw std::runtime_error("mmap failed");
        return static_cast<std::uint8_t*>(region);
    };
    auto* text=allocate(0x180000000ull);
    auto* data=allocate(0x1900000000ull);
    const auto code_address=reinterpret_cast<std::uintptr_t>(text);
    const auto data_address=reinterpret_cast<std::uintptr_t>(data);
    const auto distance=std::max(code_address,data_address)-std::min(code_address,data_address);
    if(distance<0x100000000ull)throw std::runtime_error("test did not obtain the required address separation");
    std::memcpy(data,image.data.data(),image.data.size());
    std::memcpy(text,image.text.data(),image.text.size());
    for(const auto& fix:image.fixups) {
        std::uint64_t address;
        if(fix.symbol=="payload")address=data_address;
        else if(fix.symbol=="add_two")address=reinterpret_cast<std::uintptr_t>(&add_two);
        else throw std::runtime_error("unexpected test symbol");
        std::memcpy(text+fix.offset,&address,8);
    }
    if(mprotect(text,page,PROT_READ|PROT_EXEC))throw std::runtime_error("mprotect failed");
    __builtin___clear_cache(reinterpret_cast<char*>(text),reinterpret_cast<char*>(text+image.text.size()));
    auto fn=reinterpret_cast<int(*)()>(text);
    if(call_generated(fn)!=42)throw std::runtime_error("initial value lost across far data or call");
    data[0]=80;
    if(call_generated(fn)!=82)throw std::runtime_error("mutable data access lost after relocation");
    munmap(data,page);munmap(text,page);
    std::cout<<"executed AArch64 with code/data distance "<<distance<<" bytes\n";
}
#endif
}

int main() {
    const auto image=make_image();
    verify_literals(image);
    verify_field_offset();
#if defined(__aarch64__) && defined(__linux__)
    execute(image);
#endif
    std::cout<<"PASS Apple ARM64 JIT absolute address encoding\n";
}
