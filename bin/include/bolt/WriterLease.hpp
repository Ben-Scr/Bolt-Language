#pragma once
#include "library/IO.hpp"
namespace bolt::platform {
// Shared OS resource ownership; no value formatting or line-writing policy.
class WriterLease {
    struct State {
        std::shared_ptr<library::Stream> stream;
        bool leave_open=false;
        bool closed=false;
        State(library::Stream& value,bool leave):stream(value.CloneHandle()),leave_open(leave){}
        ~State() noexcept {
            if(!closed&&stream&&!leave_open) {
                try{stream->Close();}catch(...){/* Explicit Close/Flush reports I/O errors. */}
            }
        }
    };
    std::shared_ptr<State> state_;
    void check()const{
        if(!state_||state_->closed||!state_->stream)throw library::ObjectDisposedException("The writer is closed.");
    }
public:
    WriterLease() = default;
    WriterLease(library::Stream& value,bool leave_open) {
        if(!value.CanWrite())throw library::NotSupportedException("The stream is not writable.");
        state_=std::make_shared<State>(value,leave_open);
    }
    const void* Identity()const noexcept{return state_.get();}
    void WriteText(std::string_view text){
        check();state_->stream->WriteRaw(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
    }
    void Flush(){check();state_->stream->Flush();}
    void Close(){
        if(!state_||state_->closed)return;
        state_->stream->Flush();
        if(!state_->leave_open)state_->stream->Close();
        state_->closed=true;state_->stream.reset();
    }
};
}
