#include <bolt/Frontend.hpp>
#include <bolt/native/Jit.hpp>
#include <future>
#include <iostream>

int main() {
    auto compile=[](int value) {
        auto source="public static class Program { public: int Main() { return "+std::to_string(value)+"; } }";
        bolt::JitOptions options;
        options.backend="fast";
        return std::make_unique<bolt::NativeProgram>(bolt::compile_source(source,"Scene.bolt"),options);
    };
    auto active=compile(7);
    for (int i=8;i<24;++i) {
        auto pending=std::async(std::launch::async,compile,i);
        if (active->run()!=i-1) return 1;
        auto next=pending.get();
        if (next->run()!=i || active->run()!=i-1) return 2;
        active=std::move(next);
    }
    std::cout<<"PASS overlapping JIT images, concurrent compile, old-revision lifetime and page reuse\n";
}
