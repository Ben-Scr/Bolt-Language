#pragma once
#include "Platform.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include <cstdio>
#include <iostream>
#include <unistd.h>

namespace bolt::platform {
struct HostConsole {
    static void Write(const native::String& text) {
        std::cout.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    static void WriteLine(const native::String& text) {
        Write(text);
        std::cout << '\n' << std::flush;
    }
    static native::String ReadLine() {
        std::string line;
        std::getline(std::cin, line);
        return native::String(line.data(), line.size());
    }
    static void ClearDisplay() {
#ifdef __EMSCRIPTEN__
        EM_ASM({ if (typeof console !== 'undefined' && console.clear) console.clear(); });
#else
        if (::isatty(STDOUT_FILENO)) std::fputs("\033[2J\033[H", stdout);
#endif
    }
};

struct HostDateTime : TimeZone {
    static std::int64_t UtcNowTicks() {
        using ticks = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;
        return std::chrono::duration_cast<ticks>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

} // namespace bolt::platform
