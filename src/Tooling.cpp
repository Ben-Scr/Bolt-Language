#include "ProjectInputs.hpp"
#include "ParseCache.hpp"
#include <bolt/Path.hpp>
#include <bolt/Project.hpp>
#include <fstream>
#include <iterator>
#include <algorithm>
namespace bolt {
PreprocessorOptions project_preprocessor_options(const std::filesystem::path& root,const SourceOverrides& overrides,const PreprocessorOptions& requested) {
    auto options=requested;
    if(auto model=project::resolve(root,overrides)){
        if(options.configuration.empty())options.configuration=model->main.default_profile;
        for(const auto& define:model->main.defines)options.defines.try_emplace(define,true);
    }
    return normalized_preprocessor_options(std::move(options));
}
PreprocessorOptions source_preprocessor_options(const std::filesystem::path& file,const project::Graph* graph,const PreprocessorOptions& requested) {
    auto options=requested;
    if(graph) {
        const project::Model* owner=&graph->main;
        for(const auto& dependency:graph->dependencies)if(project::contains_source(dependency,file)){owner=&dependency;break;}
        if(options.configuration.empty())options.configuration=graph->main.default_profile;
        for(const auto& symbol:owner->defines)options.defines.try_emplace(symbol,true);
    }
    return normalized_preprocessor_options(std::move(options));
}
std::vector<std::filesystem::path> read_inspection_sources(Inspection& result,const std::filesystem::path& root,
        const SourceOverrides& overrides,const PreprocessorOptions& preprocessing) {
    auto inputs=project_inputs(root,overrides);
    read_inspection_sources(result,inputs,root,overrides,preprocessing);
    return std::move(inputs.files);
}
void read_inspection_sources(Inspection& result,const ProjectInputs& inputs,const std::filesystem::path& root,
        const SourceOverrides& overrides,const PreprocessorOptions& preprocessing) {
    const auto* graph=inputs.graph?&*inputs.graph:nullptr;
    result.preprocessor=source_preprocessor_options(root,graph,preprocessing);
    for(const auto& p:inputs.files) {
        const auto name=path_to_utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(p)));
        std::string source;
        if(auto it=overrides.find(name);it!=overrides.end())source=it->second;
        else {std::ifstream in(p,std::ios::binary);if(!in)throw Error({name,1,1},"cannot open source file","BLT4001");source.assign(std::istreambuf_iterator<char>(in),{});}
        result.sources.insert_or_assign(name,std::move(source));
        result.source_preprocessors[name]=source_preprocessor_options(p,graph,preprocessing);
    }
}
Inspection inspect_project(const std::filesystem::path& root,const SourceOverrides& overrides,bool require_entry,const PreprocessorOptions& preprocessing) {
    try {
        return inspect_project(project_inputs(root,overrides),root,overrides,require_entry,preprocessing);
    } catch(const Error& e) { Inspection result;result.diagnostics.push_back(e.diagnostic);return result; }
    catch(const std::exception& e) { Inspection result;result.diagnostics.push_back(tooling_diagnostic(e.what()));return result; }
}
Inspection inspect_project(const ProjectInputs& inputs,const std::filesystem::path& root,const SourceOverrides& overrides,
                           bool require_entry,const PreprocessorOptions& preprocessing) {
    Inspection result;
    try {
        // The common valid edit needs only one parse. If a project fails to
        // load, collect every file's syntax errors for the editor in one pass.
        Program p;
        try { p=load_project(inputs,root,overrides,require_entry,false,preprocessing); }
        catch(const Error& first) {
            read_inspection_sources(result,inputs,root,overrides,preprocessing);
            for(const auto& file:inputs.files) {
                const auto name=path_to_utf8(std::filesystem::weakly_canonical(std::filesystem::absolute(file)));
                const auto& source=result.sources.at(name);
                const auto& options=result.source_preprocessors.at(name);
                try { validate_syntax(source,name,options); }
                catch(const Error& error){result.diagnostics.push_back(error.diagnostic);}
            }
            if(result.diagnostics.empty())result.diagnostics.push_back(first.diagnostic);
            return result;
        }
        result.preprocessor=p.preprocessor;
        result.sources=p.sources;
        result.source_preprocessors=p.source_preprocessors;
        // Retain a partially analyzed AST for declarations / coloring when possible.
        result.program=std::make_unique<Program>(std::move(p));
        try{analyze(*result.program,require_entry&&!result.program->library_mode);}
        catch(Error& e){if(auto it=result.sources.find(e.location.file);it!=result.sources.end())attach_source(e.diagnostic,it->second);result.diagnostics.push_back(e.diagnostic);}
    } catch(Error& e) { result.diagnostics.push_back(e.diagnostic); }
    catch(const std::exception& e) { result.diagnostics.push_back(tooling_diagnostic(e.what())); }
    return result;
}
}
