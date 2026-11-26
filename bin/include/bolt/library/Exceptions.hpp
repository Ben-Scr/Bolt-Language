#pragma once
#include "../ArenaCpp.hpp"
#include "../Diagnostics.hpp"
#include "../detail/ErasedValue.hpp"
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bolt::library {
    class Exception : public std::runtime_error {
        std::string file_;
        int line_=0, column_=0;
        std::vector<std::string> frames_;
        public:
        explicit Exception(std::string_view message= {
        }) : std::runtime_error(std::string(message)) {
        }
        virtual std::uint8_t* BoltDescriptor() const { return native::erased_descriptor<Exception>(); }
        [[noreturn]] virtual void BoltThrowCopy() const { throw *this; }
        virtual const char* TypeName() const noexcept {
            return "Exception";
        }
        native::String Message() const {
            return what();
        }
        native::String SourceFile() const {
            return file_;
        }
        std::int32_t SourceLine() const {
            return line_;
        }
        std::int32_t SourceColumn() const {
            return column_;
        }
        void SetSource(const Location& where) {
            if(file_.empty()) {
                file_=where.file;
                line_=where.line;
                column_=where.column;
            }
        }
        void AddFrame(const Location& where,std::string method) {
            SetSource(where);
            frames_.push_back(std::move(method)+" at "+where.file+":"+std::to_string(where.line));
        }
        native::String StackTrace() const {
            std::string s;
            for(const auto& f:frames_)s+="  at "+f+"\n";
            return s;
        }
        virtual native::String ToString() const {
            std::string s=std::string(TypeName())+": "+what();
            if(!file_.empty())s+="\n"+file_+":"+std::to_string(line_)+":"+std::to_string(column_);
            if(!frames_.empty())s+="\n"+static_cast<std::string>(StackTrace());
            return s;
        }
        std::string BoltFormat() const {
            return static_cast<std::string>(ToString());
        }
        bool operator==(const Exception& other) const {
            return std::string(TypeName())==other.TypeName()&&Message()==other.Message();
        }
        std::size_t BoltHash() const {
            return std::hash<std::string> {
            }
            (std::string(TypeName())+what());
        }
    };
#define BOLT_EXCEPTION(Name, Base) class Name : public Base { public: using Base::Base; std::uint8_t* BoltDescriptor() const override { return native::erased_descriptor<Name>(); } void BoltThrowCopy() const override { throw *this; } const char* TypeName() const noexcept override {return #Name;} };
    BOLT_EXCEPTION(SystemException, Exception)
    BOLT_EXCEPTION(ArgumentException, SystemException)
    BOLT_EXCEPTION(ArgumentNullException, ArgumentException)
    BOLT_EXCEPTION(ArgumentOutOfRangeException, ArgumentException)
    BOLT_EXCEPTION(InvalidOperationException, SystemException)
    BOLT_EXCEPTION(NotSupportedException, SystemException)
    BOLT_EXCEPTION(NotImplementedException, SystemException)
    BOLT_EXCEPTION(ObjectDisposedException, InvalidOperationException)
    BOLT_EXCEPTION(NullReferenceException, SystemException)
    BOLT_EXCEPTION(IndexOutOfRangeException, SystemException)
    BOLT_EXCEPTION(KeyNotFoundException, SystemException)
    BOLT_EXCEPTION(InvalidCastException, SystemException)
    BOLT_EXCEPTION(FormatException, SystemException)
    BOLT_EXCEPTION(ArithmeticException, SystemException)
    BOLT_EXCEPTION(OverflowException, ArithmeticException)
    BOLT_EXCEPTION(DivideByZeroException, ArithmeticException)
    BOLT_EXCEPTION(IOException, SystemException)
    BOLT_EXCEPTION(SocketException, IOException)
    BOLT_EXCEPTION(SocketWouldBlockException, SocketException)
    BOLT_EXCEPTION(FileNotFoundException, IOException)
    BOLT_EXCEPTION(DirectoryNotFoundException, IOException)
    BOLT_EXCEPTION(EndOfStreamException, IOException)
    BOLT_EXCEPTION(UnauthorizedAccessException, SystemException)
    BOLT_EXCEPTION(TimeoutException, SystemException)
    BOLT_EXCEPTION(OperationCanceledException, SystemException)
    BOLT_EXCEPTION(JsonException, FormatException)
    BOLT_EXCEPTION(ReflectionException, SystemException)
    BOLT_EXCEPTION(OutOfMemoryException, SystemException)
    BOLT_EXCEPTION(AssertionException, SystemException)
#undef BOLT_EXCEPTION

    // This guard is used inside an outer catch boundary generated for finally.
    // Thus a throwing finally can replace the pending exception without throwing
    // from a destructor during active stack unwinding.
    template<class F> class Finally {
        F body_;
        bool active_=true;
        public:
        explicit Finally(F f):body_(std::move(f)) {
        }
        Finally(const Finally&)=delete;
        Finally& operator=(const Finally&)=delete;
        void Run() {
            if(std::exchange(active_,false))body_();
        }
        ~Finally() noexcept(false) {
            Run();
        }
    };
    template<class F> Finally<F> finally(F f) {
        return Finally<F>(std::move(f));
    }

    [[noreturn]] inline void translate_error(const std::exception& error,const Location& location) {
        auto message=std::string(error.what());
        auto raise=[&]<class E>() -> void {
            E e {
                native::String(message)
            };
            e.SetSource(location);
            throw e;
        };
        if(dynamic_cast<const std::bad_alloc*>(&error))raise.template operator()<OutOfMemoryException>();
        if(message.find("division by zero")!=message.npos||message.find("remainder by zero")!=message.npos)raise.template operator()<DivideByZeroException>();
        if(message.find("overflow")!=message.npos||message.find("underflow")!=message.npos||message.find("conversion out of range")!=message.npos)raise.template operator()<OverflowException>();
        if(message.find("null pointer")!=message.npos)raise.template operator()<NullReferenceException>();
        if(message.find("index")!=message.npos&&message.find("range")!=message.npos)raise.template operator()<IndexOutOfRangeException>();
        if(message.find("key not found")!=message.npos)raise.template operator()<KeyNotFoundException>();
        if(dynamic_cast<const std::invalid_argument*>(&error))raise.template operator()<ArgumentException>();
        if(dynamic_cast<const std::out_of_range*>(&error))raise.template operator()<ArgumentOutOfRangeException>();
        raise.template operator()<SystemException>();
        std::terminate();
    }
    // Called from a generated catch handler to attach the innermost Bolt source
    // location and preserve an existing Bolt exception's dynamic type.
    [[noreturn]] inline void rethrow_checked(const NativeSourceLocation& location) {
        try { throw; }
        catch(Exception& error) {
            error.SetSource(location.location());
            throw;
        }
        catch(const Error& error) {
            SystemException converted {
                native::String(error.diagnostic.message)
            };
            converted.SetSource(error.location);
            throw converted;
        }
        catch(const std::exception& error) {
            translate_error(error,location.location());
        }
    }

    template<class F> decltype(auto) checked(const NativeSourceLocation& location, F&& f) {
        try { return std::forward<F>(f)(); }
        catch (const std::exception&) { rethrow_checked(location); }
    }
}
