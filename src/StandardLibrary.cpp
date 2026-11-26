#include <bolt/Frontend.hpp>
#include "ParseCache.hpp"
#include <algorithm>
#include <deque>
#include <span>
#ifndef __EMSCRIPTEN__
#include <future>
#include <thread>
#endif

namespace bolt {
namespace {
struct LibrarySource {
    const char* filename;
    std::string_view text;
    template<std::size_t N>
    constexpr LibrarySource(const char* name,const char (&source)[N]) : filename(name),text(source,N-1) {}
};
constexpr LibrarySource sources[] = {
#include "stdlib/Embedded.inc"
};

void append_library(Program& program, Program&& library) {
    for(auto& type:library.source_types)program.source_types.push_back(std::move(type));
    for(auto& c:library.classes)program.classes.push_back(std::move(c));
    for(auto& e:library.enums)program.enums.push_back(std::move(e));
    for(auto& c:library.concepts)program.concepts.push_back(std::move(c));
    program.sources.merge(library.sources);
    program.active_sources.merge(library.active_sources);
    program.source_preprocessors.merge(library.source_preprocessors);
}

void parse_libraries(Program& program) {
    const std::span<const LibrarySource> files(sources);
    auto parse_file = [&options = program.preprocessor](const LibrarySource& file) {
        return parse(file.text, file.filename, options);
    };
    auto append_files = [&](std::span<const LibrarySource> batch) {
        for (const auto& file : batch) append_library(program, parse_file(file));
    };
#ifndef __EMSCRIPTEN__
    // Persistent hosts maintain a transactional, thread-local parse cache.
    // Uncached files share only immutable inputs until the ordered merge.
    const auto workers = current_parse_cache() ? 1u : std::min(4u, std::thread::hardware_concurrency());
    if (workers > 1) {
        const auto batch_size = (files.size() + workers - 1) / workers;
        std::vector<std::future<std::deque<Program>>> pending;
        for (auto start = batch_size; start < files.size(); start += batch_size) {
            auto batch = files.subspan(start, std::min(batch_size, files.size() - start));
            pending.push_back(std::async([batch, parse_file] {
                std::deque<Program> parsed;
                for (const auto& file : batch) parsed.push_back(parse_file(file));
                return parsed;
            }));
        }
        append_files(files.first(batch_size));
        for (auto& result : pending)
            for (auto& library : result.get()) append_library(program, std::move(library));
        return;
    }
#endif
    append_files(files);
}
}
void add_standard_library(Program& program) {
    if(program.standard_library_loaded)return;
    parse_libraries(program);
    program.standard_library_loaded=true;
    if(program.prelude_enabled) {
        program.implicit_static_imports.push_back("System.ConsoleHelpers");
        program.implicit_static_imports.push_back("System.Process");
    }
}
}
