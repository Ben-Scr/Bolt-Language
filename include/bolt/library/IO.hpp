#pragma once
#include "Exceptions.hpp"
#include "../Std.hpp"
#include "../Path.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace bolt::library {
    namespace io_detail {
        inline std::filesystem::path path(const native::String& value) {
            if(value.empty())throw ArgumentException("The path must not be empty.");
            if(value.find('\0')!=native::String::npos)throw ArgumentException("The path contains a NUL character.");
            return path_from_utf8(value);
        }
        [[noreturn]] inline void error(const std::filesystem::path& path,const std::error_code& ec,const char* operation) {
            native::String message=std::string(operation)+" '"+path_to_utf8(path)+"': "+ec.message();
            if(ec==std::errc::permission_denied)throw UnauthorizedAccessException(message);
            if(ec==std::errc::no_such_file_or_directory)throw FileNotFoundException(message);
            throw IOException(message);
        }
        inline std::size_t size(std::int64_t value) {
            if(value<0||static_cast<std::uint64_t>(value)>std::numeric_limits<std::size_t>::max())throw ArgumentOutOfRangeException("Size must be non-negative and fit this platform.");
            return static_cast<std::size_t>(value);
        }
        inline void bounds(std::int32_t total,std::int32_t& offset,std::int32_t& count) {
            if(offset<0||offset>total)throw ArgumentOutOfRangeException("Buffer offset is outside the array.");
            if(count==-1)count=total-offset;
            if(count<0||count>total-offset)throw ArgumentOutOfRangeException("Buffer count exceeds the available array range.");
        }
    }
    enum class FileMode:std::int32_t {
        CreateNew=1,Create=2,Open=3,OpenOrCreate=4,Truncate=5,Append=6
    };
    enum class FileAccess:std::int32_t {
        Read=1,Write=2,ReadWrite=3
    };
    enum class SeekOrigin:std::int32_t {
        Begin=0,Current=1,End=2
    };

    class Stream {
        public:
        virtual ~Stream()=default;
        virtual const void* Identity()const noexcept {
            return this;
        }
        virtual std::shared_ptr<Stream> CloneHandle() const=0;
        virtual bool CanRead()const=0;
        virtual bool CanWrite()const=0;
        virtual bool CanSeek()const=0;
        virtual std::int64_t Length()const=0;
        virtual std::int64_t Position()const=0;
        virtual std::size_t ReadRaw(std::uint8_t* data,std::size_t length)=0;
        virtual void WriteRaw(const std::uint8_t* data,std::size_t length)=0;
        virtual std::int64_t Seek(std::int64_t offset,SeekOrigin origin=SeekOrigin::Begin)=0;
        virtual void SetLength(std::int64_t length)=0;
        virtual void Flush()=0;
        virtual void Close()=0;
        void Dispose() {
            Close();
        }
        std::int32_t Read(native::Array<std::uint8_t>& buffer,std::int32_t offset=0,std::int32_t count=-1) {
            io_detail::bounds(buffer.Count(),offset,count);
            return static_cast<std::int32_t>(ReadRaw(count?&buffer.At(offset):nullptr,static_cast<std::size_t>(count)));
        }
        std::int32_t ReadByte() {
            std::uint8_t b=0;
            return ReadRaw(&b,1)==0?-1:static_cast<std::int32_t>(b);
        }
        void Write(const native::Array<std::uint8_t>& buffer,std::int32_t offset=0,std::int32_t count=-1) {
            io_detail::bounds(buffer.Count(),offset,count);
            WriteRaw(count?&buffer.At(offset):nullptr,static_cast<std::size_t>(count));
        }
        void WriteByte(std::uint8_t value) {
            WriteRaw(&value,1);
        }
        native::Array<std::uint8_t> ReadAllBytes() {
            std::vector<std::uint8_t> data;
            std::uint8_t block[8192];
            for(;;) {
                auto n=ReadRaw(block,sizeof(block));
                if(!n)break;
                if(data.size()+n>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))throw IOException("The stream is too large for a Bolt array.");
                data.insert(data.end(),block,block+n);
            }
            return native::Array<std::uint8_t>::FromRange(data.begin(),data.end());
        }
        void CopyTo(Stream& destination,std::int32_t bufferSize=81920) {
            if(bufferSize<=0)throw ArgumentOutOfRangeException("Buffer size must be positive.");
            if(Identity()==destination.Identity())throw ArgumentException("Cannot copy a stream into itself or an alias of itself.");
            std::vector<std::uint8_t> data(static_cast<std::size_t>(bufferSize));
            for(;;) {
                auto n=ReadRaw(data.data(),data.size());
                if(!n)break;
                destination.WriteRaw(data.data(),n);
            }
        }
    };

    class FileStream final: public Stream {
        enum class Cursor { Positioned, Reading, Writing, NeedsSeek };
        struct State {
            std::filesystem::path path;
            std::FILE* file=nullptr;
            mutable std::mutex mutex;
            std::int64_t position=0;
            Cursor cursor=Cursor::Positioned;
            bool read=false,write=false,closed=true,append=false;
            ~State() {
                if(file)std::fclose(file);
            }
        };
        std::shared_ptr<State> state_;
        State& state()const {
            if(!state_)throw ObjectDisposedException("The file stream has no open file.");
            return *state_;
        }
        static void open(const State& s) {
            if(s.closed||!s.file)throw ObjectDisposedException("The file stream is closed.");
        }
        [[noreturn]] static void failure(const State& s,const char* operation) {
            io_detail::error(s.path,std::error_code(errno?errno:EIO,std::generic_category()),operation);
        }
        static void seek(State& s,std::int64_t offset,int origin) {
            s.cursor=Cursor::NeedsSeek;
#ifdef _WIN32
            if(_fseeki64(s.file,offset,origin)!=0)failure(s,"Seek");
#else
            if constexpr(sizeof(off_t)<sizeof(std::int64_t))if(offset>std::numeric_limits<off_t>::max())throw ArgumentOutOfRangeException("File offset does not fit this platform.");
            if(::fseeko(s.file,static_cast<off_t>(offset),origin)!=0)failure(s,"Seek");
#endif
            s.cursor=Cursor::Positioned;
        }
        static std::int64_t tell(State& s) {
#ifdef _WIN32
            auto result=_ftelli64(s.file);
#else
            auto result=::ftello(s.file);
#endif
            if(result<0)failure(s,"Read position");
            return native::cast<std::int64_t>(result);
        }
        static void flush(State& s) {
            if(s.write&&std::fflush(s.file)!=0)failure(s,"Flush");
        }
        static std::int64_t length(State& s) {
            flush(s);
#ifdef _WIN32
            const auto handle = _get_osfhandle(_fileno(s.file));
            LARGE_INTEGER size;
            if (handle != -1 && GetFileSizeEx(reinterpret_cast<HANDLE>(handle), &size))
                return size.QuadPart;
            // Some devices support the CRT stat interface but not GetFileSizeEx.
            struct _stati64 info {
            };
            if(_fstati64(_fileno(s.file),&info)!=0)failure(s,"Read length");
#else
            struct stat info {
            };
            if(::fstat(fileno(s.file),&info)!=0)failure(s,"Read length");
#endif
            return native::cast<std::int64_t>(info.st_size);
        }
        public:
        FileStream()=default;
        FileStream(const native::String& name,FileMode mode=FileMode::Open,FileAccess access=FileAccess::ReadWrite):state_(std::make_shared<State>()) {
            auto& s=*state_;
            s.path=io_detail::path(name);
            s.read=access==FileAccess::Read||access==FileAccess::ReadWrite;
            s.write=access==FileAccess::Write||access==FileAccess::ReadWrite;
            if(!s.read&&!s.write)throw ArgumentException("Invalid FileAccess.");
            if(mode<FileMode::CreateNew||mode>FileMode::Append)throw ArgumentException("Invalid FileMode.");
            if((mode==FileMode::CreateNew||mode==FileMode::Create||mode==FileMode::Truncate||mode==FileMode::Append)&&!s.write)throw ArgumentException("This file mode requires write access.");
            // Open and claim the file atomically; never check existence and later
            // truncate it. fdopen adopts that same descriptor (no path reopen race).
            int flags=s.read?(s.write?O_RDWR:O_RDONLY):O_WRONLY;
            switch(mode) {
                case FileMode::CreateNew:flags|=O_CREAT|O_EXCL;
                break;
                case FileMode::Create:flags|=O_CREAT|O_TRUNC;
                break;
                case FileMode::Open:break;
                case FileMode::OpenOrCreate:flags|=O_CREAT;
                break;
                case FileMode::Truncate:flags|=O_TRUNC;
                break;
                case FileMode::Append:flags|=O_CREAT|O_APPEND;
                break;
            }
            const char* fdmode=s.read?(s.write?(mode==FileMode::Append?"a+b":"r+b"):"rb"):(mode==FileMode::Append?"ab":"wb");
#ifdef _WIN32
            int fd=-1;
            int rc=_wsopen_s(&fd,s.path.c_str(),flags|_O_BINARY|_O_NOINHERIT,_SH_DENYNO,_S_IREAD|_S_IWRITE);
            if(rc!=0) {
                errno=rc;
                failure(s,"Open");
            }
            s.file=_fdopen(fd,fdmode);
            if(!s.file) {
                int saved=errno;
                _close(fd);
                errno=saved;
                failure(s,"Open stream");
            }
#else
            int fd=::open(s.path.c_str(),flags|O_CLOEXEC,0666);
            if(fd<0)failure(s,"Open");
            s.file=::fdopen(fd,fdmode);
            if(!s.file) {
                int saved=errno;
                ::close(fd);
                errno=saved;
                failure(s,"Open stream");
            }
#endif
            s.closed=false;
            s.append=mode==FileMode::Append;
            if(s.append) {
                seek(s,0,SEEK_END);
                s.position=tell(s);
            }
        }
        const void* Identity()const noexcept override {
            return state_.get();
        }
        std::shared_ptr<Stream> CloneHandle()const override {
            return std::make_shared<FileStream>(*this);
        }
        bool CanRead()const override {
            if(!state_)return false;
            auto& s=*state_;
            std::lock_guard lock(s.mutex);
            return !s.closed&&s.read;
        }
        bool CanWrite()const override {
            if(!state_)return false;
            auto& s=*state_;
            std::lock_guard lock(s.mutex);
            return !s.closed&&s.write;
        }
        bool CanSeek()const override {
            if(!state_)return false;
            auto& s=*state_;
            std::lock_guard lock(s.mutex);
            return !s.closed;
        }
        std::int64_t Length()const override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            return length(s);
        }
        std::int64_t Position()const override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            return s.position;
        }
        std::size_t ReadRaw(std::uint8_t* data,std::size_t amount)override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            if(!s.read)throw NotSupportedException("This stream is not readable.");
            if(!amount)return 0;
            // Update streams require a positioning operation when changing I/O direction.
            // Sequential transfers already start at the CRT's logical position.
            if(s.cursor==Cursor::Writing || s.cursor==Cursor::NeedsSeek)seek(s,s.position,SEEK_SET);
            std::clearerr(s.file);
            s.cursor=Cursor::NeedsSeek;
            const auto count=std::fread(data,1,amount,s.file);
            if(std::ferror(s.file))failure(s,"Read");
            s.position=native::add<std::int64_t>(s.position,native::cast<std::int64_t>(count));
            s.cursor=Cursor::Reading;
            return count;
        }
        void WriteRaw(const std::uint8_t* data,std::size_t amount)override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            if(!s.write)throw NotSupportedException("This stream is not writable.");
            if(!amount)return;
            if(s.append)seek(s,0,SEEK_END);
            else if(s.cursor==Cursor::Reading || s.cursor==Cursor::NeedsSeek)seek(s,s.position,SEEK_SET);
            s.cursor=Cursor::NeedsSeek;
            std::clearerr(s.file);
            if(std::fwrite(data,1,amount,s.file)!=amount)failure(s,"Write");
            s.position=s.append?tell(s):native::add<std::int64_t>(s.position,native::cast<std::int64_t>(amount));
            s.cursor=Cursor::Writing;
        }
        std::int64_t Seek(std::int64_t offset,SeekOrigin origin=SeekOrigin::Begin)override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            std::int64_t base=0;
            if(origin==SeekOrigin::Current)base=s.position;
            else if(origin==SeekOrigin::End)base=length(s);
            else if(origin!=SeekOrigin::Begin)throw ArgumentException("Invalid SeekOrigin.");
            auto position=native::add<std::int64_t>(base,offset);
            if(position<0)throw ArgumentOutOfRangeException("Cannot seek before the beginning of a stream.");
            seek(s,position,SEEK_SET);
            s.position=position;
            return position;
        }
        void SetLength(std::int64_t value)override {
            if(value<0)throw ArgumentOutOfRangeException("File length must be non-negative.");
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            if(!s.write)throw NotSupportedException("This stream is not writable.");
            flush(s);
#ifdef _WIN32
            auto rc=_chsize_s(_fileno(s.file),static_cast<__int64>(value));
            if(rc!=0) {
                errno=rc;
                failure(s,"Set length");
            }
#else
            if constexpr(sizeof(off_t)<sizeof(std::int64_t))if(value>std::numeric_limits<off_t>::max())throw ArgumentOutOfRangeException("File length does not fit this platform.");
            if(::ftruncate(fileno(s.file),static_cast<off_t>(value))!=0)failure(s,"Set length");
#endif
            if(s.position>value)s.position=value;
            seek(s,s.position,SEEK_SET);
        }
        void Flush()override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            flush(s);
        }
        void Close()override {
            if(!state_)return;
            auto& s=*state_;
            std::lock_guard lock(s.mutex);
            if(s.closed)return;
            auto* file=std::exchange(s.file,nullptr);
            s.closed=true;
            if(std::fclose(file)!=0)failure(s,"Close");
        }
    };

    class MemoryStream final: public Stream {
        struct State {
            std::vector<std::uint8_t> bytes;
            std::size_t position=0;
            bool closed=false;
            mutable std::mutex mutex;
        };
        std::shared_ptr<State> state_=std::make_shared<State>();
        State& state()const {
            if(!state_)throw ObjectDisposedException("The memory stream is empty.");
            return *state_;
        }
        static void open(State& s) {
            if(s.closed)throw ObjectDisposedException("The memory stream is closed.");
        }
        public:
        const void* Identity()const noexcept override {
            return state_.get();
        }
        MemoryStream()=default;
        explicit MemoryStream(const native::Array<std::uint8_t>& data) {
            auto& s=*state_;
            s.bytes.assign(data.begin(),data.end());
        }
        explicit MemoryStream(std::int32_t capacity) {
            if(capacity<0)throw ArgumentOutOfRangeException("Capacity must not be negative.");
            state_->bytes.reserve(static_cast<std::size_t>(capacity));
        }
        std::shared_ptr<Stream> CloneHandle()const override {
            return std::make_shared<MemoryStream>(*this);
        }
        bool CanRead()const override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            return !s.closed;
        }
        bool CanWrite()const override {
            return CanRead();
        }
        bool CanSeek()const override {
            return CanRead();
        }
        std::int64_t Length()const override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            return native::cast<std::int64_t>(s.bytes.size());
        }
        std::int64_t Position()const override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            return native::cast<std::int64_t>(s.position);
        }
        std::size_t ReadRaw(std::uint8_t* data,std::size_t length)override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            auto count=std::min(length,s.position>=s.bytes.size()?std::size_t {
                0
            }
            :s.bytes.size()-s.position);
            if(count)std::memcpy(data,s.bytes.data()+s.position,count);
            s.position+=count;
            return count;
        }
        void WriteRaw(const std::uint8_t* data,std::size_t length)override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            if(!length)return;
            if(s.position>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())||length>static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())-s.position)throw IOException("Memory stream exceeds the array size limit.");
            if(s.position==s.bytes.size())s.bytes.insert(s.bytes.end(),data,data+length);
            else {
                if(s.position+length>s.bytes.size())s.bytes.resize(s.position+length);
                std::memcpy(s.bytes.data()+s.position,data,length);
            }
            s.position+=length;
        }
        std::int64_t Seek(std::int64_t offset,SeekOrigin origin=SeekOrigin::Begin)override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            std::int64_t base=0;
            if(origin==SeekOrigin::Current)base=native::cast<std::int64_t>(s.position);
            else if(origin==SeekOrigin::End)base=native::cast<std::int64_t>(s.bytes.size());
            else if(origin!=SeekOrigin::Begin)throw ArgumentException("Invalid SeekOrigin.");
            auto pos=native::add<std::int64_t>(base,offset);
            s.position=io_detail::size(pos);
            return pos;
        }
        void SetLength(std::int64_t length)override {
            if(length>std::numeric_limits<std::int32_t>::max())throw ArgumentOutOfRangeException("Memory stream is too large.");
            auto n=io_detail::size(length);
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
            s.bytes.resize(n);
            if(s.position>n)s.position=n;
        }
        void Flush()override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            open(s);
        }
        void Close()override {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            s.closed=true;
        }
        native::Array<std::uint8_t> ToArray()const {
            auto& s=state();
            std::lock_guard lock(s.mutex);
            return native::Array<std::uint8_t>::FromRange(s.bytes.begin(),s.bytes.end());
        }
    };

    class File {
        public:
        File()=delete;
        static FileStream Create(const native::String& path) {
            return FileStream(path,FileMode::Create,FileAccess::ReadWrite);
        }
        static FileStream Open(const native::String& path,FileMode mode=FileMode::Open,FileAccess access=FileAccess::ReadWrite) {
            return FileStream(path,mode,access);
        }
        static FileStream OpenRead(const native::String& path) {
            return FileStream(path,FileMode::Open,FileAccess::Read);
        }
        static FileStream OpenWrite(const native::String& path) {
            return FileStream(path,FileMode::OpenOrCreate,FileAccess::Write);
        }
        static bool Exists(const native::String& path) {
            if(path.empty())return false;
            std::error_code ec;
            return std::filesystem::is_regular_file(io_detail::path(path),ec)&&!ec;
        }
        static void Delete(const native::String& name) {
            auto path=io_detail::path(name);
            std::error_code ec;
            if(std::filesystem::is_directory(path,ec))throw IOException("Use Directory.Delete for a directory.");
            ec.clear();
            std::filesystem::remove(path,ec);
            if(ec)io_detail::error(path,ec,"Delete");
        }
        static native::String ReadAllText(const native::String& name) {
            auto stream=OpenRead(name);
            std::string result;
            std::uint8_t block[8192];
            for(;;) {
                auto n=stream.ReadRaw(block,sizeof(block));
                if(!n)break;
                result.append(reinterpret_cast<const char*>(block),n);
            }
            if(result.starts_with("\xEF\xBB\xBF"))result.erase(0,3);
            (void)unicode::length(result);
            return result;
        }
        static native::Array<std::uint8_t> ReadAllBytes(const native::String& name) {
            return OpenRead(name).ReadAllBytes();
        }
        static void WriteAllText(const native::String& name,const native::String& text) {
            auto stream=Create(name);
            stream.WriteRaw(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
            stream.Flush();
        }
        static void AppendAllText(const native::String& name,const native::String& text) {
            FileStream stream(name,FileMode::Append,FileAccess::Write);
            stream.WriteRaw(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
            stream.Flush();
        }
        static void WriteAllBytes(const native::String& name,const native::Array<std::uint8_t>& data) {
            auto stream=Create(name);
            stream.Write(data);
            stream.Flush();
        }
        static native::Array<native::String> ReadAllLines(const native::String& name) {
            auto text=ReadAllText(name);
            std::vector<native::String> lines;
            std::size_t start=0;
            while(start<text.size()) {
                auto end=text.find_first_of("\r\n",start);
                if(end==native::String::npos) {
                    lines.emplace_back(text.substr(start));
                    break;
                }
                lines.emplace_back(text.substr(start,end-start));
                start=end+1;
                if(text[end]=='\r'&&start<text.size()&&text[start]=='\n')++start;
            }
            auto result=native::Array<native::String>::Create(lines.size());
            for(std::size_t i=0;i<lines.size();++i)result.At(i)=std::move(lines[i]);
            return result;
        }
        static void WriteAllLines(const native::String& name,const native::Array<native::String>& lines) {
            auto s=Create(name);
            for(std::int32_t i=0;i<lines.Count();++i) {
                const auto& line=lines.At(i);
                s.WriteRaw(reinterpret_cast<const std::uint8_t*>(line.data()),line.size());
                s.WriteByte('\n');
            }
            s.Flush();
        }
        static void AppendAllLines(const native::String& name,const native::Array<native::String>& lines) {
            FileStream s(name,FileMode::Append,FileAccess::Write);
            for(std::int32_t i=0;i<lines.Count();++i) {
                const auto& line=lines.At(i);
                s.WriteRaw(reinterpret_cast<const std::uint8_t*>(line.data()),line.size());
                s.WriteByte('\n');
            }
            s.Flush();
        }
        static void Copy(const native::String& from,const native::String& to,bool overwrite=false) {
            std::error_code ec;
            auto source=io_detail::path(from);
            std::filesystem::copy_file(source,io_detail::path(to),overwrite?std::filesystem::copy_options::overwrite_existing:std::filesystem::copy_options::none,ec);
            if(ec)io_detail::error(source,ec,"Copy");
        }
        static void Move(const native::String& from,const native::String& to,bool overwrite=false) {
            auto src=io_detail::path(from),dst=io_detail::path(to);
            std::error_code ec;
            if(!std::filesystem::is_regular_file(src,ec)) {
                if(ec)io_detail::error(src,ec,"Move");
                throw FileNotFoundException("The source file does not exist or is not a regular file.");
            }
#ifdef _WIN32
            if(!MoveFileExW(src.c_str(),dst.c_str(),overwrite?MOVEFILE_REPLACE_EXISTING:0))io_detail::error(src,std::error_code(static_cast<int>(GetLastError()),std::system_category()),"Move");
#else
            if(overwrite) {
                if(::rename(src.c_str(),dst.c_str())!=0)io_detail::error(src,std::error_code(errno,std::generic_category()),"Move");
            }
            else {
                // link() atomically rejects an existing destination. Cross-device
                // moves fail without deleting either original file.
                if(::link(src.c_str(),dst.c_str())!=0)io_detail::error(src,std::error_code(errno,std::generic_category()),"Move");
                if(::unlink(src.c_str())!=0) {
                    auto saved=errno;
                    ::unlink(dst.c_str());
                    io_detail::error(src,std::error_code(saved,std::generic_category()),"Remove moved source");
                }
            }
#endif
        }
        static std::int64_t GetLength(const native::String& name) {
            std::error_code ec;
            auto path=io_detail::path(name);
            auto n=std::filesystem::file_size(path,ec);
            if(ec)io_detail::error(path,ec,"Read size");
            return native::cast<std::int64_t>(n);
        }
    };

    class Directory {
        template<class Predicate> static native::Array<native::String> entries(const native::String& name,bool recursive,Predicate matches) {
            const auto root=io_detail::path(name);
            std::error_code ec;
            if(!std::filesystem::is_directory(root,ec)) {
                if(ec)io_detail::error(root,ec,"List directory");
                throw DirectoryNotFoundException("Directory not found: "+name);
            }
            std::vector<native::String> paths;
            auto visit=[&](const auto& item) {
                std::error_code error;
                if(matches(item,error))paths.emplace_back(path_to_utf8(item.path()));
                if(error)io_detail::error(item.path(),error,"Inspect directory entry");
            };
            if(recursive) {
                for(std::filesystem::recursive_directory_iterator it(root,ec),end;it!=end&&!ec;it.increment(ec))visit(*it);
            }else {
                for(std::filesystem::directory_iterator it(root,ec),end;it!=end&&!ec;it.increment(ec))visit(*it);
            }
            if(ec)io_detail::error(root,ec,"List directory");
            std::sort(paths.begin(),paths.end());
            auto result=native::Array<native::String>::Create(paths.size());
            for(std::size_t i=0;i<paths.size();++i)result.At(i)=std::move(paths[i]);
            return result;
        }
        public:
        Directory()=delete;
        static void Create(const native::String& name) {
            std::error_code ec;
            auto path=io_detail::path(name);
            std::filesystem::create_directories(path,ec);
            if(ec)io_detail::error(path,ec,"Create directory");
        }
        static bool Exists(const native::String& name) {
            if(name.empty())return false;
            std::error_code ec;
            return std::filesystem::is_directory(io_detail::path(name),ec)&&!ec;
        }
        static void Delete(const native::String& name,bool recursive=true) {
            auto path=io_detail::path(name);
            std::error_code ec;
            if(!std::filesystem::is_directory(path,ec)) {
                if(ec)io_detail::error(path,ec,"Inspect directory");
                throw DirectoryNotFoundException("Directory not found: "+name);
            }
            if(recursive)std::filesystem::remove_all(path,ec);
            else std::filesystem::remove(path,ec);
            if(ec)io_detail::error(path,ec,"Delete directory");
        }
        static void Move(const native::String& from,const native::String& to) {
            if(!Exists(from))throw DirectoryNotFoundException("Source directory not found.");
            std::error_code ec;
            if(std::filesystem::exists(io_detail::path(to),ec))throw IOException("Destination already exists.");
            std::filesystem::rename(io_detail::path(from),io_detail::path(to),ec);
            if(ec)io_detail::error(io_detail::path(from),ec,"Move directory");
        }
        static native::Array<native::String> GetFiles(const native::String& name,bool recursive=false) {
            return entries(name,recursive,[](const auto& e,std::error_code& ec) {
                return e.is_regular_file(ec);
            });
        }
        static native::Array<native::String> GetDirectories(const native::String& name,bool recursive=false) {
            return entries(name,recursive,[](const auto& e,std::error_code& ec) {
                return e.is_directory(ec);
            });
        }
        static native::Array<native::String> GetFileSystemEntries(const native::String& name,bool recursive=false) {
            return entries(name,recursive,[](const auto&,std::error_code&) {
                return true;
            });
        }
        static native::String GetCurrentDirectory() {
            return path_to_utf8(std::filesystem::current_path());
        }
        static void SetCurrentDirectory(const native::String& name) {
            std::error_code ec;
            auto path=io_detail::path(name);
            std::filesystem::current_path(path,ec);
            if(ec)io_detail::error(path,ec,"Change directory");
        }
        static native::String GetParent(const native::String& name) {
            return path_to_utf8(std::filesystem::absolute(io_detail::path(name)).parent_path());
        }
    };
    class Path {
        public:
        Path()=delete;
        static native::String Combine(const native::String& left,const native::String& right) {
            return path_to_utf8(path_from_utf8(left)/path_from_utf8(right));
        }
        static native::String GetFullPath(const native::String& name) {
            return path_to_utf8(std::filesystem::absolute(io_detail::path(name)).lexically_normal());
        }
        static native::String GetFileName(const native::String& name) {
            return path_to_utf8(path_from_utf8(name).filename());
        }
        static native::String GetFileNameWithoutExtension(const native::String& name) {
            return path_to_utf8(path_from_utf8(name).stem());
        }
        static native::String GetExtension(const native::String& name) {
            return path_to_utf8(path_from_utf8(name).extension());
        }
        static native::String GetDirectoryName(const native::String& name) {
            return path_to_utf8(path_from_utf8(name).parent_path());
        }
        static native::String ChangeExtension(const native::String& name,const native::String& extension) {
            auto p=path_from_utf8(name);
            p.replace_extension(path_from_utf8(extension));
            return path_to_utf8(p);
        }
        static bool IsPathRooted(const native::String& name) {
            return path_from_utf8(name).is_absolute();
        }
        static native::String GetTempPath() {
            return path_to_utf8(std::filesystem::temp_directory_path());
        }
    };

    // Reader/writer copies share their stream handle. The final handle closes it by
    // RAII; Close closes all aliases. leaveOpen controls explicit Close ownership.
    class StreamReader {
        std::shared_ptr<Stream> stream_;
        bool leaveOpen_=false;
        int pending_=-1;
        bool closed_=false;
        void check()const {
            if(closed_||!stream_)throw ObjectDisposedException("The reader is closed.");
        }
        int byte() {
            check();
            if(pending_>=0)return std::exchange(pending_,-1);
            return stream_->ReadByte();
        }
        public:
        explicit StreamReader(const native::String& path):stream_(std::make_shared<FileStream>(path,FileMode::Open,FileAccess::Read)) {
        }
        explicit StreamReader(Stream& stream,bool leaveOpen=false):stream_(stream.CloneHandle()),leaveOpen_(leaveOpen) {
        }
        bool EndOfStream() {
            check();
            if(pending_<0)pending_=stream_->ReadByte();
            return pending_<0;
        }
        native::String ReadLine() {
            check();
            std::string s;
            for(;;) {
                int c=byte();
                if(c<0||c=='\n')break;
                if(c=='\r') {
                    int next=byte();
                    if(next!='\n')pending_=next;
                    break;
                }
                s+=static_cast<char>(c);
            }
            return s;
        }
        native::String ReadToEnd() {
            std::string s;
            if(pending_>=0)s+=static_cast<char>(std::exchange(pending_,-1));
            std::uint8_t buffer[8192];
            check();
            for(;;) {
                auto n=stream_->ReadRaw(buffer,sizeof(buffer));
                if(!n)break;
                s.append(reinterpret_cast<const char*>(buffer),n);
            }
            return s;
        }
        void Close() {
            if(closed_)return;
            if(!leaveOpen_&&stream_)stream_->Close();
            stream_.reset();
            closed_=true;
        }
        void Dispose() {
            Close();
        }
    };
    class StreamWriter {
        std::shared_ptr<Stream> stream_;
        bool leaveOpen_=false;
        bool closed_=false;
        void check()const {
            if(closed_||!stream_)throw ObjectDisposedException("The writer is closed.");
        }
        public:
        explicit StreamWriter(const native::String& path,bool append=false):stream_(std::make_shared<FileStream>(path,append?FileMode::Append:FileMode::Create,FileAccess::Write)) {
        }
        explicit StreamWriter(Stream& stream,bool leaveOpen=false):stream_(stream.CloneHandle()),leaveOpen_(leaveOpen) {
        }
        template<class T> void Write(const T& value) {
            check();
            auto text=native::text(value);
            stream_->WriteRaw(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
        }
        void WriteLine() {
            check();
            stream_->WriteByte('\n');
        }
        template<class T> void WriteLine(const T& value) {
            Write(value);
            WriteLine();
        }
        void Flush() {
            check();
            stream_->Flush();
        }
        void Close() {
            if(closed_)return;
            Flush();
            if(!leaveOpen_)stream_->Close();
            stream_.reset();
            closed_=true;
        }
        void Dispose() {
            Close();
        }
    };
    class BinaryWriter {
        std::shared_ptr<Stream> stream_;
        bool leaveOpen_=false;
        void check()const {
            if(!stream_)throw ObjectDisposedException("The binary writer is closed.");
        }
        public:
        explicit BinaryWriter(Stream& stream,bool leaveOpen=false):stream_(stream.CloneHandle()),leaveOpen_(leaveOpen) {
        }
        template<class T> void Write(T value) {
            check();
            if constexpr(std::is_same_v<T,bool>) {
                stream_->WriteByte(value?1:0);
            }else if constexpr(std::is_same_v<T,char32_t>) {
                Write(static_cast<std::uint32_t>(value));
            }else if constexpr(std::is_arithmetic_v<T>||std::is_enum_v<T>) {
                auto bytes=std::bit_cast<std::array<std::uint8_t,sizeof(T)>>(value);
                if constexpr(std::endian::native==std::endian::big)std::reverse(bytes.begin(),bytes.end());
                stream_->WriteRaw(bytes.data(),bytes.size());
            }else if constexpr(std::is_same_v<T,native::String>) {
                Write(native::cast<std::uint32_t>(value.size()));
                stream_->WriteRaw(reinterpret_cast<const std::uint8_t*>(value.data()),value.size());
            }else if constexpr(std::is_same_v<T,native::Array<std::uint8_t>>) {
                stream_->Write(value);
            }else static_assert(std::is_arithmetic_v<T>,"Unsupported binary value type.");
        }
        void Flush() {
            check();
            stream_->Flush();
        }
        void Close() {
            if(!stream_)return;
            Flush();
            if(!leaveOpen_)stream_->Close();
            stream_.reset();
        }
        void Dispose() {
            Close();
        }
    };
    class BinaryReader {
        std::shared_ptr<Stream> stream_;
        bool leaveOpen_=false;
        void exact(std::uint8_t* bytes,std::size_t n) {
            if(!stream_)throw ObjectDisposedException("The binary reader is closed.");
            std::size_t i=0;
            while(i<n) {
                auto count=stream_->ReadRaw(bytes+i,n-i);
                if(!count)throw EndOfStreamException("Not enough bytes remain for the requested value.");
                i+=count;
            }
        }
        public:
        explicit BinaryReader(Stream& stream,bool leaveOpen=false):stream_(stream.CloneHandle()),leaveOpen_(leaveOpen) {
        }
        template<class T> T Read() {
            if constexpr(std::is_same_v<T,bool>) {
                auto value=Read<std::uint8_t>();
                if(value>1)throw FormatException("Invalid binary bool byte.");
                return value!=0;
            }else if constexpr(std::is_same_v<T,char32_t>) {
                auto c=Read<std::uint32_t>();
                if(!unicode::scalar(c))throw FormatException("Invalid binary Unicode scalar.");
                return static_cast<char32_t>(c);
            }else if constexpr(std::is_arithmetic_v<T>||std::is_enum_v<T>) {
                std::array<std::uint8_t,sizeof(T)> bytes {
                };
                exact(bytes.data(),bytes.size());
                if constexpr(std::endian::native==std::endian::big)std::reverse(bytes.begin(),bytes.end());
                return std::bit_cast<T>(bytes);
            }else if constexpr(std::is_same_v<T,native::String>) {
                auto length=Read<std::uint32_t>();
                if(length>64u*1024u*1024u)throw IOException("Binary string exceeds the 64 MiB safety limit.");
                std::string s(length,'\0');
                exact(reinterpret_cast<std::uint8_t*>(s.data()),s.size());
                (void)unicode::length(s);
                return s;
            }else static_assert(std::is_arithmetic_v<T>,"Unsupported binary value type.");
        }
        std::int32_t ReadInt() {
            return Read<std::int32_t>();
        }
        std::int8_t ReadInt8() {
            return Read<std::int8_t>();
        }
        std::int16_t ReadInt16() {
            return Read<std::int16_t>();
        }
        std::int64_t ReadInt64() {
            return Read<std::int64_t>();
        }
        std::uint8_t ReadUInt8() {
            return Read<std::uint8_t>();
        }
        std::uint16_t ReadUInt16() {
            return Read<std::uint16_t>();
        }
        std::uint32_t ReadUInt32() {
            return Read<std::uint32_t>();
        }
        std::uint64_t ReadUInt64() {
            return Read<std::uint64_t>();
        }
        bool ReadBool() {
            return Read<bool>();
        }
        char32_t ReadChar() {
            return Read<char32_t>();
        }
        float ReadFloat() {
            return Read<float>();
        }
        double ReadDouble() {
            return Read<double>();
        }
        native::String ReadString() {
            return Read<native::String>();
        }
        native::Array<std::uint8_t> ReadBytes(std::int32_t count) {
            if(count<0)throw ArgumentOutOfRangeException("Byte count must be non-negative.");
            auto result=native::Array<std::uint8_t>::Create(count);
            for(std::int32_t i=0;i<count;++i)result.At(i)=Read<std::uint8_t>();
            return result;
        }
        void Close() {
            if(!stream_)return;
            if(!leaveOpen_)stream_->Close();
            stream_.reset();
        }
        void Dispose() {
            Close();
        }
    };
}
