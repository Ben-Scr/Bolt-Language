#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace bolt::detail {
class ExecutionBudget {
    const bool limited_;
    const std::uint64_t initial_steps_;
    alignas(std::uint64_t) std::uint64_t local_steps_;
    std::uint64_t* const remaining_steps_;
    std::atomic<std::size_t> runtime_depth_{0};
public:
    explicit ExecutionBudget(std::size_t max_steps,std::uint64_t* shared_steps=nullptr) noexcept
        :limited_(max_steps!=0),initial_steps_(static_cast<std::uint64_t>(max_steps)),
         local_steps_(initial_steps_),remaining_steps_(shared_steps?shared_steps:&local_steps_) {}

    void reset() noexcept {
        std::atomic_ref<std::uint64_t>(*remaining_steps_).store(initial_steps_,std::memory_order_relaxed);
        runtime_depth_.store(0,std::memory_order_relaxed);
    }
    bool consume_step() noexcept {
        if(!limited_)return true;
        std::atomic_ref<std::uint64_t> budget(*remaining_steps_);
        auto remaining=budget.load(std::memory_order_relaxed);
        while(remaining){
            if(budget.compare_exchange_weak(remaining,remaining-1,std::memory_order_relaxed))return true;
        }
        return false;
    }
    bool enter_runtime(std::size_t maximum) noexcept {
        auto depth=runtime_depth_.load(std::memory_order_relaxed);
        while(depth<maximum){
            if(runtime_depth_.compare_exchange_weak(depth,depth+1,std::memory_order_relaxed))return true;
        }
        return false;
    }
    void leave_runtime() noexcept {runtime_depth_.fetch_sub(1,std::memory_order_relaxed);}
};

using SharedExecutionBudget=std::shared_ptr<ExecutionBudget>;

class RuntimeDepthGuard {
    ExecutionBudget* budget_;
public:
    RuntimeDepthGuard(ExecutionBudget& budget,std::size_t maximum) noexcept
        :budget_(budget.enter_runtime(maximum)?&budget:nullptr) {}
    ~RuntimeDepthGuard(){if(budget_)budget_->leave_runtime();}
    RuntimeDepthGuard(const RuntimeDepthGuard&)=delete;
    RuntimeDepthGuard& operator=(const RuntimeDepthGuard&)=delete;
    explicit operator bool()const noexcept{return budget_!=nullptr;}
};
}
