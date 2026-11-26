#pragma once
#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <future>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace bolt::cli_process {
struct Result {
    int exit_code=1;
    std::string output,error,termination;
};
#ifdef _WIN32
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){reset();}
    void reset(HANDLE next=nullptr){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=next;}
    Handle()=default;
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
inline std::wstring argument(const std::string& input) {
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),nullptr,0);
    if(!input.empty()&&!size)throw std::runtime_error("invalid UTF-8 process argument");
    std::wstring wide(static_cast<std::size_t>(size),L'\0');
    if(size)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input.data(),static_cast<int>(input.size()),wide.data(),size);
    std::wstring result=L"\"";std::size_t slashes=0;
    for(wchar_t c:wide){if(c==L'\\'){++slashes;continue;}result.append(c==L'"'?slashes*2+1:slashes,L'\\');result+=c;slashes=0;}
    result.append(slashes*2,L'\\');return result+L'"';
}
struct Pipe {
    Handle read,write;
    Pipe(){
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        if(!CreatePipe(&read.value,&write.value,&security,0)||!SetHandleInformation(read.value,HANDLE_FLAG_INHERIT,0))
            throw std::runtime_error("could not create process output pipe (Windows error "+std::to_string(GetLastError())+")");
    }
    void collect(std::string& target){std::array<char,16384> buffer{};DWORD size=0;while(ReadFile(read.value,buffer.data(),static_cast<DWORD>(buffer.size()),&size,nullptr)&&size)target.append(buffer.data(),size);}
};
inline void inherit_standard(Handle& target,DWORD kind) {
    const auto source=GetStdHandle(kind);
    if(source&&source!=INVALID_HANDLE_VALUE){
        if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&target.value,0,TRUE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("could not inherit process standard stream (Windows error "+std::to_string(GetLastError())+")");
    }else{
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        target.value=CreateFileW(L"NUL",kind==STD_INPUT_HANDLE?GENERIC_READ:GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
        if(target.value==INVALID_HANDLE_VALUE)throw std::runtime_error("could not open process standard stream");
    }
}
#else
struct Pipe {
    int read=-1,write=-1;
    Pipe(){int descriptors[2];if(pipe(descriptors))throw std::runtime_error("could not create process output pipe");read=descriptors[0];write=descriptors[1];}
    ~Pipe(){if(read>=0)close(read);if(write>=0)close(write);}
    void close_writer(){close(write);write=-1;}
    void collect(std::string& target){std::array<char,16384> buffer{};for(;;){const auto size=::read(read,buffer.data(),buffer.size());if(size>0)target.append(buffer.data(),static_cast<std::size_t>(size));else if(size<0&&errno==EINTR)continue;else break;}}
    Pipe(const Pipe&)=delete;
    Pipe& operator=(const Pipe&)=delete;
};
#endif

// Toolchain output uses two concurrently drained pipes: large diagnostics on
// either stream must neither deadlock nor disappear when the caller is an IDE.
// Applications inherit streams directly to retain interactive I/O behavior.
inline Result run(const std::vector<std::string>& arguments,bool capture=false) {
    if(arguments.empty())throw std::runtime_error("cannot start a process without an executable");
    Result result;
    std::unique_ptr<Pipe> output,error;
    if(capture){output=std::make_unique<Pipe>();error=std::make_unique<Pipe>();}
#ifdef _WIN32
    std::wstring command;
    for(const auto& item:arguments){if(!command.empty())command+=L' ';command+=argument(item);}
    Handle input_handle,output_handle,error_handle;
    inherit_standard(input_handle,STD_INPUT_HANDLE);
    if(!capture){inherit_standard(output_handle,STD_OUTPUT_HANDLE);inherit_standard(error_handle,STD_ERROR_HANDLE);}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=input_handle.value;
    startup.hStdOutput=capture?output->write.value:output_handle.value;
    startup.hStdError=capture?error->write.value:error_handle.value;
    PROCESS_INFORMATION info{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,0,nullptr,nullptr,&startup,&info))
        throw std::runtime_error("could not start process '"+arguments[0]+"' (Windows error "+std::to_string(GetLastError())+"). Check that the executable and its runtime dependencies exist.");
    Handle process,thread;process.value=info.hProcess;thread.value=info.hThread;
    if(capture){output->write.reset();error->write.reset();}
#else
    std::vector<char*> argv;for(const auto& item:arguments)argv.push_back(const_cast<char*>(item.c_str()));argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    if(posix_spawn_file_actions_init(&actions))throw std::runtime_error("could not initialize process stream actions");
    int setup=0;
    if(capture){
        setup|=posix_spawn_file_actions_adddup2(&actions,output->write,STDOUT_FILENO);
        setup|=posix_spawn_file_actions_adddup2(&actions,error->write,STDERR_FILENO);
        for(auto fd:{output->read,output->write,error->read,error->write})setup|=posix_spawn_file_actions_addclose(&actions,fd);
    }
    pid_t pid=0;
    const int spawned=setup?setup:posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    if(spawned)throw std::runtime_error("could not start process '"+arguments[0]+"' (error "+std::to_string(spawned)+"). Check the executable path and permissions.");
    if(capture){output->close_writer();error->close_writer();}
#endif
    if(capture){
        auto reader=std::async(std::launch::async,[&]{output->collect(result.output);});
        error->collect(result.error);
        reader.get();
    }
#ifdef _WIN32
    if(WaitForSingleObject(process.value,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("could not wait for child process");
    DWORD code=1;if(!GetExitCodeProcess(process.value,&code))throw std::runtime_error("could not obtain child process exit code");
    result.exit_code=static_cast<int>(code);
    // Windows exit codes also contain ordinary signed Main results, such as
    // -1 (0xFFFFFFFF). High bits alone do not identify an exception.
    const char* reason=nullptr;
    switch(code){
        case 0xC00000FDu:reason="stack overflow; check for unbounded recursion";break;
        case 0xC0000005u:reason="access violation";break;
        case 0xC000001Du:reason="illegal instruction";break;
        case 0xC0000135u:reason="missing runtime dependency";break;
        case 0xC0000094u:reason="integer division by zero";break;
        case 0xC000008Eu:reason="floating-point division by zero";break;
        case 0xC0000374u:reason="heap corruption";break;
        case 0xC0000409u:reason="fail-fast exception";break;
        case 0xE06D7363u:reason="unhandled C++ exception";break;
    }
    if(reason){
        result.termination="terminated with "+std::string(reason)+" (exit code "+std::to_string(code)+")";
    }
#else
    int status=0;
    while(waitpid(pid,&status,0)<0)if(errno!=EINTR)throw std::runtime_error("could not wait for child process");
    result.exit_code=WIFEXITED(status)?WEXITSTATUS(status):128+(WIFSIGNALED(status)?WTERMSIG(status):0);
    if(WIFSIGNALED(status)){
        const auto signal=WTERMSIG(status);
        const char* reason=signal==SIGSEGV?"segmentation fault; possible invalid memory access or stack overflow":signal==SIGABRT?"abort":signal==SIGILL?"illegal instruction":signal==SIGFPE?"arithmetic fault":"signal";
        result.termination="terminated by signal "+std::to_string(signal)+" ("+reason+")";
    }
#endif
    return result;
}
} // namespace bolt::cli_process
