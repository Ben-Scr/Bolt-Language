// Native boundary tests; these instrument the boundary headers, not the entire compiler.
#include <bolt/SystemMemory.hpp>
#include <bolt/Buffer.hpp>
#include <bolt/SystemProcess.hpp>
#include <bolt/SystemNetworking.hpp>
#include <stdexcept>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif
using namespace bolt::system;
// Always evaluate checks, including Release builds with NDEBUG.
static void Check(bool condition){if(!condition)throw std::runtime_error("System primitives check failed.");}
struct Tracked {
    static inline int live = 0;
    static inline int copies_until_throw = -1;
    int value;
    explicit Tracked(int v = 0) : value(v) { ++live; }
    Tracked(const Tracked& other) : value(other.value) {
        if (copies_until_throw == 0) throw std::runtime_error("injected copy failure");
        if (copies_until_throw > 0) --copies_until_throw;
        ++live;
    }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
    Tracked& operator=(const Tracked&) = default;
    ~Tracked() { --live; }
};
static void CheckBufferGrowth() {
    constexpr std::size_t limit = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    static_assert(bolt::memory::detail::buffer_growth_capacity(limit/2 + 1, limit/2 + 2) == limit);
    try {
        (void)bolt::memory::detail::buffer_growth_capacity(limit, limit + 1);
        throw std::logic_error("overflow expected");
    } catch (const std::runtime_error& error) { Check(std::string(error.what()) == "buffer size overflow"); }

    bolt::memory::Buffer<int> values;
    values.Reserve(1);
    values.Add(3);
    values.Add(values.At(0));
    Check(values.At(1) == 3);
    unsigned grows = 0;
    for (int i = 0; i < 4096; ++i) {
        const auto old = values.Capacity();
        values.Add(i);
        grows += values.Capacity() != old;
    }
    Check(grows <= 13);
    while (values.Count() < values.Capacity()) values.Add(0);
    values.Insert(0, values.At(1));
    Check(values.At(0) == 3 && values.At(1) == 3);
    bolt::memory::Buffer<bool> flags;
    auto bits = bolt::native::Array<bool>::Create(1200);
    for (int i = 0; i < 1200; ++i) bits.At(i) = (i % 3 == 0);
    flags.AppendRange(bits, 3, 1100);
    Check(flags.Count() == 1100);
    for (int i = 0; i < flags.Count(); ++i) Check(flags.At(i) == bits.At(i + 3));
    {
        auto source = bolt::native::Array<Tracked>::Create(4);
        source.At(2).value = 42;
        bolt::memory::Buffer<Tracked> destination;
        destination.Reserve(8);
        destination.Add(source.At(2));
        const auto before = Tracked::live;
        Tracked::copies_until_throw = 1;
        try { destination.AppendRange(source, 0, 4); throw std::logic_error("copy failure expected"); }
        catch (const std::runtime_error& error) { Check(std::string(error.what()) == "injected copy failure"); }
        Tracked::copies_until_throw = -1;
        Check(before == Tracked::live && destination.Count() == 1 && destination.At(0).value == 42);
    }
    Check(Tracked::live == 0);
    for (const int capacity : {2, 8}) {
        // A self-append must survive reallocation and roll back partial copies.
        bolt::memory::Buffer<Tracked> source;
        source.Reserve(capacity);
        source.Add(Tracked(7));
        source.Add(Tracked(11));
        for (int failure = 0; failure < (capacity == 2 ? 4 : 2); ++failure) {
            Tracked::copies_until_throw = failure;
            try {
                source.AppendBuffer(0, 2, source);
                throw std::logic_error("copy failure expected");
            } catch (const std::runtime_error& error) {
                Check(std::string(error.what()) == "injected copy failure");
            }
            Tracked::copies_until_throw = -1;
            Check(source.Count() == 2 && source.Capacity() == capacity);
            Check(source.At(0).value == 7 && source.At(1).value == 11 && Tracked::live == 2);
        }
        source.AppendBuffer(0, 2, source);
        Check(source.Count() == 4 && source.At(2).value == 7 && source.At(3).value == 11);
    }
    Check(Tracked::live == 0);
}
int main(){
 CheckBufferGrowth();
 Allocator allocator;
 for(int i=0;i<500;i++){
  auto a=allocator.Allocate(33,256);Check(MemoryOps::IsAligned(a.Data(),256));MemoryOps::Set(a.Data(),7,a.Length());
  auto b=a;allocator.Free(a);Check(!b.IsAlive());
 }
 Check(allocator.LiveBytes()==0&&allocator.LiveAllocations()==0);
 allocator.FailAfter(0);try{allocator.Allocate(12);Check(false);}catch(const bolt::library::OutOfMemoryException&){}
 allocator.FailAfter(-1);
 ArenaSlice stale;{ByteArena arena;stale=arena.Allocate(20);stale.Write(0,4);arena.Reset();Check(!stale.IsAlive());}
 try{stale.Read(0);Check(false);}catch(const bolt::library::ObjectDisposedException&){}

 for(int i=0;i<40;i++){
  auto socket=SocketHandle::Create(false,true);socket.Bind(IPEndPoint(IPAddress::Loopback(),0));auto alias=socket;socket.Close();Check(!alias.IsOpen());
 }
#ifndef _WIN32
 auto args=bolt::native::Array<bolt::native::String>::Create(1);args.At(0)="test";
 auto environment=bolt::native::Array<bolt::native::String>::Create(0);
 for(int i=0;i<12;i++){auto p=ProcessHandle::Start("/usr/bin/printf",args,"",true,environment,true);auto result=p.Capture(5000,100);Check(result.ExitCode()==0&&result.out.size()==4);}
 // Closed standard descriptors must not collide with child redirections.
 int saved=::dup(STDIN_FILENO);::close(STDIN_FILENO);
 auto p=ProcessHandle::Start("/usr/bin/printf",args,"",true,environment,true);auto result=p.Capture(5000,100);Check(result.out.size()==4);
 if(saved>=0){::dup2(saved,STDIN_FILENO);::close(saved);}
#endif
 std::cout<<"System primitives: allocation/cleanup/process/socket checks passed\n";
}
