#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "library/Exceptions.hpp"
#include "SystemProcess.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
namespace bolt::system {
#ifdef _WIN32
using SocketDescriptor=SOCKET;using SocketLength=int;inline constexpr SocketDescriptor InvalidSocket=INVALID_SOCKET;
inline int SocketError(){return WSAGetLastError();}
inline void CloseSocket(SocketDescriptor h)noexcept{if(h!=InvalidSocket)::closesocket(h);}
inline bool SocketPending(int e){return e==WSAEWOULDBLOCK||e==WSAEINPROGRESS||e==WSAEALREADY;}
inline bool SocketInterrupted(int e){return e==WSAEINTR;}
inline void EnsureNetwork(){struct Init{Init(){WSADATA data;int e=WSAStartup(MAKEWORD(2,2),&data);if(e)throw library::SocketException(SystemFailure("WSAStartup",e));}~Init(){WSACleanup();}};static Init initialized;(void)initialized;}
#else
using SocketDescriptor=int;using SocketLength=socklen_t;inline constexpr SocketDescriptor InvalidSocket=-1;
inline int SocketError(){return errno;}
inline void CloseSocket(SocketDescriptor h)noexcept{if(h>=0)::close(h);}
inline bool SocketPending(int e){return e==EAGAIN||e==EWOULDBLOCK||e==EINPROGRESS||e==EALREADY;}
inline bool SocketInterrupted(int e){return e==EINTR;}
inline void EnsureNetwork(){}
#endif
[[noreturn]] inline void ThrowSocket(const char* action,int code){throw library::SocketException(SystemFailure(action,code));}
class IPEndPoint;
class IPAddress {
    int family_=AF_INET;std::array<std::uint8_t,16> bytes_{};std::uint32_t scope_=0;
    friend class IPEndPoint;
public:
    IPAddress()=default;
    explicit IPAddress(native::String text){*this=Parse(text);}
    static IPAddress Parse(native::String input){auto text=SystemText(input);EnsureNetwork();IPAddress out;
        if(::inet_pton(AF_INET,text.c_str(),out.bytes_.data())==1)return out;
        auto percent=text.find('%');if(percent!=text.npos){auto tail=text.substr(percent+1);if(tail.empty())throw library::FormatException("Empty IPv6 scope id.");std::uint64_t scope=0;for(char c:tail){if(c<'0'||c>'9'||scope>(UINT32_MAX-static_cast<unsigned>(c-'0'))/10)throw library::FormatException("IPv6 scopes must be a uint numeric interface index.");scope=scope*10+static_cast<unsigned>(c-'0');}out.scope_=static_cast<std::uint32_t>(scope);text.resize(percent);}
        if(::inet_pton(AF_INET6,text.c_str(),out.bytes_.data())==1){out.family_=AF_INET6;return out;}
        throw library::FormatException("Invalid IPv4 or IPv6 address.");
    }
    static bool TryParse(native::String text,IPAddress& output){try{auto value=Parse(text);output=value;return true;}catch(const library::FormatException&){return false;}catch(const library::ArgumentException&){return false;}}
    native::String ToString()const{char buffer[INET6_ADDRSTRLEN]{};if(!::inet_ntop(family_,bytes_.data(),buffer,sizeof(buffer)))ThrowSocket("inet_ntop",SocketError());std::string text(buffer);if(family_==AF_INET6&&scope_)text+="%"+std::to_string(scope_);return text;}
    bool IsIPv6()const{return family_==AF_INET6;}
    static IPAddress Any(){return Parse("0.0.0.0");}static IPAddress Loopback(){return Parse("127.0.0.1");}static IPAddress IPv6Any(){return Parse("::");}static IPAddress IPv6Loopback(){return Parse("::1");}
    bool operator==(const IPAddress& o)const noexcept{return family_==o.family_&&bytes_==o.bytes_&&scope_==o.scope_;}
    std::size_t BoltHash()const noexcept{std::size_t hash=static_cast<std::size_t>(family_);for(auto b:bytes_)hash=(hash*131)^b;return hash^scope_;}
};
class IPEndPoint {
    sockaddr_storage address_{};SocketLength length_=sizeof(sockaddr_in);
public:
    IPEndPoint(){reinterpret_cast<sockaddr*>(&address_)->sa_family=AF_INET;}
    IPEndPoint(IPAddress address,std::int32_t port){if(port<0||port>65535)throw library::ArgumentOutOfRangeException("Port must be between 0 and 65535.");
        if(address.family_==AF_INET){auto& a=*reinterpret_cast<sockaddr_in*>(&address_);a.sin_family=AF_INET;a.sin_port=htons(static_cast<std::uint16_t>(port));std::memcpy(&a.sin_addr,address.bytes_.data(),4);length_=sizeof(a);
#if defined(__APPLE__) || defined(__FreeBSD__)
            a.sin_len=sizeof(a);
#endif
        }
        else{auto& a=*reinterpret_cast<sockaddr_in6*>(&address_);a.sin6_family=AF_INET6;a.sin6_port=htons(static_cast<std::uint16_t>(port));a.sin6_scope_id=address.scope_;std::memcpy(&a.sin6_addr,address.bytes_.data(),16);length_=sizeof(a);
#if defined(__APPLE__) || defined(__FreeBSD__)
            a.sin6_len=sizeof(a);
#endif
        }
    }
    static IPEndPoint FromNative(const sockaddr* address,SocketLength length){if(!address||length<sizeof(address->sa_family))throw library::SocketException("Truncated socket address.");SocketLength needed=address->sa_family==AF_INET?sizeof(sockaddr_in):address->sa_family==AF_INET6?sizeof(sockaddr_in6):0;if(!needed||length<needed||static_cast<std::size_t>(length)>sizeof(sockaddr_storage))throw library::SocketException("Unsupported socket address.");IPEndPoint out;std::memcpy(&out.address_,address,needed);out.length_=needed;return out;}
    const sockaddr* Native()const{return reinterpret_cast<const sockaddr*>(&address_);}SocketLength NativeLength()const{return length_;}
    IPAddress Address()const{IPAddress out;out.family_=Native()->sa_family;if(out.family_==AF_INET)std::memcpy(out.bytes_.data(),&reinterpret_cast<const sockaddr_in*>(&address_)->sin_addr,4);else{auto& a=*reinterpret_cast<const sockaddr_in6*>(&address_);std::memcpy(out.bytes_.data(),&a.sin6_addr,16);out.scope_=a.sin6_scope_id;}return out;}
    std::int32_t Port()const{return ntohs(Native()->sa_family==AF_INET?reinterpret_cast<const sockaddr_in*>(&address_)->sin_port:reinterpret_cast<const sockaddr_in6*>(&address_)->sin6_port);}
    native::String ToString()const{auto a=Address();auto text=static_cast<std::string>(a.ToString());return (a.IsIPv6()?"["+text+"]":text)+":"+std::to_string(Port());}
    bool operator==(const IPEndPoint& o)const{return Address()==o.Address()&&Port()==o.Port();}
    std::size_t BoltHash()const{return Address().BoltHash()^static_cast<std::size_t>(Port());}
};
class Resolver {
public:
    static native::Array<IPAddress> Resolve(native::String hostname){EnsureNetwork();auto host=SystemText(hostname);if(host.empty())throw library::ArgumentException("Hostname is empty.");addrinfo hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;addrinfo* result=nullptr;int e=::getaddrinfo(host.c_str(),nullptr,&hints,&result);if(e)throw library::SocketException(SystemFailure("getaddrinfo",e));struct Guard{addrinfo* p;~Guard(){freeaddrinfo(p);}} guard{result};std::vector<IPAddress> values;for(auto* p=result;p;p=p->ai_next)if(p->ai_family==AF_INET||p->ai_family==AF_INET6){auto a=IPEndPoint::FromNative(p->ai_addr,static_cast<SocketLength>(p->ai_addrlen)).Address();if(std::find(values.begin(),values.end(),a)==values.end())values.push_back(a);}auto out=native::Array<IPAddress>::Create(static_cast<std::int32_t>(values.size()));for(std::size_t i=0;i<values.size();++i)out.At(static_cast<std::int32_t>(i))=values[i];return out;}
};
class SocketHandle {
    struct State {SocketDescriptor handle=InvalidSocket;int family=AF_INET;bool datagram=false;std::int32_t receive=-1,send=-1;~State(){CloseSocket(handle);}};
    std::shared_ptr<State> state_;
    void Check()const{if(!state_||state_->handle==InvalidSocket)throw library::ObjectDisposedException("Socket is closed or uninitialized.");}
    static void Prepare(SocketDescriptor fd){
#ifdef _WIN32
        u_long enabled=1;if(ioctlsocket(fd,FIONBIO,&enabled)!=0)ThrowSocket("nonblocking socket",SocketError());if(!SetHandleInformation(reinterpret_cast<HANDLE>(fd),HANDLE_FLAG_INHERIT,0))throw library::SocketException(SystemFailure("socket handle inheritance",GetLastError()));
#else
        auto flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0||fcntl(fd,F_SETFD,FD_CLOEXEC)<0)ThrowSocket("socket flags",errno);
#ifdef SO_NOSIGPIPE
        int one=1;if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one))!=0)ThrowSocket("SO_NOSIGPIPE",errno);
#endif
#endif
    }
    using Clock=std::chrono::steady_clock;
    static Clock::time_point Deadline(std::int32_t ms){return Clock::now()+std::chrono::milliseconds(ms<0?0:ms);}
    bool Wait(bool read,std::int32_t timeout,Clock::time_point deadline)const{Check();for(;;){int ms=timeout;if(timeout>=0){auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();ms=static_cast<int>(std::max<std::int64_t>(0,left));}
#ifdef _WIN32
        WSAPOLLFD pollfd{state_->handle,static_cast<SHORT>(read?POLLRDNORM:POLLWRNORM),0};int result=WSAPoll(&pollfd,1,ms);
#else
        pollfd pollfd{state_->handle,static_cast<short>(read?POLLIN:POLLOUT),0};int result=::poll(&pollfd,1,ms);
#endif
        if(result>0)return true;if(result==0)return false;auto error=SocketError();if(SocketInterrupted(error))continue;ThrowSocket("poll socket",error);}}
    void NeedWait(bool read,std::int32_t timeout,Clock::time_point deadline)const{if(!Wait(read,timeout,deadline)){if(timeout==0)throw library::SocketWouldBlockException("Socket operation would block.");throw library::TimeoutException("Socket operation timed out.");}}
    static void Buffer(const void* data,std::int32_t count){if(count<0)throw library::ArgumentOutOfRangeException("Negative socket byte count.");if(count&&!data)throw library::ArgumentNullException("Null socket buffer.");}
public:
    SocketHandle()=default;
    static SocketHandle Create(bool ipv6=false,bool datagram=false){EnsureNetwork();SocketHandle result;result.state_=std::make_shared<State>();result.state_->family=ipv6?AF_INET6:AF_INET;result.state_->datagram=datagram;
#ifdef _WIN32
        result.state_->handle=::WSASocketW(result.state_->family,datagram?SOCK_DGRAM:SOCK_STREAM,0,nullptr,0,WSA_FLAG_OVERLAPPED|WSA_FLAG_NO_HANDLE_INHERIT);
#else
        result.state_->handle=::socket(result.state_->family,(datagram?SOCK_DGRAM:SOCK_STREAM)
#ifdef SOCK_CLOEXEC
        |SOCK_CLOEXEC
#endif
        ,0);
#endif
        if(result.state_->handle==InvalidSocket)ThrowSocket("socket",SocketError());Prepare(result.state_->handle);if(ipv6){int one=1;if(::setsockopt(result.state_->handle,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<const char*>(&one),sizeof(one))!=0)ThrowSocket("IPV6_V6ONLY",SocketError());}return result;}
    void Close(){if(state_&&state_->handle!=InvalidSocket){CloseSocket(state_->handle);state_->handle=InvalidSocket;}}
    bool IsOpen()const{return state_&&state_->handle!=InvalidSocket;}
    void SetTimeouts(std::int32_t receive=-1,std::int32_t send=-1){Check();CheckTimeout(receive);CheckTimeout(send);state_->receive=receive;state_->send=send;}
    void Bind(IPEndPoint endpoint){Check();if(endpoint.Native()->sa_family!=state_->family)throw library::ArgumentException("Socket/address family mismatch.");if(::bind(state_->handle,endpoint.Native(),endpoint.NativeLength())!=0)ThrowSocket("bind",SocketError());}
    void Listen(std::int32_t backlog=128){Check();if(state_->datagram||backlog<1)throw library::ArgumentException("Listen requires TCP and a positive backlog.");if(::listen(state_->handle,backlog)!=0)ThrowSocket("listen",SocketError());}
    SocketHandle Accept(){Check();if(state_->datagram)throw library::InvalidOperationException("UDP does not accept connections.");auto deadline=Deadline(state_->receive);for(;;){auto fd=::accept(state_->handle,nullptr,nullptr);if(fd!=InvalidSocket){SocketHandle value;try{value.state_=std::make_shared<State>();}catch(...){CloseSocket(fd);throw;}value.state_->handle=fd;value.state_->family=state_->family;value.state_->receive=state_->receive;value.state_->send=state_->send;Prepare(fd);return value;}int error=SocketError();if(SocketInterrupted(error))continue;if(!SocketPending(error))ThrowSocket("accept",error);NeedWait(true,state_->receive,deadline);}}
    void Connect(IPEndPoint endpoint){Check();if(endpoint.Native()->sa_family!=state_->family)throw library::ArgumentException("Socket/address family mismatch.");int result=::connect(state_->handle,endpoint.Native(),endpoint.NativeLength());if(result==0)return;int error=SocketError();if(!SocketPending(error)&&!SocketInterrupted(error))ThrowSocket("connect",error);auto deadline=Deadline(state_->send);NeedWait(false,state_->send,deadline);int status=0;SocketLength size=sizeof(status);if(::getsockopt(state_->handle,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&status),&size)!=0)ThrowSocket("connect status",SocketError());if(status)ThrowSocket("connect",status);}
    std::int32_t Send(const std::uint8_t* data,std::int32_t count){Check();if(state_->datagram)throw library::InvalidOperationException("Use SendTo for datagrams.");Buffer(data,count);if(!count)return 0;auto deadline=Deadline(state_->send);for(;;){auto n=::send(state_->handle,reinterpret_cast<const char*>(data),count,
#ifdef MSG_NOSIGNAL
MSG_NOSIGNAL
#else
0
#endif
);if(n>=0)return static_cast<std::int32_t>(n);auto e=SocketError();if(SocketInterrupted(e))continue;if(!SocketPending(e))ThrowSocket("send",e);NeedWait(false,state_->send,deadline);}}
    std::int32_t Receive(std::uint8_t* data,std::int32_t count){Check();if(state_->datagram)throw library::InvalidOperationException("Use ReceiveFrom for datagrams and truncation reporting.");Buffer(data,count);if(!count)return 0;auto deadline=Deadline(state_->receive);for(;;){auto n=::recv(state_->handle,reinterpret_cast<char*>(data),count,0);if(n>=0)return static_cast<std::int32_t>(n);auto e=SocketError();if(SocketInterrupted(e))continue;if(!SocketPending(e))ThrowSocket("recv",e);NeedWait(true,state_->receive,deadline);}}
    std::int32_t SendTo(const std::uint8_t* data,std::int32_t count,IPEndPoint destination){Check();Buffer(data,count);if(!state_->datagram)throw library::InvalidOperationException("SendTo requires UDP.");if(destination.Native()->sa_family!=state_->family)throw library::ArgumentException("Socket/address family mismatch.");auto deadline=Deadline(state_->send);for(;;){auto n=::sendto(state_->handle,reinterpret_cast<const char*>(data),count,
#ifdef MSG_NOSIGNAL
MSG_NOSIGNAL
#else
0
#endif
,destination.Native(),destination.NativeLength());if(n>=0){if(n!=count)throw library::SocketException("A partial datagram send is not a successful SendTo.");return static_cast<std::int32_t>(n);}auto e=SocketError();if(SocketInterrupted(e))continue;if(!SocketPending(e))ThrowSocket("sendto",e);NeedWait(false,state_->send,deadline);}}
    std::int32_t ReceiveFrom(std::uint8_t* data,std::int32_t count,IPEndPoint& sender,bool& truncated){Check();Buffer(data,count);if(!state_->datagram)throw library::InvalidOperationException("ReceiveFrom requires UDP.");auto deadline=Deadline(state_->receive);for(;;){sockaddr_storage remote{};SocketLength size=sizeof(remote);bool cut=false;
#ifdef _WIN32
        char dummy=0;WSABUF buffer{static_cast<ULONG>(count),count?reinterpret_cast<char*>(data):&dummy};DWORD n=0,flags=0;int result=WSARecvFrom(state_->handle,&buffer,1,&n,&flags,reinterpret_cast<sockaddr*>(&remote),&size,nullptr,nullptr);int e=result==0?0:SocketError();if(result==0||e==WSAEMSGSIZE){cut=e==WSAEMSGSIZE;auto received=cut?count:static_cast<std::int32_t>(n);sender=IPEndPoint::FromNative(reinterpret_cast<sockaddr*>(&remote),size);truncated=cut;return received;}
#else
        iovec buffer{data,static_cast<std::size_t>(count)};msghdr message{};message.msg_name=&remote;message.msg_namelen=size;message.msg_iov=&buffer;message.msg_iovlen=1;auto n=::recvmsg(state_->handle,&message,0);int e=n>=0?0:SocketError();if(n>=0){cut=(message.msg_flags&MSG_TRUNC)!=0;sender=IPEndPoint::FromNative(reinterpret_cast<sockaddr*>(&remote),message.msg_namelen);truncated=cut;return static_cast<std::int32_t>(std::min<decltype(n)>(n,count));}
#endif
        if(SocketInterrupted(e))continue;if(!SocketPending(e))ThrowSocket("receive datagram",e);NeedWait(true,state_->receive,deadline);}}
    std::int32_t Send(const native::Array<std::uint8_t>& bytes,std::int32_t offset,std::int32_t count){PipeHandle::Range(bytes.Count(),offset,count);return Send(count?&bytes.At(offset):nullptr,count);}
    std::int32_t Receive(native::Array<std::uint8_t>& bytes,std::int32_t offset,std::int32_t count){PipeHandle::Range(bytes.Count(),offset,count);return Receive(count?&bytes.At(offset):nullptr,count);}
    std::int32_t SendTo(const native::Array<std::uint8_t>& bytes,IPEndPoint destination){return SendTo(bytes.Count()?&bytes.At(0):nullptr,bytes.Count(),destination);}
    std::int32_t ReceiveFrom(native::Array<std::uint8_t>& bytes,IPEndPoint& sender,bool& truncated){return ReceiveFrom(bytes.Count()?&bytes.At(0):nullptr,bytes.Count(),sender,truncated);}
    void Shutdown(bool receive=true,bool send=true){Check();if(!receive&&!send)return;
#ifdef _WIN32
        int how=receive?(send?SD_BOTH:SD_RECEIVE):SD_SEND;
#else
        int how=receive?(send?SHUT_RDWR:SHUT_RD):SHUT_WR;
#endif
        if(::shutdown(state_->handle,how)!=0)ThrowSocket("shutdown",SocketError());}
    IPEndPoint LocalEndpoint()const{Check();sockaddr_storage a{};SocketLength size=sizeof(a);if(::getsockname(state_->handle,reinterpret_cast<sockaddr*>(&a),&size)!=0)ThrowSocket("getsockname",SocketError());return IPEndPoint::FromNative(reinterpret_cast<sockaddr*>(&a),size);}
    IPEndPoint RemoteEndpoint()const{Check();sockaddr_storage a{};SocketLength size=sizeof(a);if(::getpeername(state_->handle,reinterpret_cast<sockaddr*>(&a),&size)!=0)ThrowSocket("getpeername",SocketError());return IPEndPoint::FromNative(reinterpret_cast<sockaddr*>(&a),size);}
};
}
