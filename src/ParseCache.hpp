#pragma once
#include <bolt/Ast.hpp>
#include <optional>
#include <unordered_set>

namespace bolt {
class ParseCacheScope;
// Used by persistent frontends only. Entries are syntax trees before semantic
// analysis. Each compilation receives its own mutable copy.
class ParseCache {
    struct Entry {
        std::string configuration;
        Program tree;
        std::shared_ptr<const std::vector<Token>> navigation_tokens;
    };
    std::unordered_map<std::string,Entry> files;
    std::unordered_set<std::string> visited;
    ParseCacheScope* active_scope=nullptr;
    friend class ParseCacheScope;
public:
    const Program* syntax(const std::string& name) const {
        auto it=files.find(name);
        return it==files.end()?nullptr:&it->second.tree;
    }
    void reuse_standard_library();
    std::size_t reused=0, parsed=0;
    const Program* lookup(const std::string& name,std::string_view bytes,const std::string& configuration);
    friend std::shared_ptr<const std::vector<Token>> navigation_tokens(const std::string&, std::string_view);
    const Program& save(const std::string& name,std::string configuration,Program tree);
};

// Parsing also happens recursively inside semantic analysis (generated adapters
// and embedded stdlib). A scoped thread-local selects the caller's session.
class ParseCacheScope {
    ParseCache& cache;
    ParseCache* previous;
    int exceptions_at_entry;
    bool failed=false;
    bool complete_project=true;
    std::unordered_map<std::string,std::optional<ParseCache::Entry>> originals;
    friend class ParseCache;
public:
    explicit ParseCacheScope(ParseCache&);
    ~ParseCacheScope();
    void discard_changes() noexcept { failed=true; }
    // Partial passes may populate entries without visiting other project files.
    void retain_unvisited() noexcept { complete_project=false; }
    ParseCacheScope(const ParseCacheScope&)=delete;
    ParseCacheScope& operator=(const ParseCacheScope&)=delete;
};
ParseCache* current_parse_cache() noexcept;
// Tokens are ordered by physical source position for generic and attribute lookups.
std::shared_ptr<const std::vector<Token>> navigation_tokens(const std::string& file, std::string_view active);
Program parse_memory_source(std::string_view,const std::string&,const PreprocessorOptions&);
Program copy_syntax_tree(const Program& tree);
void validate_syntax(std::string_view source,const std::string& filename,const PreprocessorOptions& preprocessing);
}
