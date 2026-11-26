#pragma once
#include "Exceptions.hpp"
#include "../Std.hpp"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <type_traits>
namespace bolt::library {
    class Random {
        std::mt19937_64 generator_;
        static std::uint64_t fresh_seed() {
            static std::atomic<std::uint64_t> counter {
                0
            };
            std::random_device device;
            return (static_cast<std::uint64_t>(device())<<32)^device()^(counter.fetch_add(1,std::memory_order_relaxed)*0x9E3779B97F4A7C15ULL);
        }
        public:
        Random():generator_(fresh_seed()) {
        }
        explicit Random(std::uint64_t seed):generator_(seed) {
        }
        bool operator==(const Random&)const=default;
        void Seed(std::uint64_t seed) {
            generator_.seed(seed);
        }
        template<class T> T Next(T maximum) {
            return Next<T>(T {
                0
            },maximum);
        }
        template<class T> T Next(T minimum,T maximum) {
            static_assert((std::is_integral_v<T>||std::is_floating_point_v<T>)&&!std::is_same_v<T,bool>&&!std::is_same_v<T,char32_t>);
            if constexpr(std::is_floating_point_v<T>)if(!std::isfinite(minimum)||!std::isfinite(maximum))throw ArgumentOutOfRangeException("Random bounds must be finite.");
            if(!(minimum<maximum))throw ArgumentOutOfRangeException("Random.Next requires min < max (exclusive upper bound).");
            if constexpr(std::is_integral_v<T>) {
                using Work=std::conditional_t<std::is_signed_v<T>,std::int64_t,std::uint64_t>;
                return static_cast<T>(std::uniform_int_distribution<Work>(static_cast<Work>(minimum),static_cast<Work>(maximum)-1)(generator_));
            }
            else {
                const auto unit=std::generate_canonical<T,std::numeric_limits<T>::digits>(generator_);
                const auto result=std::lerp(minimum,maximum,unit);
                return result<maximum?result:std::nextafter(maximum,minimum);
            }
        }
        double NextDouble() {
            return Next<double>(0.0,1.0);
        }
        float NextFloat() {
            return Next<float>(0.0f,1.0f);
        }
        bool NextBool() {
            return (generator_()&1u)!=0;
        }
        void NextBytes(native::Array<std::uint8_t>& values) {
            for(std::int32_t i=0;i<values.Count();++i)values.At(i)=static_cast<std::uint8_t>(generator_());
        }
    };
}
