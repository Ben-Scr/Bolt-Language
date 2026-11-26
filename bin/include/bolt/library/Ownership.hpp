#pragma once
#include "Exceptions.hpp"
#include <memory>
#include <utility>
namespace bolt::library {
    // A heap owner with Bolt VALUE semantics: copy = an independent deep copy;
    // move = transfer; destruction/Reset deterministically destroys the owned value.
    // Pointers from Borrow() never extend lifetime. No implicit raw-pointer adoption.
    template<class T> class Owner {
        std::unique_ptr<T> value_;
        public:
        Owner()=default;
        bool operator==(const Owner& other)const {
            return value_&&other.value_?native::value_equal(*value_,*other.value_):!value_&&!other.value_;
        }
        std::size_t BoltHash()const {
            return value_?native::Hasher<T> {
            }
            (*value_):0;
        }
        explicit Owner(const T& value):value_(std::make_unique<T>(value)) {
        }
        Owner(const Owner& other):value_(other.value_?std::make_unique<T>(*other.value_):nullptr) {
        }
        Owner(Owner&&)=default;
        Owner& operator=(const Owner& other) {
            if(this!=&other) {
                auto copy=other.value_?std::make_unique<T>(*other.value_):nullptr;
                value_=std::move(copy);
            }
            return *this;
        }
        Owner& operator=(Owner&&)=default;
        static Owner Create() {
            Owner owner;
            owner.value_=std::make_unique<T>();
            return owner;
        }
        bool HasValue()const noexcept {
            return static_cast<bool>(value_);
        }
        T& Get() {
            if(!value_)throw InvalidOperationException("The Owner is empty.");
            return *value_;
        }
        const T& Get()const {
            if(!value_)throw InvalidOperationException("The Owner is empty.");
            return *value_;
        }
        T* Borrow()noexcept {
            return value_.get();
        }
        void Reset()noexcept {
            value_.reset();
        }
        void Reset(const T& value) {
            auto copy=std::make_unique<T>(value);
            value_=std::move(copy);
        }
        T* Release()noexcept {
            return value_.release();
        }
        // Caller explicitly takes ownership.
    };
}
