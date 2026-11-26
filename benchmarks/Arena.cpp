// Standalone microbenchmark; timings are observations, never test thresholds.
#include <bolt/ArenaCpp.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
double micros(Clock::time_point start,Clock::time_point end) {
    return std::chrono::duration<double,std::micro>(end-start).count();
}
template<std::size_t Bytes> struct Payload {
    std::array<std::uint64_t,Bytes/sizeof(std::uint64_t)> words;
    explicit Payload(std::uint64_t seed) {for(std::size_t i=0;i<words.size();++i)words[i]=seed+i;}
};
struct Sample {
    double setup,process,cleanup,dispose;
    std::size_t peak,retained;
    std::uint64_t checksum;
};
template<std::size_t Bytes> Sample measure(std::size_t count,bool use_arena) {
    const auto begin=Clock::now();
    std::vector<Payload<Bytes>*> pointers;
    pointers.reserve(count);
    std::unique_ptr<bolt::memory::Arena> arena;
    if(use_arena)arena=std::make_unique<bolt::memory::Arena>(count*sizeof(Payload<Bytes>),count*sizeof(Payload<Bytes>));
    const auto ready=Clock::now();
    for(std::size_t i=0;i<count;++i) {
        pointers.push_back(use_arena?bolt::native::arena_new<Payload<Bytes>>(*arena,i):new Payload<Bytes>(i));
    }
    std::uint64_t checksum=0;
    for(const auto* pointer:pointers)checksum+=pointer->words.front()+pointer->words.back();
    const auto processed=Clock::now();
    const auto peak=use_arena?arena->peak():count*sizeof(Payload<Bytes>);
    if(use_arena)arena->reset();
    else for(auto* pointer:pointers)delete pointer;
    const auto cleaned=Clock::now();
    const auto retained=use_arena?arena->capacity():0;
    if(use_arena)arena->dispose();
    const auto disposed=Clock::now();
    return {micros(begin,ready),micros(ready,processed),micros(processed,cleaned),micros(cleaned,disposed),peak,retained,checksum};
}
template<std::size_t Bytes> void report(std::size_t count,bool arena) {
    (void)measure<Bytes>(count,arena); // Warm-up is separate from reported samples.
    std::vector<Sample> samples;
    for(int i=0;i<7;++i)samples.push_back(measure<Bytes>(count,arena));
    auto median=[&](double Sample::*member) {
        std::vector<double> values;for(const auto& sample:samples)values.push_back(sample.*member);
        std::sort(values.begin(),values.end());return values[values.size()/2];
    };
    std::cout<<(arena?"arena":"heap")<<','<<count<<','<<Bytes<<','
        <<median(&Sample::setup)<<','<<median(&Sample::process)<<','<<median(&Sample::cleanup)<<','<<median(&Sample::dispose)<<','
        <<samples.back().peak<<','<<samples.back().retained<<','<<samples.back().checksum<<'\n';
}
}
int main() {
    std::cout<<"# Seven-sample phase medians in microseconds; one warm-up per case.\n"
        <<"# Requested heap payload versus occupied arena backing; excludes allocator/control-record overhead and process RSS.\n"
        <<"# Each case allocates count logical root objects; underlying system-allocation counts are not instrumented.\n"
        <<"mode,count,object_bytes,setup_us,construct_read_us,cleanup_us,dispose_us,peak_payload_bytes,retained_after_cleanup_bytes,checksum\n"
        <<std::fixed<<std::setprecision(2);
    report<64>(100000,false);report<64>(100000,true);
    report<4096>(5000,false);report<4096>(5000,true);
}
