#pragma once
#include "library/Exceptions.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__EMSCRIPTEN__)
#include "detail/WebRandom.h"
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
#include <cerrno>
#include <sys/random.h>
#elif defined(__ANDROID__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>
#else
#include <unistd.h>
#endif

namespace bolt::platform {
// Used by the interpreter and hosted platforms. All providers consume OS or
// Web Crypto entropy; failure is reported to the script as an I/O error.
inline void secure_random_bytes(std::uint8_t* buffer,std::size_t size) {
    if(size==0)return;
#if defined(_WIN32)
    // Keep bcrypt optional for programs that never use cryptography. Pin the
    // system provider for the lifetime of the process, including worker threads.
    static const auto generate=[]{
        const auto library=LoadLibraryExW(L"bcrypt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        return library?reinterpret_cast<decltype(&BCryptGenRandom)>(GetProcAddress(library,"BCryptGenRandom")):nullptr;
    }();
    if(!generate)throw library::IOException("Windows system random provider is unavailable.");
    while(size) {
        const auto count=static_cast<ULONG>(std::min<std::size_t>(size,0xffffffffu));
        if(generate(nullptr,buffer,count,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
            throw library::IOException("BCryptGenRandom failed.");
        buffer+=count;size-=count;
    }
#elif defined(__linux__) && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    while(size) {
        const auto count=::getrandom(buffer,size,0);
        if(count<0&&errno==EINTR)continue;
        if(count<=0)throw library::IOException("getrandom failed.");
        buffer+=count;size-=static_cast<std::size_t>(count);
    }
#elif defined(__EMSCRIPTEN__)
    if(::bolt_web_random_fill(buffer,size)!=0)throw library::IOException("Web Crypto could not fill the random buffer.");
#elif defined(__ANDROID__) || defined(__APPLE__) || defined(__FreeBSD__)
    ::arc4random_buf(buffer,size);
#else
    while(size) {
        const auto count=std::min<std::size_t>(size,256);
        if(::getentropy(buffer,count)!=0)throw library::IOException("The host random provider failed.");
        buffer+=count;size-=count;
    }
#endif
}
} // namespace bolt::platform
