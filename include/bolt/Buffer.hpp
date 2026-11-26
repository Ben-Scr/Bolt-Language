#pragma once
#include "Std.hpp"
#include <memory>
#include <utility>

namespace bolt::memory {
namespace detail {
// Explicit Reserve has exact-size semantics. Only automatic growth uses this rule.
constexpr std::int32_t buffer_growth_capacity(std::size_t capacity, std::size_t required) {
    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
    if (required > limit) throw std::runtime_error("buffer size overflow");
    const auto doubled = capacity > limit / 2 ? limit : 2 * capacity;
    return static_cast<std::int32_t>(std::max({std::size_t{4}, required, doubled}));
}
} // namespace detail
// Typed storage primitive. No public collection lookup/growth policy lives here.
// Only the first size_ elements are alive. Reserving capacity never constructs T.
// Allocation follows the destination arena, including copies of containing values.
template<class T> class Buffer {
    using Allocator=bolt::native::DestinationAllocator<T>;
    Allocator allocator_{owner_of(this)};
    T* data_=nullptr;
    std::size_t size_=0,capacity_=0;
    void changed() const noexcept { bolt::native::detail::RangeObserver::changed(this); }
    void ensure_capacity(std::size_t required) {
        if (required <= capacity_) return;
        Reserve(detail::buffer_growth_capacity(capacity_, required));
    }
    void release() noexcept {Clear();if(data_)allocator_.deallocate(data_,capacity_);data_=nullptr;capacity_=0;}
    void swap_storage(Buffer& other)noexcept{changed();other.changed();std::swap(data_,other.data_);std::swap(size_,other.size_);std::swap(capacity_,other.capacity_);}
public:
    Buffer()=default;
    Buffer(const Buffer& other) {Reserve(bolt::native::cast<std::int32_t>(other.capacity_));try{for(;size_<other.size_;++size_)std::construct_at(data_+size_,other.data_[size_]);}catch(...){release();throw;}}
    Buffer(Buffer&& other):Buffer(){if(allocator_==other.allocator_)swap_storage(other);else{Reserve(other.Count());for(const auto& value:other)Add(value);}}
    Buffer& operator=(const Buffer& other){changed();if(this!=&other){Buffer replacement;replacement.allocator_=allocator_;replacement.Reserve(bolt::native::cast<std::int32_t>(other.capacity_));for(const auto& value:other)replacement.Add(value);swap_storage(replacement);}return *this;}
    Buffer& operator=(Buffer&& other){changed();if(this!=&other){if(allocator_==other.allocator_){release();swap_storage(other);}else *this=static_cast<const Buffer&>(other);}return *this;}
    ~Buffer(){release();}
    std::int32_t Count()const{return bolt::native::cast<std::int32_t>(size_);}
    std::int32_t Capacity()const{return bolt::native::cast<std::int32_t>(capacity_);}
    T* begin()noexcept{return data_;}const T* begin()const noexcept{return data_;}
    T* end()noexcept{return data_?data_+size_:nullptr;}const T* end()const noexcept{return data_?data_+size_:nullptr;}
    void Reserve(std::int32_t count){
        changed();
        if(count<0)throw std::runtime_error("capacity must be nonnegative");
        auto n=static_cast<std::size_t>(count);if(n<=capacity_)return;
        T* replacement=allocator_.allocate(n);std::size_t alive=0;
        try{for(;alive<size_;++alive)std::construct_at(replacement+alive,std::move_if_noexcept(data_[alive]));}
        catch(...){std::destroy_n(replacement,alive);allocator_.deallocate(replacement,n);throw;}
        std::destroy_n(data_,size_);if(data_)allocator_.deallocate(data_,capacity_);data_=replacement;capacity_=n;
    }
    void Add(const T& value){
        changed();
        if(size_<capacity_){std::construct_at(data_+size_,value);++size_;return;}
        // Safe even when value aliases an element that Reserve relocates.
        T saved(value);
        ensure_capacity(size_ + 1);
        std::construct_at(data_+size_,std::move(saved));++size_;
    }
    void AppendRange(const bolt::native::Array<T>& values,std::int32_t offset,std::int32_t count){
        if(offset<0||count<0||offset>values.Count()||count>values.Count()-offset)throw std::runtime_error("invalid buffer source range");
        if (count == 0) return;
        changed();
        ensure_capacity(size_ + static_cast<std::size_t>(count));
        std::uninitialized_copy_n(values.begin() + offset, count, data_ + size_);
        size_ += static_cast<std::size_t>(count);
    }
    void AppendBuffer(std::int32_t offset, std::int32_t count, const Buffer& source) {
        if (offset < 0 || count < 0 || offset > source.Count() || count > source.Count() - offset)
            throw std::runtime_error("invalid buffer source range");
        if (!count) return;
        const auto required = size_ + static_cast<std::size_t>(count);
        if (required > capacity_) {
            // The source may be this buffer, or a field of one of its elements.
            // Keep the old storage alive until both copies have succeeded.
            Buffer grown;
            grown.allocator_ = allocator_;
            grown.Reserve(detail::buffer_growth_capacity(capacity_, required));
            if (size_) std::uninitialized_copy_n(data_, size_, grown.data_);
            grown.size_ = size_;
            std::uninitialized_copy_n(source.data_ + offset, count, grown.data_ + grown.size_);
            grown.size_ = required;
            swap_storage(grown);
        } else {
            // Appending to oneself always writes past the end of the source range.
            std::uninitialized_copy_n(source.data_ + offset, count, data_ + size_);
            changed();
            size_ = required;
        }
    }
    void Truncate(std::int32_t count){changed();if(count<0||static_cast<std::size_t>(count)>size_)throw std::runtime_error("invalid buffer truncation");while(size_>static_cast<std::size_t>(count))std::destroy_at(data_+--size_);}
    void Swap(Buffer& other){
        changed(); other.changed();
        if(this==&other)return;
        if(allocator_==other.allocator_){swap_storage(other);return;}
        Buffer left,right;left.allocator_=allocator_;right.allocator_=other.allocator_;
        left.Reserve(other.Capacity());right.Reserve(Capacity());
        for(const auto& value:other)left.Add(value);
        for(const auto& value:*this)right.Add(value);
        swap_storage(left);other.swap_storage(right);
    }
    void Clear()noexcept{changed();std::destroy_n(data_,size_);size_=0;}
    template<class I> T& At(I index){return data_[bolt::native::checked_index(index,size_)];}
    template<class I> const T& At(I index)const{return data_[bolt::native::checked_index(index,size_)];}
    template<class I> void RemoveAt(I index){changed();auto i=bolt::native::checked_index(index,size_);for(;i+1<size_;++i)data_[i]=std::move(data_[i+1]);std::destroy_at(data_+--size_);}
    template<class I> void Insert(I index,const T& value){
        changed();
        const auto i=bolt::native::checked_index(index,size_,true);T saved(value);
        if(size_==capacity_)ensure_capacity(size_ + 1);
        if(i==size_)std::construct_at(data_+size_,std::move(saved));
        else{std::construct_at(data_+size_,std::move_if_noexcept(data_[size_-1]));++size_;for(auto j=size_-2;j>i;--j)data_[j]=std::move(data_[j-1]);data_[i]=std::move(saved);return;}
        ++size_;
    }
    bolt::native::Array<T> ToArray()const{return size_?bolt::native::Array<T>::FromRange(begin(),end()):bolt::native::Array<T>{};}
    bool operator==(const Buffer& other)const{if(size_!=other.size_)return false;for(std::size_t i=0;i<size_;++i)if(!bolt::native::value_equal(data_[i],other.data_[i]))return false;return true;}
    std::size_t BoltHash()const{std::size_t h=size_;for(const auto& v:*this)h=bolt::native::hash_combine(h,bolt::native::Hasher<T>{}(v));return h;}
};
} // namespace bolt::memory
