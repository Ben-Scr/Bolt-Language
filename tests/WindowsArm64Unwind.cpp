#include "../src/native/Object.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
using namespace bolt::ir;
using namespace bolt::native_backend::detail;

Function probe(std::string name,std::size_t size) {
    Function fn;
    fn.name=std::move(name);fn.exported=true;fn.result=Type::I64;
    fn.parameters={Type::I64};fn.registers={Type::Void,Type::I64,Type::Ptr,Type::Ptr,Type::Ptr,Type::I64};
    fn.blocks.emplace_back();
    fn.blocks[0].code={{.op=Op::Alloca,.result=2,.immediate=size},
                       {.op=Op::Offset,.result=3,.args={2},.immediate=size-8},
                       {.op=Op::Store,.args={3,1}},
                       {.op=Op::Address,.result=4,.text=fn.name},
                       {.op=Op::Call,.result=5,.args={4,1,3},.text="native_probe"},
                       {.op=Op::Return,.args={5}}};
    return fn;
}

std::uint32_t word(const Bytes& bytes,std::size_t at) {
    return bytes.at(at)|(std::uint32_t(bytes.at(at+1))<<8)|
           (std::uint32_t(bytes.at(at+2))<<16)|(std::uint32_t(bytes.at(at+3))<<24);
}
void check(bool ok,const char* detail){if(!ok)throw std::runtime_error(detail);}
}

int main(int argc,char** argv) {
    try {
        if(argc>3)throw std::runtime_error("usage: BoltWindowsArm64UnwindTests [object-path [arm64|x64]]");
        const bool x64=argc==3 && std::string(argv[2])=="x64";
        if(argc==3 && !x64 && std::string(argv[2])!="arm64")throw std::runtime_error("expected arm64 or x64");
        Module m;
        m.externals.push_back({"native_probe",Type::I64,{Type::Ptr,Type::I64,Type::Ptr},true});
        m.functions={probe("small_probe",16),probe("medium_probe",1024),probe("large_probe",70008)};
        // Validate all Win64 allocation encodings. The two-slot large form
        // stores units of eight, the three-slot form stores a 32-bit byte count.
        Module extra=m;
        extra.functions.push_back(probe("huge_probe",600000));
        const auto win64=x64_image(extra,X64ABI::Windows);
        check(win64.unwind.size()==4,"Missing Win64 unwind records");
        for(std::size_t i=0;i<win64.unwind.size();++i) {
            const auto& record=win64.unwind[i];const auto& info=record.info;
            check(info.size()>=8 && info[0]==1 && info[3]==0,"Invalid Win64 fixed-frame header");
            check(info[1]==info[4] && info[1]<64,"Invalid Win64 stack allocation code offset");
            const auto count=std::size_t(info[2])*2;
            check(info.at(4+count-2)==1 && info.at(4+count-1)==0x50,"Wrong pushed nonvolatile register");
            check((i==0 && (info[5]&15)==2) || ((i==1||i==2) && info[5]==1) || (i==3 && info[5]==0x11),
                  "Wrong Win64 allocation opcode");
            check(win64.text.at(record.end-2)==0x5d && win64.text.at(record.end-1)==0xc3,"Wrong Win64 epilogue");
        }
        auto amd64_obj=coff_object(win64);
        check(amd64_obj.size()>180 && amd64_obj[0]==0x64 && amd64_obj[1]==0x86 && amd64_obj[2]==4,
              "Expected four-section x64 COFF");
        auto image=aarch64_image(m,ArmABI::Windows);
        check(image.unwind.size()==3,"Missing Windows ARM64 unwind data");
        for(std::size_t i=0;i<image.unwind.size();++i) {
            auto& record=image.unwind[i];auto symbol=std::find_if(image.symbols.begin(),image.symbols.end(),
                [&](const auto& s){return s.name==m.functions[i].name;});
            check(symbol!=image.symbols.end()&&record.begin==symbol->offset,"Unwind function start mismatch");
            auto header=word(record.info,0);
            check((header&0x3ffff)*4==symbol->size,"Wrong unwind function size");
            check(((header>>18)&15)==8,"Unwind metadata must encode a single final epilogue");
            check(4+4*(header>>27)==record.info.size(),"Wrong unwind code length");
            auto epilogue=4+((header>>22)&31);
            check(record.info.at(epilogue)==0xe1&&record.info.at(epilogue+1)==0x81&&record.info.at(epilogue+2)==0xe4,
                  "Unwind epilogue does not restore sp/fp/lr");
            auto size_class=record.info.at(4);
            check((i==0&&size_class<32)||(i==1&&(size_class&0xf8)==0xc0)||(i==2&&size_class==0xe0),
                  "Wrong unwind stack allocation encoding");
        }
        auto obj=coff_object(image);
        check(obj.size()>180&&obj[0]==0x64&&obj[1]==0xaa&&obj[2]==4,"Expected four-section ARM64 COFF");
        if(argc>=2){if(x64)obj=std::move(amd64_obj);std::ofstream file(argv[1],std::ios::binary);file.write(reinterpret_cast<const char*>(obj.data()),obj.size());if(!file)throw std::runtime_error("cannot write object");}
        std::cout<<"PASS ARM64 and x64 Windows COFF and small/medium/large SEH records\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
