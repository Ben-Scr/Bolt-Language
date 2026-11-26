#pragma once
#define NOMINMAX
#include <windows.h>

namespace bolt_test {
// Walk from a compiled C++ callback across the JIT frame while its freshly
// registered pdata/xdata is live. Leaf functions have no unwind entry.
__declspec(noinline) bool walk_through_generated_frame(DWORD64 function) {
    DWORD64 base=0;
    auto expected=RtlLookupFunctionEntry(function,&base,nullptr);
    if(!expected || base+expected->BeginAddress!=function)return false;
    CONTEXT context;
    RtlCaptureContext(&context);
    for(unsigned step=0;step<16;++step) {
        DWORD64 image=0,establisher=0;
#if defined(__aarch64__) || defined(_M_ARM64)
        auto& current_pc=context.Pc;
        auto& current_sp=context.Sp;
#else
        auto& current_pc=context.Rip;
        auto& current_sp=context.Rsp;
#endif
        auto entry=RtlLookupFunctionEntry(current_pc,&image,nullptr);
        if(!entry) {
#if defined(__aarch64__) || defined(_M_ARM64)
            if(current_pc==context.Lr)return false;
            current_pc=context.Lr;
#else
            current_pc=*reinterpret_cast<const DWORD64*>(current_sp);
            current_sp+=sizeof(DWORD64);
#endif
            continue;
        }
        const auto pc=current_pc,sp=current_sp;
        PVOID handler=nullptr;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,pc,entry,&context,&handler,&establisher,nullptr);
        if(!current_pc||current_pc==pc||current_sp<sp)return false;
        if(entry==expected)return current_sp>sp;
    }
    return false;
}
}
