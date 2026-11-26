#include "../src/native/JitCall.hpp"
#include "WindowsStackwalk.hpp"
#include <bolt/native/IR.hpp>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
DWORD64 expected_function=0;
bool unwound=false;
__declspec(noinline) std::uint64_t probe(std::uint8_t* stack,std::uint64_t value) {
    for(unsigned offset=0;offset<70000;offset+=4096)stack[offset]=static_cast<std::uint8_t>(offset/4096+1);
    stack[69999]=25;
    unwound=bolt_test::walk_through_generated_frame(expected_function);
    return value+stack[0]+stack[69999];
}
}

int main(){
    using namespace bolt::ir;
    using namespace bolt::native_backend;
    Options options;options.release=true;options.debug_symbols=false;
    try{(void)llvm_version(options);}catch(const std::exception& e){std::cout<<"SKIP "<<e.what()<<'\n';return 77;}
    try{
        Module module;
        module.externals.push_back({"probe",Type::I64,{Type::Ptr,Type::I64},true});
        Function frame;frame.name="call_probe";frame.exported=true;frame.result=Type::I64;
        frame.parameters={Type::I64};frame.registers={Type::Void,Type::I64,Type::Ptr,Type::I64,Type::I64,Type::I64};
        frame.blocks={{{{.op=Op::Alloca,.result=2,.immediate=70000},
            {.op=Op::Call,.result=3,.args={2,1},.text="probe"},
            {.op=Op::Constant,.result=4,.immediate=1},{.op=Op::Add,.result=5,.args={3,4}},
            {.op=Op::Return,.args={5}}}}};
        module.functions.push_back(std::move(frame));
        auto image=llvm_jit(module,{{"probe",reinterpret_cast<void*>(&probe)}},options);
        auto fn=reinterpret_cast<std::uint64_t(*)(std::uint64_t)>(image->symbol("call_probe"));
        expected_function=reinterpret_cast<DWORD64>(fn);
        if(bolt::native_backend::detail::call_generated(fn,15)!=42 || !unwound)throw std::runtime_error("optimized callback or live Windows stack walk failed");
        image.reset();
        DWORD64 base=0;
        if(RtlLookupFunctionEntry(expected_function,&base,nullptr))throw std::runtime_error("stale LLVM JIT exception table");
        std::cout<<"PASS Windows ARM64 LLVM optimized callback, 70 KB frame and unwind lifetime\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
