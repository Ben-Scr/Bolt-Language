#pragma once
#include <emscripten/em_asm.h>
#include <stddef.h>

static inline int bolt_web_random_fill(void* buffer, size_t length) {
    return EM_ASM_INT({
        try {
            const provider = globalThis.crypto || require('node:crypto').webcrypto;
            const size = Number($1);
            // Web Crypto accepts at most 65536 bytes per call. An ordinary
            // typed array also works when the destination is shared Wasm memory.
            const bytes = new Uint8Array(Math.min(size, 65536));
            for (let offset = 0; offset < size; offset += bytes.length) {
                const chunk = bytes.subarray(0, Math.min(bytes.length, size - offset));
                provider.getRandomValues(chunk);
                HEAPU8.set(chunk, Number($0) + offset);
            }
            return 0;
        } catch (error) {
            return -1;
        }
    }, buffer, length);
}
