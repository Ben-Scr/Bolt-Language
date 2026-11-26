#pragma once
#include "library/Exceptions.hpp"
#include "Std.hpp"
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <sstream>
#elif defined(__ANDROID__) || !__has_include(<execinfo.h>)
#define BOLT_DIAGNOSTICS_USE_UNWINDER
#include <unwind.h>
#include <unistd.h>
#else
#include <execinfo.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/user.h>
#endif
#endif
#endif
namespace bolt::system {
class DiagnosticOps {
public:
    static void WriteError(const native::String& text){static std::mutex mutex;std::lock_guard lock(mutex);if(!text.empty())std::fwrite(text.data(),1,text.size(),stderr);std::fputc('\n',stderr);std::fflush(stderr);}
    [[noreturn]] static void Fail(const native::String& message){throw library::AssertionException(std::string(message.data(),message.size()));}
    static bool IsAttached(){
#ifdef _WIN32
        return IsDebuggerPresent()!=FALSE;
#elif defined(__EMSCRIPTEN__)
        return false; // The browser does not expose a debugger-attached flag.
#elif defined(__APPLE__)
        int names[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};kinfo_proc info{};std::size_t size=sizeof(info);return sysctl(names,4,&info,&size,nullptr,0)==0&&(info.kp_proc.p_flag&P_TRACED)!=0;
#elif defined(__FreeBSD__)
        int names[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,getpid()};struct kinfo_proc info{};
        std::size_t size=sizeof(info);
        return sysctl(names,4,&info,&size,nullptr,0)==0 && size>=sizeof(info) && (info.ki_flag&P_TRACED)!=0;
#else
        FILE* file=std::fopen("/proc/self/status","r");if(!file)return false;char line[256];bool attached=false;while(std::fgets(line,sizeof(line),file))if(std::strncmp(line,"TracerPid:",10)==0){attached=std::strtoul(line+10,nullptr,10)!=0;break;}std::fclose(file);return attached;
#endif
    }
    static bool Break(){if(!IsAttached())return false;
#ifdef _WIN32
        ::DebugBreak();
#else
        std::raise(SIGTRAP);
#endif
        return true;
    }
    static native::Array<native::String> Stack(std::int32_t skip=0,std::int32_t maximum=64){if(skip<0||skip>128||maximum<0||maximum>128)throw library::ArgumentOutOfRangeException("Stack frame limits must be 0..128.");
#if defined(__EMSCRIPTEN__)
        const int bytes = emscripten_get_callstack(EM_LOG_JS_STACK, nullptr, 0);
        std::vector<char> trace(static_cast<std::size_t>(std::max(1, bytes)));
        emscripten_get_callstack(EM_LOG_JS_STACK, trace.data(), static_cast<int>(trace.size()));
        std::istringstream lines(trace.data());
        std::string line;
        std::vector<std::string> selected;
        for (int index = 0; static_cast<int>(selected.size()) < maximum && std::getline(lines, line); ++index)
            if (index > skip && !line.empty()) selected.push_back(line);
        auto result = native::Array<native::String>::Create(static_cast<std::int32_t>(selected.size()));
        for (std::size_t i = 0; i < selected.size(); ++i) result.At(i) = selected[i];
        return result;
#else
        std::array<void*,256> frames{};int count=0;
#ifdef _WIN32
        count=static_cast<int>(CaptureStackBackTrace(0,256,frames.data(),nullptr));
#elif defined(BOLT_DIAGNOSTICS_USE_UNWINDER)
        struct Cursor { void** next; void** end; } cursor{frames.data(), frames.data()+frames.size()};
        _Unwind_Backtrace([](_Unwind_Context* context, void* data) {
            auto& position = *static_cast<Cursor*>(data);
            if (position.next == position.end) return _URC_END_OF_STACK;
            if (auto address = _Unwind_GetIP(context)) *position.next++ = reinterpret_cast<void*>(address);
            return _URC_NO_REASON;
        }, &cursor);
        count = static_cast<int>(cursor.next - frames.data());
#else
        count=::backtrace(frames.data(),static_cast<int>(frames.size()));
#endif
        const auto begin=std::min(count,skip+1),length=std::min(count-begin,maximum);auto result=native::Array<native::String>::Create(length);
#if !defined(_WIN32) && !defined(BOLT_DIAGNOSTICS_USE_UNWINDER)
        char** names=::backtrace_symbols(frames.data(),count);struct Free{char** p;~Free(){std::free(p);}} guard{names};
#endif
        for(int i=0;i<length;++i){char buffer[64];std::snprintf(buffer,sizeof(buffer),"native frame %p",frames[static_cast<std::size_t>(begin+i)]);
#if !defined(_WIN32) && !defined(BOLT_DIAGNOSTICS_USE_UNWINDER)
            result.At(i)=names?native::String(names[begin+i]):native::String(buffer);
#else
            result.At(i)=buffer;
#endif
        }return result;
#endif
    }
};
}

#ifdef BOLT_DIAGNOSTICS_USE_UNWINDER
#undef BOLT_DIAGNOSTICS_USE_UNWINDER
#endif
