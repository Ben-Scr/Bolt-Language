#pragma once
// Included once from Fast.cpp on Apple Silicon. A hardened process can own a
// single MAP_JIT region. Keep that reservation across overlapping revisions
// and keep mutable Bolt globals in a different, non-executable mapping.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <sys/mman.h>
#include <system_error>
#include <vector>

namespace bolt::native_backend::apple_jit {
struct Block {
    std::uint8_t* code;
    std::uint8_t* data;
    std::size_t size;
    std::size_t data_size;
};

class Pool {
    static constexpr std::size_t capacity = 256u * 1024u * 1024u;
    std::size_t page_;
    std::uint8_t* code_ = nullptr;
    std::mutex mutex_;
    std::vector<std::uint8_t> occupied_;
public:
    explicit Pool(std::size_t page) : page_(page), occupied_(capacity / page) {
        void* code = mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
        if (code == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(),
                "macOS could not reserve JIT pages; a hardened host needs the Allow JIT entitlement");
        code_ = static_cast<std::uint8_t*>(code);
    }
    ~Pool() { munmap(code_, capacity); }

    Block acquire(std::size_t size, std::size_t data_size) {
        std::lock_guard lock(mutex_);
        const auto pages = size / page_;
        // A small replacement module generally needs one or two pages. Stop as
        // soon as they are found, even when most of the pool is still free.
        const auto first = std::search_n(occupied_.begin(), occupied_.end(), pages, 0);
        if (first == occupied_.end())
            throw std::runtime_error("Bolt's 256 MiB macOS JIT reservation is full; release unused modules");
        void* data = mmap(nullptr, data_size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (data == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "could not map Bolt JIT data");
        std::fill_n(first, pages, 1);
        const auto offset = static_cast<std::size_t>(first-occupied_.begin()) * page_;
        return {code_ + offset, static_cast<std::uint8_t*>(data), size, data_size};
    }
    void release(Block block) {
        std::lock_guard lock(mutex_);
        munmap(block.data, block.data_size);
        const auto first = static_cast<std::size_t>(block.code-code_) / page_;
        std::fill_n(occupied_.begin()+first, block.size/page_, 0);
    }
};
inline Pool& pool(std::size_t page) {
    static Pool value(page);
    return value;
}

// The allowlisted callback only copies a verified, relocated compiler image.
// Its public callback argument carries no destination, size or machine code.
struct PreparedWrite { Block block; const detail::Bytes* image; };
static thread_local const PreparedWrite* pending = nullptr;
static int copy_image(void* unused) {
    if (unused || !pending || pending->image->size() > pending->block.size) return EINVAL;
    if (!pending->image->empty())
        std::memcpy(pending->block.code, pending->image->data(), pending->image->size());
    return 0;
}
PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP(copy_image);

inline void publish(Block block, const detail::Bytes& image) {
    if (pending) throw std::logic_error("nested JIT publication on one thread");
    const PreparedWrite write{block,&image};
    pending = &write;
    int result;
    if (__builtin_available(macOS 11.4, *)) {
        result = pthread_jit_write_with_callback_np(copy_image, nullptr);
    } else {
        // Apple Silicon's first releases predate the callback form.
        pthread_jit_write_protect_np(0);
        result = copy_image(nullptr);
        pthread_jit_write_protect_np(1);
    }
    pending = nullptr;
    if (result) throw std::system_error(result, std::generic_category(), "macOS JIT publication failed");
    sys_icache_invalidate(block.code, image.size());
}
} // namespace bolt::native_backend::apple_jit
