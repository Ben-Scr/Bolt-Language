#pragma once
#include "ParseCache.hpp"
#include <bolt/Frontend.hpp>
#include <mutex>

namespace bolt {
// A compiler session owns source trees before semantic analysis. Each returned
// program can outlive the session and can run independently of further edits.
class InterpreterSourceCache {
    ParseCache cache_;
    std::mutex mutex_;
public:
    Program compile(std::string_view source, const std::string& filename) {
        std::lock_guard lock(mutex_);
        ParseCacheScope scope(cache_);
        return compile_source(source, filename);
    }
};
}
