#include <cstdio>
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios_base::sync_with_stdio(false);
    for (int i = 0; i < 300000; ++i) {
        if ((i & 3) == 0) std::cout << "status: caf\xc3\xa9\n";
        else std::cout << i << '\n';
#ifdef FLUSH_EACH_LINE
        std::cout.flush();
#endif
    }
    return std::cout.flush() ? 0 : 1;
}
