#include "../src/native/JitCall.hpp"
#include <bolt/native/IR.hpp>
#include <algorithm>
#include <array>
#if defined(_WIN32) && (defined(_M_ARM64)||defined(__aarch64__)||defined(_M_X64)||defined(__x86_64__)) && !defined(_M_ARM64EC) && !defined(__arm64ec__)
#include "WindowsStackwalk.hpp"
#define BOLT_TEST_WINDOWS_SEH 1
#endif
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using namespace bolt::ir;
using U64 = std::uint64_t;
using bolt::native_backend::detail::call_generated;

Function function(std::string name, Type result, std::vector<Type> parameters) {
    Function fn;
    fn.name = std::move(name);
    fn.result = result;
    fn.exported = true;
    fn.parameters = std::move(parameters);
    fn.registers.insert(fn.registers.end(),fn.parameters.begin(),fn.parameters.end());
    fn.blocks.emplace_back();
    return fn;
}

Reg reg(Function& fn, Type type) {
    fn.registers.push_back(type);
    return static_cast<Reg>(fn.registers.size()-1);
}

Reg cast(Function& fn, Reg input, Type to, std::string operation) {
    if (fn.registers.at(input)==to) return input;
    auto dst=reg(fn,to);
    fn.blocks[0].code.push_back({.op=Op::Cast,.result=dst,.args={input},.text=std::move(operation)});
    return dst;
}

const char* opname(Op op) {
    switch (op) {
    case Op::CheckedAdd: return "add";
    case Op::CheckedSub: return "sub";
    case Op::CheckedMul: return "mul";
    default: throw std::logic_error("unexpected checked operation");
    }
}

std::string checked_name(Type type, Op op, bool sign) {
    return std::string(sign?"s":"u")+opname(op)+name(type);
}

Function checked(Type type, Op op, bool sign) {
    auto fn=function(checked_name(type,op,sign),Type::I64,{Type::I64,Type::I64,Type::Ptr});
    auto a=cast(fn,1,type,"trunc"),b=cast(fn,2,type,"trunc");
    auto dst=reg(fn,type),overflow=reg(fn,Type::I32);
    auto& code=fn.blocks[0].code;
    code.push_back({.op=op,.result=dst,.overflow=overflow,.args={a,b},.text=sign?"s":"u"});
    code.push_back({.op=Op::Store,.args={3,overflow}});
    auto full=cast(fn,dst,Type::I64,"zext");
    code.push_back({.op=Op::Return,.args={full}});
    return fn;
}

// This oracle works entirely with unsigned arithmetic. Signed magnitudes avoid
// overflowing C++ when a test input is INT64_MIN, including INT64_MIN * -1.
bool overflows(Op op, bool sign, U64 a, U64 b, U64 result, U64 mask, U64 sign_bit) {
    if (!sign) {
        if (op==Op::CheckedAdd) return a>mask-b;
        if (op==Op::CheckedSub) return a<b;
        return b && a>mask/b;
    }
    if (op==Op::CheckedAdd) return (~(a^b) & (a^result) & sign_bit)!=0;
    if (op==Op::CheckedSub) return ((a^b) & (a^result) & sign_bit)!=0;
    const U64 limit=(a^b)&sign_bit?sign_bit:sign_bit-1;
    const auto magnitude=[&](U64 value){return value&sign_bit?(-value)&mask:value;};
    const auto rhs=magnitude(b);
    return rhs && magnitude(a)>limit/rhs;
}

std::size_t test_checked(const bolt::native_backend::JitImage& jit, Type type, Op op, bool sign) {
    using Binary=U64(*)(U64,U64,std::uint32_t*);
    auto symbol=checked_name(type,op,sign);
    auto call=reinterpret_cast<Binary>(jit.symbol(symbol));
    unsigned bits=width(type)*8;
    const U64 sign_bit=U64{1}<<(bits-1),mask=sign_bit|(sign_bit-1);
    std::vector<U64> inputs={0,1,2,sign_bit-2,sign_bit-1,sign_bit,sign_bit+1,mask-1,mask};
    std::mt19937_64 random(0x215a8b30u+bits);
    for (int n=0;n<16;++n) inputs.push_back(random()&mask);
    for (auto a:inputs) for (auto b:inputs) {
        U64 expected=(op==Op::CheckedAdd?a+b:op==Op::CheckedSub?a-b:a*b)&mask;
        std::uint32_t flag=7;
        auto result=call_generated(call,a,b,&flag);
        if (result!=expected || flag!=overflows(op,sign,a,b,expected,mask,sign_bit))
            throw std::runtime_error(symbol+" mismatch for "+std::to_string(a)+", "+std::to_string(b));
    }
    return inputs.size()*inputs.size();
}

constexpr std::array predicates={"oeq","one","une","olt","ole","ogt","oge"};

bool float_check(std::string_view predicate,double a,double b) {
    if (predicate=="oeq") return a==b;
    if (predicate=="one") return !std::isnan(a) && !std::isnan(b) && a!=b;
    if (predicate=="une") return a!=b;
    if (predicate=="olt") return a<b;
    if (predicate=="ole") return a<=b;
    if (predicate=="ogt") return a>b;
    return a>=b;
}

template<class Float>
std::size_t test_float(const bolt::native_backend::JitImage& jit,Type type,const char* predicate) {
    using Compare=std::uint32_t(*)(Float,Float);
    auto symbol=std::string(name(type))+predicate;
    auto call=reinterpret_cast<Compare>(jit.symbol(symbol));
    using Limit=std::numeric_limits<Float>;
    const Float values[]={-Limit::infinity(),Float(-2.5),-Limit::denorm_min(),Float(-0.),Float(0.),
                          Limit::denorm_min(),Float(2.5),Limit::infinity(),Limit::quiet_NaN()};
    for (auto a:values) for (auto b:values)
        if (call_generated(call,a,b)!=float_check(predicate,a,b)) throw std::runtime_error(symbol+" comparison mismatch");
    return std::size(values)*std::size(values);
}

// A compiled C++ caller enters the JIT and the JIT calls an independently
// compiled C++ function. Ten alternating integer/FP arguments exhaust both
// register banks on AAPCS64 and SysV and cover the Win64 stack home area.
#ifdef BOLT_TEST_WINDOWS_SEH
DWORD64 expected_frame=0;
bool recovered_frame=false;


#endif

using Mixed=U64(*)(U64,double,U64,double,U64,double,U64,double,U64,double,
                   U64,double,U64,double,U64,double,U64,double,U64,double);
U64 mixed(U64 a,double b,U64 c,double d,U64 e,double f,U64 g,double h,U64 i,double j,
          U64 k,double l,U64 m,double n,U64 o,double p,U64 q,double r,U64 s,double t) {
#ifdef BOLT_TEST_WINDOWS_SEH
    if(expected_frame)recovered_frame=bolt_test::walk_through_generated_frame(expected_frame);
#endif
    U64 result=19;
    for (auto value:{a,std::bit_cast<U64>(b),c,std::bit_cast<U64>(d),e,std::bit_cast<U64>(f),
                    g,std::bit_cast<U64>(h),i,std::bit_cast<U64>(j),k,std::bit_cast<U64>(l),
                    m,std::bit_cast<U64>(n),o,std::bit_cast<U64>(p),q,std::bit_cast<U64>(r),
                    s,std::bit_cast<U64>(t)}) result=1099511628211ull*result+value;
    return result;
}

U64 invoke_mixed(Mixed call) {
    return call_generated(call,1,-2.25,3,4.5,5,-6.75,7,8.125,9,-10.25,11,12.5,13,-14.75,15,16.0,
                0xffffffffffEEEDDDull,18.125,19,-20.5);
}

constexpr std::array<std::size_t,11> lengths={0,1,2,3,7,8,31,63,64,65,4099};
constexpr std::array<std::size_t,4> record_sizes={65,96,127,128};

Module make_module() {
    Module m;
    for (auto size:record_sizes) {
        auto fn=function("record"+std::to_string(size),Type::Void,{Type::Ptr,Type::Ptr});
        fn.blocks[0].code={{.op=Op::CopyElement,.args={1,2},.immediate=size},{.op=Op::Return}};
        m.functions.push_back(std::move(fn));
    }
    for (auto type:{Type::I8,Type::I16,Type::I32,Type::I64})
        for (auto op:{Op::CheckedAdd,Op::CheckedSub,Op::CheckedMul})
            for (bool sign:{false,true}) m.functions.push_back(checked(type,op,sign));
    for (auto type:{Type::F32,Type::F64}) for (const char* predicate:predicates) {
        auto fn=function(std::string(name(type))+predicate,Type::I32,{type,type});
        auto dst=reg(fn,Type::I32);
        fn.blocks[0].code={{.op=Op::Compare,.result=dst,.args={1,2},.text=predicate},
                           {.op=Op::Return,.args={dst}}};
        m.functions.push_back(std::move(fn));
    }
    for (auto size:lengths) {
        auto fn=function("zero"+std::to_string(size),Type::Void,{Type::Ptr});
        fn.blocks[0].code={{.op=Op::Zero,.args={1},.immediate=size},{.op=Op::Return}};
        m.functions.push_back(std::move(fn));
    }
    auto large=function("large_frame",Type::I64,{Type::I64});
    auto base=reg(large,Type::Ptr),end=reg(large,Type::Ptr),value=reg(large,Type::I64);
    large.blocks[0].code={{.op=Op::Alloca,.result=base,.immediate=70008},
                          {.op=Op::Offset,.result=end,.args={base},.immediate=70000},
                          {.op=Op::Store,.args={end,1}},{.op=Op::Load,.result=value,.args={end}},
                          {.op=Op::Return,.args={value}}};
    m.functions.push_back(std::move(large));
    std::vector<Type> signature;
    std::vector<Reg> arguments;
    for (int i=0;i<10;++i) {
        signature.insert(signature.end(),{Type::I64,Type::F64});
        arguments.insert(arguments.end(),{Reg(2*i+1),Reg(2*i+2)});
    }
    m.externals.push_back({"native_mixed",Type::I64,signature,true});
    auto forward=function("forward",Type::I64,signature);
    auto result=reg(forward,Type::I64);
    forward.blocks[0].code={{.op=Op::Call,.result=result,.args=arguments,.text="native_mixed"},
                            {.op=Op::Return,.args={result}}};
    m.functions.push_back(std::move(forward));
    return m;
}
}

int main() {
    using namespace bolt::native_backend;
    if (!fast_supported()) { std::cout<<"SKIP no Fast backend for this host\n"; return 0; }
    try {
        auto module=make_module();
        auto jit=fast_jit(module,{{"native_mixed",reinterpret_cast<void*>(&mixed)}});
        std::size_t checks=0;
        for (auto size:record_sizes) {
            std::vector<std::uint8_t> source(size), dest(size+2,0xb3);
            for (unsigned i=0;i<size;++i) source[i]=static_cast<std::uint8_t>(i*17);
            auto copy=reinterpret_cast<void(*)(void*,const void*)>(jit->symbol("record"+std::to_string(size)));
            call_generated(copy,dest.data()+1,source.data());
            call_generated(copy,dest.data()+1,dest.data()+1);
            if (dest.front()!=0xb3 || dest.back()!=0xb3 || !std::equal(source.begin(),source.end(),dest.begin()+1))
                throw std::runtime_error("record copy failed at length "+std::to_string(size));
            ++checks;
        }
        for (auto type:{Type::I8,Type::I16,Type::I32,Type::I64})
            for (auto op:{Op::CheckedAdd,Op::CheckedSub,Op::CheckedMul})
                for (bool sign:{false,true}) checks+=test_checked(*jit,type,op,sign);
        for (const char* predicate:predicates) {
            checks+=test_float<float>(*jit,Type::F32,predicate);
            checks+=test_float<double>(*jit,Type::F64,predicate);
        }
        for (auto size:lengths) {
            std::vector<std::uint8_t> data(size+2,0xa5),expected(size+2);
            expected.front()=expected.back()=0xa5;
            auto zero=reinterpret_cast<void(*)(void*)>(jit->symbol("zero"+std::to_string(size)));
            call_generated(zero,data.data()+1);
            if (data!=expected) throw std::runtime_error("zeroing failed at length "+std::to_string(size));
            ++checks;
        }
        auto large=reinterpret_cast<U64(*)(U64)>(jit->symbol("large_frame"));
        if (call_generated(large,0xffffeeddbbccaa99ull)!=0xffffeeddbbccaa99ull) throw std::runtime_error("large frame mismatch");
        const auto forward=reinterpret_cast<Mixed>(jit->symbol("forward"));
#ifdef BOLT_TEST_WINDOWS_SEH
        expected_frame=reinterpret_cast<DWORD64>(forward);
#endif
        const auto mixed_result=invoke_mixed(forward);
#ifdef BOLT_TEST_WINDOWS_SEH
        expected_frame=0;
        if(!recovered_frame)throw std::runtime_error("failed to unwind through the live Windows JIT frame");
        jit.reset();
        DWORD64 released_base=0;
        if(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(forward),&released_base,nullptr))
            throw std::runtime_error("stale Windows JIT function table after destroying image");
        std::cout<<"PASS Windows JIT unwind registration, stack walk and removal\n";
#endif
        if (mixed_result!=invoke_mixed(mixed))throw std::runtime_error("mixed C++ and Bolt ABI mismatch");
        std::cout<<"PASS "<<checks+2<<" Fast codegen checks (integers, floats, memory, stack, C ABI)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}
