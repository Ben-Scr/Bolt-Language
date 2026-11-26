#pragma once
#include <bolt/native/ABI.hpp>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace bolt::tests {
inline void verify_storage_shifts() {
    using ir::Type;
    ir::Module module;
    module.externals.push_back({"br_memory_move",Type::Void,{Type::Ptr,Type::Ptr,Type::Ptr,Type::Ptr,Type::I64}});
    for (unsigned width : {1u,2u,4u,8u}) for (int direction : {0,-1,1}) {
        ir::Function f;
        f.name="shift"+std::to_string(width)+(direction<0 ? "left" : direction>0 ? "right" : "");
        f.exported=true;
        f.parameters={Type::Ptr,Type::Ptr,Type::Ptr,Type::I64};
        f.registers={Type::Void,Type::Ptr,Type::Ptr,Type::Ptr,Type::I64,Type::Ptr};
        ir::Instruction site; site.op=ir::Op::Constant; site.result=5;
        ir::Instruction move; move.op=ir::Op::Call; move.text="br_memory_move";
        move.args={1,5,2,3,4}; move.storage_move_stride=static_cast<std::uint8_t>(width);
        ir::Block block;
        block.code.push_back(site);
        if (direction) {
            ir::Instruction next; next.op=ir::Op::Offset; next.result=6;
            next.args={direction>0 ? 3u : 2u}; next.immediate=width;
            f.registers.push_back(Type::Ptr);
            block.code.push_back(next);
            move.args[direction>0 ? 2 : 3]=next.result;
        }
        block.code.push_back(move);
        ir::Instruction ret; ret.op=ir::Op::Return; block.code.push_back(ret);
        f.blocks.push_back(std::move(block));
        module.functions.push_back(std::move(f));
    }
    auto image=native_backend::llvm_jit(module,native_abi::symbols(),{.release=true,.debug_symbols=false});
    std::unique_ptr<BrContext,decltype(&br_destroy)> ctx(br_create(0,0),br_destroy);
    if (!ctx) throw std::runtime_error("cannot create shift test context");
    const auto detected=ctx->flags;
    for (auto flags : {detected,detected & ~BrContext::Avx512F,detected & ~(BrContext::Avx2|BrContext::Avx512F)}) {
        ctx->flags=flags;
        for (unsigned width : {1u,2u,4u,8u}) {
            using Shift=void(*)(BrContext*,void*,const void*,std::uint64_t);
            const auto shift=reinterpret_cast<Shift>(image->symbol("shift"+std::to_string(width)));
            const auto left=reinterpret_cast<Shift>(image->symbol("shift"+std::to_string(width)+"left"));
            const auto rightward=reinterpret_cast<Shift>(image->symbol("shift"+std::to_string(width)+"right"));
            for (unsigned boundary : {0u,32u,128u,160u,256u,512u,1024u,2048u,4096u,8192u,131072u,262144u}) {
                std::vector<unsigned char> seed(boundary+256);
                for (std::size_t i=0;i<seed.size();++i) seed[i]=static_cast<unsigned char>(i*31+i/17);
                auto actual=seed, expected=seed;
                for (auto n : {boundary>=width ? boundary-width : 0u,boundary,boundary+width}) {
                    for (unsigned offset=0;offset<64;offset+=boundary>8192 ? 7 : 1)
                    for (auto distance : {width,width*3}) for (bool right : {false,true}) {
                        const auto src=64+offset, dst=right ? src+distance : src-distance;
                        actual=seed; expected=seed;
                        std::memmove(expected.data()+dst,expected.data()+src,n);
                        shift(ctx.get(),actual.data()+dst,actual.data()+src,n);
                        if (actual!=expected || ctx->failed)
                            throw std::runtime_error("overlapping shift differs from memmove at width "+
                                std::to_string(width)+", size "+std::to_string(n)+", offset "+std::to_string(offset)+
                                ", displacement "+std::to_string(int(dst)-int(src)));
                        if (distance==width) {
                            actual=seed;
                            (right ? rightward : left)(ctx.get(),actual.data()+dst,actual.data()+src,n);
                            if (actual!=expected || ctx->failed)
                                throw std::runtime_error("adjacent storage shift differs from memmove");
                        }
                    }
                }
            }
        }
    }
}
} // namespace bolt::tests
