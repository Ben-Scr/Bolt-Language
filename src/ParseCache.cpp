#include "ParseCache.hpp"
#include <bolt/Frontend.hpp>
#include <algorithm>
#include <exception>

namespace bolt {
namespace {
thread_local ParseCache* current=nullptr;

void detach_defaults(std::vector<Parameter>& parameters) {
    for(auto& parameter:parameters)
        if(parameter.default_value)parameter.default_value=clone(*parameter.default_value);
}
}

ParseCache* current_parse_cache() noexcept { return current; }

ParseCacheScope::ParseCacheScope(ParseCache& value)
    :cache(value),previous(current),exceptions_at_entry(std::uncaught_exceptions()) {
    current=&cache;
    if (cache.active_scope) return;
    cache.active_scope=this;
    cache.visited.clear();
    cache.reused=cache.parsed=0;
}
ParseCacheScope::~ParseCacheScope() {
    current=previous;
    if (cache.active_scope!=this) return;
    cache.active_scope=nullptr;
    if (!failed && std::uncaught_exceptions()==exceptions_at_entry) {
        if(complete_project)
            std::erase_if(cache.files,[this](const auto& entry){return !cache.visited.contains(entry.first);});
        return;
    }
    // A failed edit may stop before the compiler visits the remaining files.
    // Restore the last successful generation, including any replaced source.
    for (auto& [name, original] : originals) {
        if (original) cache.files.at(name)=std::move(*original);
        else cache.files.erase(name);
    }
}
const Program* ParseCache::lookup(const std::string& name,std::string_view bytes,const std::string& configuration) {
    visited.insert(name);
    const auto it=files.find(name);
    if(it!=files.end()&&it->second.configuration==configuration&&it->second.tree.sources.at(name)==bytes) {
        ++reused;
        return &it->second.tree;
    }
    ++parsed;
    return nullptr;
}
void ParseCache::reuse_standard_library() {
    for(const auto& [name,entry]:files)
        if(name.starts_with("<bolt-stdlib>/") && visited.insert(name).second) ++reused;
}
const Program& ParseCache::save(const std::string& name,std::string configuration,Program tree) {
    if (active_scope) {
        auto [record, first_change]=active_scope->originals.try_emplace(name);
        if (first_change) {
            auto previous_file=files.find(name);
            if (previous_file!=files.end()) record->second.emplace(std::move(previous_file->second));
        }
    }
    return files.insert_or_assign(name,Entry{std::move(configuration),std::move(tree),{}}).first->second.tree;
}

std::shared_ptr<const std::vector<Token>> navigation_tokens(const std::string& file, std::string_view active) {
    ParseCache::Entry* entry = nullptr;
    if (auto* cache = current_parse_cache()) {
        auto it = cache->files.find(file);
        if (it != cache->files.end()) {
            const auto source = it->second.tree.active_sources.find(file);
            if (source != it->second.tree.active_sources.end() && source->second == active)
                entry = &it->second;
        }
    }
    if (entry && entry->navigation_tokens) return entry->navigation_tokens;
    auto tokens = std::make_shared<std::vector<Token>>(tokenize(active, file));
    auto by_offset = [](const Token& a, const Token& b) {
        return a.at.offset < b.at.offset;
    };
    // Interpolations may synthesize out-of-order tokens. Ordinary source is
    // already in physical order straight from the lexer.
    if (!std::is_sorted(tokens->begin(), tokens->end(), by_offset))
        std::stable_sort(tokens->begin(), tokens->end(), by_offset);
    if (entry) entry->navigation_tokens = tokens;
    return tokens;
}

Program copy_syntax_tree(const Program& tree) {
    Program out;
    out.imports=tree.imports;
    out.source_types=tree.source_types;
    out.sources=tree.sources;
    out.active_sources=tree.active_sources;
    out.source_preprocessors=tree.source_preprocessors;
    out.preprocessor=tree.preprocessor;
    for(const auto& c:tree.classes) {
        auto& copy=out.classes.emplace_back(clone(c));
        for(auto& property:copy.properties)detach_defaults(property.parameters);
    }
    for(const auto& e:tree.enums) {
        auto& copy=out.enums.emplace_back();
        copy.name=e.name;
        copy.short_name=e.short_name;
        copy.namespace_name=e.namespace_name;
        copy.underlying=e.underlying;
        copy.native_cpp=e.native_cpp;
        copy.declaring_type=e.declaring_type;
        copy.is_public=e.is_public;
        copy.is_template_member=e.is_template_member;
        copy.token=e.token;
        copy.imports=e.imports;
        copy.attributes=e.attributes;
        for(const auto& member:e.members)
            copy.members.push_back({member.name,member.token,member.expression?clone(*member.expression):nullptr,member.value});
    }
    for(const auto& c:tree.concepts) {
        auto& copy=out.concepts.emplace_back();
        copy.name=c.name;
        copy.parameter=c.parameter;
        copy.namespace_name=c.namespace_name;
        copy.parameters=c.parameters;
        copy.imports=c.imports;
        copy.token=c.token;
        if(c.expression)copy.expression=clone(*c.expression);
        for(const auto& r:c.requirements) {
            auto& req=copy.requirements.emplace_back();
            req.parameters=r.parameters;
            detach_defaults(req.parameters);
            req.result_type=r.result_type;
            req.token=r.token;
            if(r.expression)req.expression=clone(*r.expression);
        }
    }
    return out;
}
}
