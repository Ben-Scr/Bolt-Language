#pragma once
// Raw allocation and arena primitives. Public policy lives in System/Memory.bolt.
#include "library/Exceptions.hpp"
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
namespace bolt::system {
inline std::size_t MemorySize(std::uint64_t n) {
    if(n>std::numeric_limits<std::size_t>::max())throw library::OverflowException("Size is not representable on this target.");
    return static_cast<std::size_t>(n);
}
inline std::size_t MemoryAlignment(std::uint64_t n) {
    const auto a=MemorySize(n);
    if(!a||(a&(a-1)))throw library::ArgumentException("Alignment must be a nonzero power of two.");
    return a;
}
class MemoryOps {
    static void Check(const void* a,const void* b,std::size_t n) {
        if(n&&(!a||!b))throw library::ArgumentNullException("A nonempty memory range needs a non-null pointer.");
        const auto maximum=std::numeric_limits<std::uintptr_t>::max();
        if(n&&(reinterpret_cast<std::uintptr_t>(a)>maximum-(n-1)||reinterpret_cast<std::uintptr_t>(b)>maximum-(n-1)))
            throw library::OverflowException("Memory address range overflow.");
    }
public:
    static std::uint64_t Address(const std::uint8_t* pointer) noexcept {
        return reinterpret_cast<std::uintptr_t>(pointer);
    }
    static void Copy(std::uint8_t* dest,const std::uint8_t* src,std::uint64_t bytes) {
        auto n=MemorySize(bytes);Check(dest,src,n);if(!n)return;
        const auto a=reinterpret_cast<std::uintptr_t>(dest),b=reinterpret_cast<std::uintptr_t>(src);
        if((a>=b?a-b:b-a)<n)throw library::ArgumentException("Memory.Copy ranges overlap; use Memory.Move.");
        std::memcpy(dest,src,n);
    }
    static void Move(std::uint8_t* dest,const std::uint8_t* src,std::uint64_t bytes) {auto n=MemorySize(bytes);Check(dest,src,n);if(n)std::memmove(dest,src,n);}
    static void Set(std::uint8_t* dest,std::uint8_t value,std::uint64_t bytes) {auto n=MemorySize(bytes);Check(dest,dest,n);if(n)std::memset(dest,value,n);}
    static std::int32_t Compare(const std::uint8_t* a,const std::uint8_t* b,std::uint64_t bytes) {auto n=MemorySize(bytes);Check(a,b,n);if(!n)return 0;auto v=std::memcmp(a,b,n);return v<0?-1:v>0?1:0;}
    static bool IsAligned(const std::uint8_t* p,std::uint64_t alignment) {return reinterpret_cast<std::uintptr_t>(p)%MemoryAlignment(alignment)==0;}
};
struct AllocationLedger {
    std::mutex mutex;std::uint64_t bytes=0,count=0,limit=std::numeric_limits<std::uint64_t>::max();
    std::int64_t remaining=-1;
};
struct AllocationState {
    std::shared_ptr<AllocationLedger> ledger;
    std::uint8_t* data=nullptr;std::size_t bytes=0,alignment=1;bool live=true;
    void Close() noexcept {
        if(!live)return;live=false;
        if(data){::operator delete(data,std::align_val_t(alignment));data=nullptr;
            std::lock_guard lock(ledger->mutex);ledger->bytes-=bytes;--ledger->count;}
    }
    ~AllocationState(){Close();}
};
class Allocator;
class MemoryBlock {
    std::shared_ptr<AllocationState> state_;
    explicit MemoryBlock(std::shared_ptr<AllocationState> s):state_(std::move(s)){}
    friend class Allocator;
    void Check()const {if(!state_||!state_->live)throw library::ObjectDisposedException("Memory block is not alive.");}
public:
    MemoryBlock()=default;
    // Copies intentionally share a checked allocation; Close invalidates all aliases.
    std::uint8_t* Data()const {Check();return state_->data;}
    std::uint64_t Length()const {Check();return state_->bytes;}
    std::uint64_t Alignment()const {Check();return state_->alignment;}
    bool IsAlive()const noexcept {return state_&&state_->live;}
    void Close() {if(state_)state_->Close();}
    std::uint8_t Read(std::uint64_t index)const {Check();if(index>=state_->bytes)throw library::IndexOutOfRangeException("Memory block index out of range.");return state_->data[index];}
    void Write(std::uint64_t index,std::uint8_t value) {Check();if(index>=state_->bytes)throw library::IndexOutOfRangeException("Memory block index out of range.");state_->data[index]=value;}
};
class Allocator {
    std::shared_ptr<AllocationLedger> state_=std::make_shared<AllocationLedger>();
public:
    explicit Allocator(std::int64_t limit=-1) {if(limit<-1)throw library::ArgumentOutOfRangeException("Allocator limit must be -1 or non-negative.");if(limit>=0)state_->limit=static_cast<std::uint64_t>(limit);}
    MemoryBlock Allocate(std::uint64_t bytes,std::uint64_t alignment=16) {
        const auto n=MemorySize(bytes),a=std::max(MemoryAlignment(alignment),alignof(std::max_align_t));
        auto block=std::make_shared<AllocationState>();block->ledger=state_;block->bytes=n;block->alignment=a;
        if(!n)return MemoryBlock(block);
        {
            std::lock_guard lock(state_->mutex);
            if(bytes>state_->limit-state_->bytes||state_->remaining==0)throw library::OutOfMemoryException("Allocator budget or injected allocation failure.");
            state_->bytes+=bytes;++state_->count;if(state_->remaining>0)--state_->remaining;
        }
        try {block->data=static_cast<std::uint8_t*>(::operator new(n,std::align_val_t(a)));}
        catch(...){std::lock_guard lock(state_->mutex);state_->bytes-=bytes;--state_->count;throw;}
        return MemoryBlock(block);
    }
    void Free(MemoryBlock& block) {
        if(!block.state_)return;
        if(block.state_->ledger!=state_)throw library::ArgumentException("Memory must be freed by its originating allocator.");
        block.Close();
    }
    MemoryBlock Reallocate(MemoryBlock& block,std::uint64_t bytes,std::uint64_t alignment=16) {
        if(block.state_&&block.state_->ledger!=state_)throw library::ArgumentException("Allocator mismatch.");
        auto next=Allocate(bytes,alignment); // old memory is preserved if this fails
        if(block.IsAlive()){auto n=std::min(block.Length(),bytes);if(n)MemoryOps::Copy(next.Data(),block.Data(),n);block.Close();}
        return next;
    }
    std::uint64_t LiveBytes()const {std::lock_guard lock(state_->mutex);return state_->bytes;}
    std::uint64_t LiveAllocations()const {std::lock_guard lock(state_->mutex);return state_->count;}
    void FailAfter(std::int64_t count) {if(count<-1)throw library::ArgumentOutOfRangeException("Failure countdown must be -1 or non-negative.");std::lock_guard lock(state_->mutex);state_->remaining=count;}
};
struct ArenaEpoch {std::uint64_t generation=1;bool live=true;std::thread::id owner=std::this_thread::get_id();};
class ArenaSlice {
    std::shared_ptr<ArenaEpoch> epoch_;std::uint64_t generation_=0;std::uint8_t* data_=nullptr;std::size_t count_=0;
    friend class ByteArena;
    void Check()const {if(!epoch_||epoch_->owner!=std::this_thread::get_id())throw library::InvalidOperationException("Arena handles are confined to their creating thread.");if(!IsAlive())throw library::ObjectDisposedException("Arena handle was invalidated by reset or disposal.");}
public:
    ArenaSlice()=default;
    bool IsAlive()const noexcept {return epoch_&&epoch_->owner==std::this_thread::get_id()&&epoch_->live&&generation_==epoch_->generation;}
    std::uint8_t* Data()const {Check();return data_;}
    std::uint64_t Length()const {Check();return count_;}
    std::uint8_t Read(std::uint64_t i)const {Check();if(i>=count_)throw library::IndexOutOfRangeException("Arena index out of range.");return data_[i];}
    void Write(std::uint64_t i,std::uint8_t value) {Check();if(i>=count_)throw library::IndexOutOfRangeException("Arena index out of range.");data_[i]=value;}
};
class ByteArena {
    struct State {
        struct Block {MemoryBlock memory;std::size_t used=0;};
        Allocator allocator;std::vector<Block> blocks;std::shared_ptr<ArenaEpoch> epoch=std::make_shared<ArenaEpoch>();
        std::size_t initial,limit,capacity=0,used=0;
        State(std::size_t a,std::size_t b):initial(a),limit(b){}
        ~State(){epoch->live=false;}
        void Check(){if(epoch->owner!=std::this_thread::get_id())throw library::InvalidOperationException("Arena used by another thread.");if(!epoch->live)throw library::ObjectDisposedException("Arena is disposed.");}
    };
    std::shared_ptr<State> state_;
public:
    ByteArena(std::uint64_t initial=4096,std::uint64_t maximum=1048576) {
        if(!initial||initial>maximum)throw library::ArgumentOutOfRangeException("Invalid arena capacities.");
        state_=std::make_shared<State>(MemorySize(initial),MemorySize(maximum));
    }
    ArenaSlice Allocate(std::uint64_t bytes,std::uint64_t alignment=16) {
        auto& s=*state_;s.Check();auto n=MemorySize(bytes),a=MemoryAlignment(alignment);
        if(n>s.limit||a-1>std::numeric_limits<std::size_t>::max()-n)throw library::OutOfMemoryException("Arena capacity exhausted.");
        auto attempt=[&](State::Block& b)->std::uint8_t*{
            void* ptr=b.memory.Data()+b.used;auto space=static_cast<std::size_t>(b.memory.Length())-b.used;
            if(!std::align(a,n? n:1,ptr,space))return nullptr;
            auto end=static_cast<std::uint8_t*>(ptr)-b.memory.Data()+n;s.used+=end-b.used;b.used=end;return static_cast<std::uint8_t*>(ptr);
        };
        std::uint8_t* ptr=nullptr;
        for(auto& b:s.blocks)if((ptr=attempt(b)))break;
        if(!ptr){auto available=s.limit-s.capacity;auto size=std::min(available,std::max(s.initial,std::max<std::size_t>(1,n)+a-1));
            if(size<n||(!n&&!size))throw library::OutOfMemoryException("Arena capacity exhausted.");
            auto block=s.allocator.Allocate(size,a);s.blocks.push_back({std::move(block),0});s.capacity+=size;ptr=attempt(s.blocks.back());
            if(!ptr)throw library::OutOfMemoryException("Arena alignment exceeds remaining capacity.");
        }
        ArenaSlice result;result.epoch_=s.epoch;result.generation_=s.epoch->generation;result.data_=ptr;result.count_=n;return result;
    }
    void Reset(){auto& s=*state_;s.Check();if(s.epoch->generation==std::numeric_limits<std::uint64_t>::max())throw library::OverflowException("Arena generation exhausted.");++s.epoch->generation;for(auto& b:s.blocks)b.used=0;s.used=0;}
    void Close(){auto& s=*state_;if(!s.epoch->live)return;s.Check();s.epoch->live=false;s.blocks.clear();s.used=s.capacity=0;}
    std::uint64_t Used()const {state_->Check();return state_->used;}
    std::uint64_t Capacity()const {state_->Check();return state_->capacity;}
};
}
