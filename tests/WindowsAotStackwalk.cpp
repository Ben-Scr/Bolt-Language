#include "WindowsStackwalk.hpp"
#include <cstdint>
#include <cstdio>

using U64=std::uint64_t;
extern "C" U64 small_probe(U64);
extern "C" U64 medium_probe(U64);
extern "C" U64 large_probe(U64);
#if defined(_M_X64) || defined(__x86_64__)
extern "C" U64 huge_probe(U64);
#endif

extern "C" __declspec(noinline) U64 native_probe(void* function,U64 input,const U64* local) {
    if(*local!=input || !bolt_test::walk_through_generated_frame(reinterpret_cast<DWORD64>(function)))return 0;
    return input+3;
}

int main() {
    constexpr U64 value=UINT64_C(0xff00ffeeccddee5a);
    if(small_probe(value)!=value+3 || medium_probe(value)!=value+3 || large_probe(value)!=value+3)return 1;
#if defined(_M_X64) || defined(__x86_64__)
    if(huge_probe(value)!=value+3)return 2;
#endif
    std::puts("PASS Win64 AOT lookup, native callback and virtual unwind across small through large frames");
    return 0;
}
