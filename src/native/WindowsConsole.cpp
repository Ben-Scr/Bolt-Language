// Keep standard iostream references in a separate static archive member.
// The MSVC linker retains a stream's startup constructor once its library
// object is extracted, even if /OPT:REF later discards the referencing code.
#include <bolt/native/ABI.hpp>
#include <iostream>

extern "C" void bolt_write_stream(BrContext*,const char*,const void*,std::int32_t,std::ostream&) noexcept;
extern "C" void bolt_read_stream(BrContext*,const char*,void*,std::istream&) noexcept;

void br_write(BrContext* c,const char* site,const void* value,std::int32_t newline) noexcept {
    bolt_write_stream(c,site,value,newline,std::cout);
}
void br_read(BrContext* c,const char* site,void* dest) noexcept {
    bolt_read_stream(c,site,dest,std::cin);
}
