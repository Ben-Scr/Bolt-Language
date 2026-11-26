#pragma once
#include "library/Exceptions.hpp"
#include <thread>
#include "Path.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#if defined(__ANDROID__) && __ANDROID_API__ < 34
#include <dlfcn.h>
#endif
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif
namespace bolt::system {
inline void CheckTimeout(std::int32_t milliseconds) {
    if(milliseconds < -1)throw library::ArgumentOutOfRangeException("Timeout must be -1 or non-negative.");
}

inline std::string SystemText(const native::String& value){std::string s(value.data(),value.size());if(s.find('\0')!=std::string::npos)throw library::ArgumentException("Native text cannot contain an embedded NUL.");return s;}
inline std::mutex& EnvironmentMutex(){static std::mutex value;return value;}
inline std::string SystemFailure(const char* operation,int error){return std::string(operation)+" failed (native error "+std::to_string(error)+").";}
#ifdef _WIN32
inline std::wstring Wide(const std::string& s){return bolt::path_from_utf8(s).native();}
inline std::string Narrow(const std::wstring& s){return bolt::path_to_utf8(std::filesystem::path(s));}
using ProcessDescriptor=HANDLE;
inline const HANDLE BadProcessDescriptor=INVALID_HANDLE_VALUE;
inline void CloseDescriptor(HANDLE value)noexcept{if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
#else
using ProcessDescriptor=int;
inline constexpr auto BadProcessDescriptor=-1;
inline void CloseDescriptor(int value)noexcept{if(value>=0)::close(value);}
inline char** NativeEnvironment(){
#ifdef __APPLE__
return *_NSGetEnviron();
#else
return ::environ;
#endif
}
#endif
class EnvironmentOps {
public:
    static native::String Get(native::String name){const auto key=SystemText(name);if(key.empty()||key.find('=')!=std::string::npos)throw library::ArgumentException("Invalid environment variable name.");std::lock_guard lock(EnvironmentMutex());
#ifdef _WIN32
        auto w=Wide(key);auto n=GetEnvironmentVariableW(w.c_str(),nullptr,0);if(!n)return "";std::wstring value(n,L'\0');auto written=GetEnvironmentVariableW(w.c_str(),value.data(),n);if(written>=n)throw library::IOException("Environment changed during read.");value.resize(written);return Narrow(value);
#else
        auto p=std::getenv(key.c_str());return p?native::String(p):native::String();
#endif
    }
    static bool Contains(native::String name){const auto key=SystemText(name);if(key.empty()||key.find('=')!=std::string::npos)throw library::ArgumentException("Invalid environment variable name.");std::lock_guard lock(EnvironmentMutex());
#ifdef _WIN32
        SetLastError(ERROR_SUCCESS);GetEnvironmentVariableW(Wide(key).c_str(),nullptr,0);return GetLastError()!=ERROR_ENVVAR_NOT_FOUND;
#else
        return std::getenv(key.c_str())!=nullptr;
#endif
    }
    static void Set(native::String name,native::String value){const auto key=SystemText(name),text=SystemText(value);if(key.empty()||key.find('=')!=std::string::npos)throw library::ArgumentException("Invalid environment variable name.");std::lock_guard lock(EnvironmentMutex());
#ifdef _WIN32
        if(!SetEnvironmentVariableW(Wide(key).c_str(),Wide(text).c_str()))throw library::IOException(SystemFailure("SetEnvironmentVariable",GetLastError()));
#else
        if(::setenv(key.c_str(),text.c_str(),1)!=0)throw library::IOException(SystemFailure("setenv",errno));
#endif
    }
    static void Remove(native::String name){const auto key=SystemText(name);if(key.empty()||key.find('=')!=std::string::npos)throw library::ArgumentException("Invalid environment variable name.");std::lock_guard lock(EnvironmentMutex());
#ifdef _WIN32
        if(!SetEnvironmentVariableW(Wide(key).c_str(),nullptr)&&GetLastError()!=ERROR_ENVVAR_NOT_FOUND)throw library::IOException(SystemFailure("SetEnvironmentVariable",GetLastError()));
#else
        if(::unsetenv(key.c_str())!=0)throw library::IOException(SystemFailure("unsetenv",errno));
#endif
    }
    static native::String CurrentDirectory(){return bolt::path_to_utf8(std::filesystem::current_path());}
    static void SetCurrentDirectory(native::String path){std::filesystem::current_path(bolt::path_from_utf8(SystemText(path)));}
    static native::String TemporaryDirectory(){return bolt::path_to_utf8(std::filesystem::temp_directory_path());}
    static std::int32_t ProcessorCount(){return static_cast<std::int32_t>(std::max(1u,std::thread::hardware_concurrency()));}
    static native::String OS(){
#ifdef _WIN32
return "Windows";
#elif defined(__APPLE__)
return "MacOS";
#elif defined(__FreeBSD__)
return "FreeBSD";
#else
return "Linux";
#endif
    }
    static native::String Architecture(){
#if defined(__aarch64__) || defined(_M_ARM64)
return "ARM64";
#else
return "X64";
#endif
    }
};
class ProcessHandle;
class PipeHandle {
    struct State {ProcessDescriptor handle=BadProcessDescriptor;bool readable=false;std::mutex io;
        State(ProcessDescriptor h,bool r):handle(h),readable(r){}~State(){CloseDescriptor(handle);}};
    std::shared_ptr<State> state_;
    PipeHandle(ProcessDescriptor h,bool r){try{state_=std::make_shared<State>(h,r);}catch(...){CloseDescriptor(h);throw;}}
    friend class ProcessHandle;
    void Check(bool read)const{if(!state_||state_->handle==BadProcessDescriptor)throw library::ObjectDisposedException("Pipe is closed or was not redirected.");if(state_->readable!=read)throw library::InvalidOperationException("Wrong pipe access direction.");}
public:
    PipeHandle()=default;
    void Close(){if(!state_)return;std::lock_guard lock(state_->io);CloseDescriptor(state_->handle);state_->handle=BadProcessDescriptor;}
    bool IsOpen()const{if(!state_)return false;std::lock_guard lock(state_->io);return state_->handle!=BadProcessDescriptor;}
    // -1 = timeout, zero = EOF (read) or zero-length request, positive = bytes.
    std::int32_t Transfer(std::uint8_t* bytes,std::int32_t count,std::int32_t timeout,bool read){CheckTimeout(timeout);if(count<0)throw library::ArgumentOutOfRangeException("Negative pipe byte count.");if(count&&!bytes)throw library::ArgumentNullException("Null pipe buffer.");if(!state_)throw library::ObjectDisposedException("Pipe is not redirected.");std::lock_guard lock(state_->io);Check(read);if(!count)return 0;
#ifdef _WIN32
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!operation.hEvent)throw library::IOException(SystemFailure("CreateEvent",GetLastError()));
        struct EventGuard{HANDLE h;~EventGuard(){CloseHandle(h);}} guard{operation.hEvent};DWORD n=0;
        BOOL ok=read?ReadFile(state_->handle,bytes,static_cast<DWORD>(count),&n,&operation):WriteFile(state_->handle,bytes,static_cast<DWORD>(count),&n,&operation);
        if(!ok){auto error=GetLastError();if(read&&(error==ERROR_BROKEN_PIPE||error==ERROR_PIPE_NOT_CONNECTED))return 0;if(error!=ERROR_IO_PENDING)throw library::IOException(SystemFailure("Pipe transfer",error));
            auto wait=WaitForSingleObject(operation.hEvent,timeout<0?INFINITE:static_cast<DWORD>(timeout));
            if(wait==WAIT_TIMEOUT){CancelIoEx(state_->handle,&operation);GetOverlappedResult(state_->handle,&operation,&n,TRUE);return n?static_cast<std::int32_t>(n):-1;}
            if(wait!=WAIT_OBJECT_0){auto e=GetLastError();CancelIoEx(state_->handle,&operation);GetOverlappedResult(state_->handle,&operation,&n,TRUE);throw library::IOException(SystemFailure("WaitForSingleObject",e));}
            if(!GetOverlappedResult(state_->handle,&operation,&n,FALSE)){auto e=GetLastError();if(read&&e==ERROR_BROKEN_PIPE)return 0;throw library::IOException(SystemFailure("Pipe result",e));}}
        return static_cast<std::int32_t>(n);
#else
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeout<0?0:timeout);
        for(;;){int wait=timeout;if(timeout>=0){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();wait=static_cast<int>(std::max<std::int64_t>(0,left));}
            pollfd fd{state_->handle,static_cast<short>(read?POLLIN:POLLOUT),0};int ready=::poll(&fd,1,wait);if(!ready)return -1;if(ready<0){if(errno==EINTR)continue;throw library::IOException(SystemFailure("poll pipe",errno));}
            ssize_t n;int error=0;
            if(read){n=::read(state_->handle,bytes,static_cast<std::size_t>(count));error=errno;}
            else { // Block SIGPIPE only for this write and consume only its new EPIPE signal.
                sigset_t mask,old,pending;sigemptyset(&mask);sigaddset(&mask,SIGPIPE);int e=pthread_sigmask(SIG_BLOCK,&mask,&old);if(e)throw library::IOException(SystemFailure("pthread_sigmask",e));sigpending(&pending);bool already=sigismember(&pending,SIGPIPE)==1;
                n=::write(state_->handle,bytes,static_cast<std::size_t>(count));error=errno;
                if(n<0&&error==EPIPE&&!already){sigpending(&pending);if(sigismember(&pending,SIGPIPE)==1){int signal=0;sigwait(&mask,&signal);}}pthread_sigmask(SIG_SETMASK,&old,nullptr);
            }
            if(n>=0)return static_cast<std::int32_t>(n);if(error==EINTR||error==EAGAIN||error==EWOULDBLOCK){if(timeout>=0&&std::chrono::steady_clock::now()>=deadline)return -1;continue;}throw library::IOException(SystemFailure(read?"read pipe":"write pipe",error));
        }
#endif
    }
    static std::int32_t Range(std::int32_t length,std::int32_t offset,std::int32_t count){if(offset<0||offset>length||count<0||count>length-offset)throw library::ArgumentOutOfRangeException("Invalid buffer range.");return count;}
    std::int32_t Read(native::Array<std::uint8_t>& bytes,std::int32_t offset,std::int32_t count,std::int32_t timeout){Range(bytes.Count(),offset,count);return Read(count?&bytes.At(offset):nullptr,count,timeout);}
    std::int32_t Write(const native::Array<std::uint8_t>& bytes,std::int32_t offset,std::int32_t count,std::int32_t timeout){Range(bytes.Count(),offset,count);return Write(count?&bytes.At(offset):nullptr,count,timeout);}
    std::int32_t Read(std::uint8_t* bytes,std::int32_t count,std::int32_t timeout=-1){return Transfer(bytes,count,timeout,true);}
    std::int32_t Write(const std::uint8_t* bytes,std::int32_t count,std::int32_t timeout=-1){return Transfer(const_cast<std::uint8_t*>(bytes),count,timeout,false);}
};
class CapturedProcess {
public:
    std::vector<std::uint8_t> out,err;std::int32_t code=0;bool timed=false;
    native::Array<std::uint8_t> Output()const{auto a=native::Array<std::uint8_t>::Create(static_cast<std::int32_t>(out.size()));for(std::size_t i=0;i<out.size();++i)a.At(static_cast<std::int32_t>(i))=out[i];return a;}
    native::Array<std::uint8_t> Error()const{auto a=native::Array<std::uint8_t>::Create(static_cast<std::int32_t>(err.size()));for(std::size_t i=0;i<err.size();++i)a.At(static_cast<std::int32_t>(i))=err[i];return a;}
    std::int32_t ExitCode()const{return code;}bool TimedOut()const{return timed;}
};
class ProcessHandle {
    struct State {
        std::mutex mutex;
#ifdef _WIN32
        HANDLE process=nullptr;DWORD id=0;
#else
        pid_t id=-1;
#endif
        bool done=false;std::int32_t exit=0,signal=0;PipeHandle input,output,error;
        bool Poll(){if(done)return true;
#ifdef _WIN32
            DWORD status=WaitForSingleObject(process,0);if(status==WAIT_TIMEOUT)return false;if(status!=WAIT_OBJECT_0)throw library::IOException(SystemFailure("process wait",GetLastError()));DWORD code;if(!GetExitCodeProcess(process,&code))throw library::IOException(SystemFailure("process status",GetLastError()));exit=static_cast<std::int32_t>(code);
#else
            int status=0;auto r=::waitpid(id,&status,WNOHANG);if(r==0)return false;if(r<0){if(errno==EINTR)return false;throw library::IOException(SystemFailure("waitpid",errno));}if(WIFEXITED(status))exit=WEXITSTATUS(status);else if(WIFSIGNALED(status)){signal=WTERMSIG(status);exit=128+signal;}else return false;
#endif
            done=true;return true;
        }
        void Kill(){if(done)return;
#ifdef _WIN32
            if(!TerminateProcess(process,137)&&GetLastError()!=ERROR_ACCESS_DENIED)throw library::IOException(SystemFailure("TerminateProcess",GetLastError()));
#else
            if(::kill(id,SIGKILL)!=0&&errno!=ESRCH)throw library::IOException(SystemFailure("kill",errno));
#endif
        }
        ~State(){try{if(
#ifdef _WIN32
            process
#else
            id>0
#endif
        ){if(!Poll()){Kill();
#ifdef _WIN32
            WaitForSingleObject(process,INFINITE);
#else
            int status;while(::waitpid(id,&status,0)<0&&errno==EINTR){}
#endif
        }}}catch(...){}
#ifdef _WIN32
            if(process)CloseHandle(process);
#endif
        }
    };
    std::shared_ptr<State> state_;
    void Check()const{if(!state_)throw library::InvalidOperationException("Process has not been started.");}
#ifdef _WIN32
    static std::wstring Quote(const std::wstring& s){std::wstring r=L"\"";std::size_t slashes=0;for(auto c:s){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){r.append(slashes*2+1,L'\\');r+=c;}else{r.append(slashes,L'\\');r+=c;}slashes=0;}r.append(slashes*2,L'\\');r+=L'\"';return r;}
#endif
public:
    ProcessHandle()=default;
    static ProcessHandle Start(native::String program,const native::Array<native::String>& arguments,native::String workingDirectory,bool redirect,const native::Array<native::String>& overrides,bool inheritEnvironment){
        auto executable=SystemText(program),cwd=SystemText(workingDirectory);if(executable.empty())throw library::ArgumentException("Executable path is empty.");if(!cwd.empty()&&!std::filesystem::is_directory(bolt::path_from_utf8(cwd)))throw library::DirectoryNotFoundException("Child working directory does not exist.");
        std::vector<std::string> args;args.reserve(static_cast<std::size_t>(arguments.Count())+1);args.push_back(executable);for(int i=0;i<arguments.Count();++i)args.push_back(SystemText(arguments.At(i)));
        auto result=ProcessHandle();auto state=std::make_shared<State>();
#ifdef _WIN32
        std::map<std::wstring,std::wstring,decltype([](const std::wstring& a,const std::wstring& b){return _wcsicmp(a.c_str(),b.c_str())<0;})> env;
        {std::lock_guard lock(EnvironmentMutex());if(inheritEnvironment){auto* list=GetEnvironmentStringsW();if(!list)throw library::IOException(SystemFailure("GetEnvironmentStrings",GetLastError()));for(auto* p=list;*p;p+=wcslen(p)+1){std::wstring entry(p);auto pos=entry.find(L'=',entry.starts_with(L"=")?1:0);if(pos!=entry.npos)env[entry.substr(0,pos)]=entry.substr(pos+1);}FreeEnvironmentStringsW(list);}}
        for(int i=0;i<overrides.Count();++i){auto entry=Wide(SystemText(overrides.At(i)));auto pos=entry.find(L'=');if(pos==0||pos==entry.npos)throw library::ArgumentException("Child environment overrides must be NAME=VALUE.");env[entry.substr(0,pos)]=entry.substr(pos+1);}
        std::vector<wchar_t> block;for(auto& [k,v]:env){auto entry=k+L"="+v;block.insert(block.end(),entry.begin(),entry.end());block.push_back(0);}if(block.empty())block.push_back(0);block.push_back(0);
        auto path=Wide(executable);if(executable.find_first_of("/\\")==std::string::npos){DWORD n=SearchPathW(nullptr,path.c_str(),L".exe",0,nullptr,nullptr);if(!n)throw library::FileNotFoundException("Executable not found: "+executable);std::wstring full(n+1,0);n=SearchPathW(nullptr,path.c_str(),L".exe",static_cast<DWORD>(full.size()),full.data(),nullptr);full.resize(n);path=full;}
        std::wstring command;for(auto& arg:args){if(!command.empty())command+=L' ';command+=Quote(Wide(arg));}std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0);
        STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);std::vector<HANDLE> child;child.reserve(3);std::vector<std::byte> attrs;
        struct ChildHandles {std::vector<HANDLE>& values;LPPROC_THREAD_ATTRIBUTE_LIST list=nullptr;~ChildHandles(){if(list)DeleteProcThreadAttributeList(list);for(auto h:values)CloseDescriptor(h);}} guards{child};
        if(redirect){static std::atomic<std::uint64_t> serial{1};
            auto make=[&](bool parentReads)->std::pair<PipeHandle,HANDLE>{auto name=L"\\\\.\\pipe\\Bolt-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(serial.fetch_add(1));
                auto server=CreateNamedPipeW(name.c_str(),(parentReads?PIPE_ACCESS_INBOUND:PIPE_ACCESS_OUTBOUND)|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr);if(server==INVALID_HANDLE_VALUE)throw library::IOException(SystemFailure("CreateNamedPipe",GetLastError()));
                SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};auto client=CreateFileW(name.c_str(),parentReads?GENERIC_WRITE:GENERIC_READ,0,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(client==INVALID_HANDLE_VALUE){auto e=GetLastError();CloseDescriptor(server);throw library::IOException(SystemFailure("Open child pipe",e));}
                // CreateFile has synchronously connected the client; the server is ready.
                try{return {PipeHandle(server,parentReads),client};}catch(...){CloseDescriptor(client);throw;}};
            auto in=make(false);state->input=in.first;child.push_back(in.second);auto out=make(true);state->output=out.first;child.push_back(out.second);auto err=make(true);state->error=err.first;child.push_back(err.second);
            startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;startup.StartupInfo.hStdInput=child[0];startup.StartupInfo.hStdOutput=child[1];startup.StartupInfo.hStdError=child[2];SIZE_T length=0;InitializeProcThreadAttributeList(nullptr,1,0,&length);attrs.resize(length);startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&length))throw library::IOException(SystemFailure("InitializeProcThreadAttributeList",GetLastError()));guards.list=startup.lpAttributeList;
            if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,child.data(),child.size()*sizeof(HANDLE),nullptr,nullptr))throw library::IOException(SystemFailure("Handle inheritance",GetLastError()));}
        else startup.StartupInfo.cb=sizeof(STARTUPINFOW);
        PROCESS_INFORMATION info{};auto wcwd=Wide(cwd);if(!CreateProcessW(path.c_str(),cmd.data(),nullptr,nullptr,redirect,CREATE_UNICODE_ENVIRONMENT|(redirect?EXTENDED_STARTUPINFO_PRESENT:0),block.data(),cwd.empty()?nullptr:wcwd.c_str(),&startup.StartupInfo,&info))throw library::IOException(SystemFailure("CreateProcess",GetLastError()));state->process=info.hProcess;state->id=info.dwProcessId;CloseHandle(info.hThread);
#else
        std::map<std::string,std::string> env;{std::lock_guard lock(EnvironmentMutex());if(inheritEnvironment)for(auto p=NativeEnvironment();p&&*p;++p){std::string s(*p);auto pos=s.find('=');if(pos!=s.npos)env[s.substr(0,pos)]=s.substr(pos+1);}}
        for(int i=0;i<overrides.Count();++i){auto s=SystemText(overrides.At(i));auto pos=s.find('=');if(!pos||pos==s.npos)throw library::ArgumentException("Child environment overrides must be NAME=VALUE.");env[s.substr(0,pos)]=s.substr(pos+1);}
        std::vector<std::string> entries;for(auto& [key,value]:env)entries.push_back(key+"="+value);std::vector<char*> envp;for(auto& s:entries)envp.push_back(s.data());envp.push_back(nullptr);std::vector<char*> argv;for(auto& s:args)argv.push_back(s.data());argv.push_back(nullptr);
        // Resolve PATH explicitly, so there is never an implicit /bin/sh fallback.
        if(executable.find('/')==executable.npos){std::string path=env.contains("PATH")?env["PATH"]:"/usr/bin:/bin";bool found=false;for(std::size_t begin=0;begin<=path.size();){auto end=path.find(':',begin);auto part=path.substr(begin,end==path.npos?path.size()-begin:end-begin);auto base=part.empty()?std::filesystem::path("."):bolt::path_from_utf8(part);if(base.is_relative()&&!cwd.empty())base=bolt::path_from_utf8(cwd)/base;auto full=base/executable;if(::access(full.c_str(),X_OK)==0&&!std::filesystem::is_directory(full)){executable=bolt::path_to_utf8(std::filesystem::absolute(full));found=true;break;}if(end==path.npos)break;begin=end+1;}if(!found)throw library::FileNotFoundException("Executable not found: "+executable);}
        else if(executable.front()!='/'&&!cwd.empty())executable=bolt::path_to_utf8(std::filesystem::absolute(bolt::path_from_utf8(cwd)/executable));
        struct Actions{posix_spawn_file_actions_t value;Actions(){int e=posix_spawn_file_actions_init(&value);if(e)throw library::IOException(SystemFailure("spawn actions",e));}~Actions(){posix_spawn_file_actions_destroy(&value);}} actions;
        std::vector<int> child;child.reserve(3);struct CloseChildren{std::vector<int>& values;~CloseChildren(){for(int f:values)::close(f);}} guard{child};
        auto action=[&](int error){if(error)throw library::IOException(SystemFailure("spawn file action",error));};
        if(redirect){auto make=[&](bool parentReads,int target){int p[2];
#ifdef __linux__
            if(::pipe2(p,O_CLOEXEC)!=0)throw library::IOException(SystemFailure("pipe2",errno));
#else
            if(::pipe(p)!=0)throw library::IOException(SystemFailure("pipe",errno));for(int i=0;i<2;++i)if(::fcntl(p[i],F_SETFD,FD_CLOEXEC)<0){auto e=errno;::close(p[0]);::close(p[1]);throw library::IOException(SystemFailure("fcntl pipe",e));}
#endif
            // Keep redirection descriptors away from stdin/out/err even when
            // the parent intentionally entered with a closed standard stream.
            for(int i=0;i<2;++i)if(p[i]<3){int moved=::fcntl(p[i],F_DUPFD_CLOEXEC,3);if(moved<0){auto e=errno;::close(p[0]);::close(p[1]);throw library::IOException(SystemFailure("duplicate pipe",e));}::close(p[i]);p[i]=moved;}
            int parent=parentReads?p[0]:p[1],kid=parentReads?p[1]:p[0];child.push_back(kid);PipeHandle handle(parent,parentReads);auto flags=::fcntl(parent,F_GETFL,0);if(flags<0||::fcntl(parent,F_SETFL,flags|O_NONBLOCK)<0)throw library::IOException(SystemFailure("nonblocking pipe",errno));action(posix_spawn_file_actions_adddup2(&actions.value,kid,target));action(posix_spawn_file_actions_addclose(&actions.value,parent));if(kid!=target)action(posix_spawn_file_actions_addclose(&actions.value,kid));return handle;};
            state->input=make(false,STDIN_FILENO);state->output=make(true,STDOUT_FILENO);state->error=make(true,STDERR_FILENO);}
#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
        if(!cwd.empty())throw library::InvalidOperationException("A child working directory is unavailable on iOS.");
#elif defined(__ANDROID__) && __ANDROID_API__ < 34
        if (!cwd.empty()) {
            using AddChdir = int (*)(posix_spawn_file_actions_t*, const char*);
            static const auto add_chdir = reinterpret_cast<AddChdir>(
                ::dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addchdir_np"));
            if (!add_chdir) throw library::InvalidOperationException("A child working directory requires Android API 34 or later.");
            action(add_chdir(&actions.value, cwd.c_str()));
        }
#else
        if(!cwd.empty())action(posix_spawn_file_actions_addchdir_np(&actions.value,cwd.c_str()));
#endif
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2,34)
        action(posix_spawn_file_actions_addclosefrom_np(&actions.value,3));
#endif
#endif
        posix_spawnattr_t attr;int init=posix_spawnattr_init(&attr);if(init)throw library::IOException(SystemFailure("spawn attributes",init));
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
        int flags_error=posix_spawnattr_setflags(&attr,POSIX_SPAWN_CLOEXEC_DEFAULT);if(flags_error){posix_spawnattr_destroy(&attr);throw library::IOException(SystemFailure("spawn close-on-exec attributes",flags_error));}
#endif
        pid_t pid;int e=posix_spawn(&pid,executable.c_str(),&actions.value,&attr,argv.data(),envp.data());posix_spawnattr_destroy(&attr);if(e){if(e==ENOENT)throw library::FileNotFoundException("Executable not found: "+executable);throw library::IOException(SystemFailure("posix_spawn",e));}state->id=pid;
#endif
        result.state_=std::move(state);return result;
    }
    bool Wait(std::int32_t milliseconds=-1){Check();CheckTimeout(milliseconds);auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds<0?0:milliseconds);for(;;){{std::lock_guard lock(state_->mutex);if(state_->Poll())return true;}if(milliseconds>=0&&std::chrono::steady_clock::now()>=deadline)return false;std::this_thread::sleep_for(std::chrono::milliseconds(1));}}
    void Kill(){Check();{std::lock_guard lock(state_->mutex);if(!state_->Poll())state_->Kill();}(void)Wait(-1);}
    bool IsRunning()const{Check();std::lock_guard lock(state_->mutex);return !state_->Poll();}
    std::int32_t ExitCode()const{Check();std::lock_guard lock(state_->mutex);if(!state_->Poll())throw library::InvalidOperationException("Process is still running.");return state_->exit;}
    std::int32_t TerminationSignal()const{Check();std::lock_guard lock(state_->mutex);if(!state_->Poll())throw library::InvalidOperationException("Process is still running.");return state_->signal;}
    std::int64_t Id()const{Check();return state_->id;}
    PipeHandle Input()const{Check();return state_->input;}PipeHandle Output()const{Check();return state_->output;}PipeHandle Error()const{Check();return state_->error;}
    void Close(){if(!state_)return;if(IsRunning())Kill();state_->input.Close();state_->output.Close();state_->error.Close();}
    CapturedProcess Capture(std::int32_t milliseconds=30000,std::int32_t maximumBytes=16777216){Check();if(milliseconds<0||maximumBytes<0)throw library::ArgumentOutOfRangeException("Capture needs a finite non-negative timeout and byte limit.");if(!state_->output.IsOpen()||!state_->error.IsOpen())throw library::InvalidOperationException("Capture requires redirected output.");state_->input.Close();CapturedProcess result;std::array<std::uint8_t,8192> buffer{};bool out=true,err=true;auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
        try{while(out||err||IsRunning()){
            if(std::chrono::steady_clock::now()>=deadline){result.timed=true;Kill();break;}bool progress=false;
            auto drain=[&](PipeHandle& pipe,std::vector<std::uint8_t>& data,bool& active){if(!active)return;int n=pipe.Read(buffer.data(),static_cast<int>(buffer.size()),0);if(n==0){active=false;return;}if(n>0){if(static_cast<std::size_t>(n)>static_cast<std::size_t>(maximumBytes)-std::min(static_cast<std::size_t>(maximumBytes),result.out.size()+result.err.size()))throw library::IOException("Captured stdout/stderr exceeded the configured total byte limit.");data.insert(data.end(),buffer.begin(),buffer.begin()+n);progress=true;}};
            drain(state_->output,result.out,out);drain(state_->error,result.err,err);if(!progress)std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            result.code=ExitCode();state_->output.Close();state_->error.Close();return result;
        }catch(...){try{Kill();}catch(...){}state_->output.Close();state_->error.Close();throw;}
    }
};
}
