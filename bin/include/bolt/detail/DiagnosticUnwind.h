#pragma once
#include <stdint.h>
#include <unwind.h>

/* Android and musl toolchains expose a callback-only unwind ABI. Keep the C callback
   synchronous so the caller owns both the buffer and its lifetime. */
struct bolt_unwind_buffer { void** frames; int count; int capacity; };
static inline _Unwind_Reason_Code bolt_unwind_frame(struct _Unwind_Context* context, void* data) {
    struct bolt_unwind_buffer* buffer = (struct bolt_unwind_buffer*)data;
    uintptr_t ip = _Unwind_GetIP(context);
    if (ip) buffer->frames[buffer->count++] = (void*)ip;
    return buffer->count == buffer->capacity ? _URC_END_OF_STACK : _URC_NO_REASON;
}
#ifndef BOLT_DIAGNOSTIC_LINKAGE
#define BOLT_DIAGNOSTIC_LINKAGE static inline
#endif
BOLT_DIAGNOSTIC_LINKAGE int bolt_diagnostic_backtrace(void** frames, int capacity) {
    struct bolt_unwind_buffer buffer = {frames, 0, capacity};
    if (capacity > 0) _Unwind_Backtrace(bolt_unwind_frame, &buffer);
    return buffer.count;
}
