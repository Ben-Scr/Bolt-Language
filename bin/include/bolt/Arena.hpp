#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace bolt::memory {
class Arena;
#if defined(BOLT_IOS_AOT_EXTERNAL_REGISTRY)
// Linked iOS scripts share the runtime archive's registry and its Darwin TLS initializer.
extern thread_local std::vector<Arena*> arenas;
#else
inline thread_local std::vector<Arena*> arenas;
#endif
inline thread_local Arena* current_arena=nullptr;
struct Scope {
    Arena* previous;
    explicit Scope(Arena* owner) noexcept:previous(current_arena){current_arena=owner;}
    ~Scope(){current_arena=previous;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
};

// Control structures live outside the user backing budget. Payload, alignment
// padding and obsolete buffers occupy the bounded backing blocks until reset.
class Arena final:public std::pmr::memory_resource {
    struct Block {
        std::byte* data;
        std::size_t capacity,offset=0;
        explicit Block(std::size_t n):data(static_cast<std::byte*>(::operator new(n))),capacity(n){}
        Block(Block&& b) noexcept:data(std::exchange(b.data,nullptr)),capacity(b.capacity),offset(b.offset){}
        Block(const Block&)=delete;
        ~Block(){::operator delete(data);}
    };
    struct Root {void* address;std::function<void(void*)> cleanup;};
    std::vector<Block> blocks_;
    std::vector<Root> roots_;
    std::size_t used_=0,capacity_=0,peak_=0,limit_=0,initial_=0;
    bool grow_=false,disposed_=false,clearing_=false;
    std::thread::id thread_=std::this_thread::get_id();
    void check() const {
        if(thread_!=std::this_thread::get_id())throw std::runtime_error("arena belongs to another thread");
        if(disposed_)throw std::runtime_error("arena has been disposed");
        if(clearing_)throw std::runtime_error("arena cleanup cannot be reentered");
    }
    void cleanup() noexcept {
        clearing_=true;
        for(auto i=roots_.rbegin();i!=roots_.rend();++i){try{i->cleanup(i->address);}catch(...){}}
        roots_.clear();
        clearing_=false;
    }
    void* do_allocate(std::size_t bytes,std::size_t alignment) override {
        check();
        if(!alignment||(alignment&(alignment-1)))throw std::runtime_error("invalid arena alignment");
        bytes=std::max<std::size_t>(1,bytes);
        if(bytes>limit_||alignment-1>std::numeric_limits<std::size_t>::max()-bytes)
            throw std::runtime_error("arena capacity exhausted");
        auto attempt=[&](Block& b)->void* {
            void* p=b.data+b.offset;
            auto available=b.capacity-b.offset;
            if(!std::align(alignment,bytes,p,available))return nullptr;
            auto end=static_cast<std::byte*>(p)-b.data+bytes;
            used_+=static_cast<std::size_t>(end)-b.offset;
            b.offset=static_cast<std::size_t>(end);
            peak_=std::max(peak_,used_);
            return p;
        };
        for(auto& block:blocks_)if(auto* p=attempt(block))return p;
        if(!grow_||capacity_==limit_)throw std::runtime_error("arena capacity exhausted");
        const auto remaining=limit_-capacity_;
        if(bytes>remaining)throw std::runtime_error("arena capacity exhausted");
        auto desired=std::max(initial_,bytes+alignment-1);
        auto count=std::min(remaining,desired);
        Block candidate(count);
        void* aligned=candidate.data;auto available=count;
        if(!std::align(alignment,bytes,aligned,available))throw std::runtime_error("arena capacity exhausted");
        blocks_.push_back(std::move(candidate));capacity_+=count;
        if(auto* p=attempt(blocks_.back()))return p;
        throw std::runtime_error("arena capacity exhausted");
    }
    void do_deallocate(void*,std::size_t,std::size_t) override {}
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override{return this==&other;}
public:
    Arena(std::size_t initial,std::size_t maximum,bool grow=false):limit_(maximum),initial_(initial),grow_(grow) {
        if(!initial||!maximum||initial>maximum||(!grow&&initial!=maximum))throw std::runtime_error("invalid arena capacity");
        blocks_.emplace_back(initial);capacity_=initial;
        arenas.push_back(this);
    }
    Arena(const Arena&)=delete;
    Arena& operator=(const Arena&)=delete;
    Arena(Arena&&)=delete;
    ~Arena(){cleanup();blocks_.clear();auto i=std::find(arenas.begin(),arenas.end(),this);if(i!=arenas.end())arenas.erase(i);}
    void* allocate(std::size_t bytes,std::size_t alignment=alignof(std::max_align_t)){return do_allocate(bytes,alignment);}
    void add_root(void* p,std::function<void(void*)> cleanup_fn){check();roots_.push_back({p,std::move(cleanup_fn)});}
    void reset(){check();cleanup();for(auto& b:blocks_)b.offset=0;used_=0;}
    void dispose(){if(disposed_)return;check();cleanup();blocks_.clear();used_=capacity_=0;disposed_=true;}
    bool owns(const void* p) const noexcept {
        const auto a=reinterpret_cast<std::uintptr_t>(p);
        for(const auto& b:blocks_){const auto start=reinterpret_cast<std::uintptr_t>(b.data);if(a>=start&&a-start<b.capacity)return true;}
        return false;
    }
    std::size_t used()const noexcept{return used_;}
    std::size_t capacity()const noexcept{return capacity_;}
    std::size_t peak()const noexcept{return peak_;}
    std::size_t limit()const noexcept{return limit_;}
    bool disposed()const noexcept{return disposed_;}
};
inline Arena* owner_of(const void* p) noexcept {if(p)for(auto* a:arenas)if(a->owns(p))return a;return nullptr;}
inline std::pmr::memory_resource* resource(Arena* a) noexcept {return a?static_cast<std::pmr::memory_resource*>(a):std::pmr::new_delete_resource();}
inline void* allocate_bytes(Arena* a,std::size_t n,std::size_t alignment=alignof(std::max_align_t)) {
    return a ? a->allocate(n,alignment) : ::operator new(std::max<std::size_t>(n,1),std::align_val_t(std::max(alignment,alignof(std::max_align_t))));
}
inline void deallocate_bytes(void* p,std::size_t alignment=alignof(std::max_align_t)) noexcept {
    if(p&&!owner_of(p))::operator delete(p,std::align_val_t(std::max(alignment,alignof(std::max_align_t))));
}
template<class T,class... A> T* make(Arena* owner,A&&... args) {
    auto* p=allocate_bytes(owner,sizeof(T),alignof(T));
    Scope scope(owner);
    try{return ::new(p) T(std::forward<A>(args)...);}catch(...){deallocate_bytes(p,alignof(T));throw;}
}
template<class T> void destroy(T* p) noexcept {if(p){p->~T();deallocate_bytes(p,alignof(T));}}
template<class T> struct Deleter {void operator()(T* p)const noexcept{destroy(p);}};
template<class T> using Unique=std::unique_ptr<T,Deleter<T>>;
template<class T> struct Allocator {
    using value_type=T;
    using propagate_on_container_copy_assignment=std::false_type;
    using propagate_on_container_move_assignment=std::false_type;
    using propagate_on_container_swap=std::false_type;
    Arena* owner=current_arena;
    Allocator()noexcept=default;
    explicit Allocator(Arena* a)noexcept:owner(a){}
    template<class U> Allocator(const Allocator<U>& a)noexcept:owner(a.owner){}
    T* allocate(std::size_t n){if(n>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_array_new_length();return static_cast<T*>(allocate_bytes(owner,n*sizeof(T),alignof(T)));}
    void deallocate(T* p,std::size_t)noexcept{deallocate_bytes(p,alignof(T));}
    Allocator select_on_container_copy_construction()const noexcept{return Allocator(current_arena);}
    template<class U> bool operator==(const Allocator<U>& a)const noexcept{return owner==a.owner;}
};
} // namespace bolt::memory
