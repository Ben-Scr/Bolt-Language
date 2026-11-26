#pragma once
#include "ErasedValue.hpp"
#include "../library/Exceptions.hpp"
#include <atomic>
#include <new>
namespace bolt::platform {
struct AtomicOps {
    static std::uint64_t Add(std::uint64_t& word,std::uint64_t amount) noexcept {
#ifdef _MSC_VER
        return std::atomic_ref(word).fetch_add(amount);
#else
        return __atomic_fetch_add(&word,amount,__ATOMIC_SEQ_CST);
#endif
    }
    static std::uint64_t Cas(std::uint64_t& word,std::uint64_t expected,std::uint64_t desired) noexcept {
#ifdef _MSC_VER
        std::atomic_ref(word).compare_exchange_strong(expected,desired);
#else
        __atomic_compare_exchange_n(&word,&expected,desired,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
#endif
        return expected;
    }
};
struct ExecutionMemory {
    // C++ exceptions travel in the target exception ABI. Only the Bolt join
    // registry needs storage in this execution context.
    alignas(8) static inline std::uint8_t registry[8+sizeof(void*)]{};
    static inline thread_local std::uint8_t* active=registry;
    static std::uint8_t* Context() noexcept { return active; }
    static std::uint8_t* Registry(std::uint8_t* ctx) noexcept { return ctx; }
    static std::uint8_t* Activate(std::uint8_t* ctx) noexcept { auto* old=active;active=ctx;return old; }
    static void Dispose(std::uint8_t*) noexcept {}
    static std::uint64_t Size(std::uint8_t* t){return Ops(t).size;}
    static void Copy(std::uint8_t*,std::uint8_t* t,std::uint8_t* to,std::uint8_t* from){Ops(t).copy(to,from);}
    static void Drop(std::uint8_t* t,std::uint8_t* value){Ops(t).drop(value);}
    [[noreturn]] static void OutOfMemory(std::uint8_t*){throw library::OutOfMemoryException("Cannot allocate a Bolt value.");}
    template<class T> static std::uint8_t* Descriptor(){return native::erased_descriptor<T>();}
    static std::uint8_t* ExceptionDescriptor(library::Exception& e){return e.BoltDescriptor();}
    static std::uint8_t* ExceptionAddress(library::Exception& e){return static_cast<std::uint8_t*>(dynamic_cast<void*>(&e));}
    static library::Exception* ExceptionBase(std::uint8_t* type,std::uint8_t* value){return Ops(type).as_exception(value);}
private:
    static const native::ErasedValue& Ops(std::uint8_t* type){return *reinterpret_cast<const native::ErasedValue*>(type);}
};
}
