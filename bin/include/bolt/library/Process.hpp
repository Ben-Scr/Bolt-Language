#pragma once
#include <cstdlib>
#include <cstdint>
namespace bolt::library {
    class Process {
        public: Process()=delete;
        [[noreturn]] static void Terminate(std::int32_t code=0) {
            std::exit(code);
        }
    };
}
