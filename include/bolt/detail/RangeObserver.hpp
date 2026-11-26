#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace bolt::native::detail {

// A Bolt foreach observes the object being enumerated even when a mutation
// reuses its storage. Empty buckets need one atomic read per mutation. Ranges
// can be copied or released on another thread without changing value layouts.
class RangeObserver {
    struct Bucket {
        std::mutex mutex;
        std::atomic<unsigned> count{0};
        RangeObserver* first = nullptr;
    };
    static Bucket& bucket(const void* owner) noexcept {
        static Bucket buckets[32];
        const auto address = reinterpret_cast<std::uintptr_t>(owner);
        return buckets[(address >> 3) & 31u];
    }
    const void* owner_;
    std::atomic<bool> valid_{true};
    RangeObserver* next_ = nullptr;
    RangeObserver** link_ = nullptr;

    void attach() {
        auto& list = bucket(owner_);
        std::lock_guard guard(list.mutex);
        next_ = list.first;
        link_ = &list.first;
        if (next_) next_->link_ = &next_;
        list.first = this;
        list.count.fetch_add(1,std::memory_order_release);
    }
public:
    explicit RangeObserver(const void* owner) : owner_(owner) { attach(); }
    RangeObserver(const RangeObserver& source) : owner_(source.owner_) {
        attach();
        if (!source.valid()) valid_.store(false,std::memory_order_release);
    }
    RangeObserver& operator=(const RangeObserver&) = delete;
    ~RangeObserver() {
        auto& list = bucket(owner_);
        std::lock_guard guard(list.mutex);
        *link_ = next_;
        if (next_) next_->link_ = link_;
        list.count.fetch_sub(1,std::memory_order_release);
    }
    static void changed(const void* owner) noexcept {
        auto& list = bucket(owner);
        if (!list.count.load(std::memory_order_acquire)) return;
        std::lock_guard guard(list.mutex);
        for (auto* entry=list.first; entry; entry=entry->next_)
            if (entry->owner_ == owner) entry->valid_.store(false,std::memory_order_release);
    }
    bool valid() const noexcept { return valid_.load(std::memory_order_acquire); }
};

}
