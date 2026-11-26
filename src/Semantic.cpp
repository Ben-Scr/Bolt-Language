#include "semantic/Analyzer.hpp"
#include "GetterEffects.hpp"
#include <bolt/Frontend.hpp>
#include <bolt/Types.hpp>
#include <algorithm>

namespace bolt::semantic {

Analyzer::Analyzer(Program& p) : program(p) {
    classes_by_name.reserve(p.classes.size());
    enums_by_name.reserve(p.enums.size());
    for(auto& c:p.classes)classes_by_name.try_emplace(c.name,&c);
    for(auto& e:p.enums)enums_by_name.try_emplace(e.name,&e);
}

void Analyzer::recheck_all(const std::vector<Class*>& classes) {
    for(auto* c:classes) prepare(*c);
    for(auto* c:classes) for(auto& m:c->methods) analyze_method(*c,m);
}

void Analyzer::recheck(Class& c,Method& m) { analyze_method(c,m); }

void Analyzer::navigation_only() { navigation_bindings(); }

void Analyzer::run(bool require_entry,bool collect_references) {
    if(collect_references)navigation_bindings();
    validate_source_type_access(collect_references);
    validate_template_type_access();
    check_declarations_and_imports();

    enums();
    for (std::size_t i = 0; i < program.classes.size(); ++i) prepare(program.classes[i]);
    layouts();

    for (auto& c : program.classes)
        if (c.token.at.file=="<bolt-stdlib>/System/StandaloneCore.bolt" ||
            c.token.at.file=="<bolt-stdlib>/System/StandaloneStrings.bolt" ||
            (!c.is_native && c.token.at.file=="<bolt-stdlib>/System/PlatformMemoryOps.bolt")) {
            mark_type(c.name);
            for(auto& m:c.methods)m.referenced=true;
        }
    analyze_reachable_classes();

    // Body analysis can introduce enum and class specializations.
    enums();
    validate_conversion_cycles();
    layouts();
    if (require_entry) select_entry_point();
}

void Analyzer::check_declarations_and_imports() {
    for(auto& c:program.classes)if(!c.token.at.file.starts_with("<bolt-stdlib>")||c.name=="System.IStringable")c.reachable=true;
    std::unordered_set<std::string> names;
    for(const auto& c:program.classes)if((is_builtin_import(c.name)&&!c.is_native)||!names.insert(c.name).second)throw Error(c.token.at,"duplicate or reserved class name '"+c.name+"'", "BLT2000");
    for(const auto& e:program.enums)if(e.name=="Arena"||!names.insert(e.name).second)throw Error(e.token.at,"duplicate or reserved type name", "BLT2000");
    for(const auto& c:program.concepts)if(!names.insert(c.name).second)throw Error(c.token.at,"duplicate concept name", "BLT2000");
    for(const auto& i:program.imports) {
        if(is_builtin_import(i.name)||i.is_path)continue;
        bool found=namespace_exists(i.name);
        if(const auto* owner=find_class(i.name);owner&&std::any_of(owner->methods.begin(),owner->methods.end(),[](const Method& method){return method.is_extension;})) {
            require_type_access(owner->name,nullptr,i.token);
            found=true;
        }
        for(const auto& dep:program.dependencies) {
            auto stem=std::filesystem::path(dep).stem().string();
            if(stem==i.name)found=true;
        }
        if(!found)throw Error(i.token.at,"unknown import '"+i.name+"'; a namespace or extension owner must be supplied by a project source file", "BLT2000");
    }
}

void Analyzer::analyze_field_initializers(Class& c) {
    current_class=&c;
    current_method=nullptr;
    scopes.clear();
    for(auto& f:c.fields) {
        static_context=f.is_static;
        if(f.constant&&!f.initializer&&f.native_getter.empty())throw Error(f.token.at,"const fields need an initializer", "BLT2000");
        if(f.initializer) {
            expression(*f.initializer,false,f.type);
            convertible(f.type,*f.initializer);
            if(!pure(*f.initializer))throw Error(f.token.at,"field initializers must be pure literal/cast/arithmetic/constructor expressions", "BLT2000");
        }else if(f.is_static||std::none_of(c.methods.begin(),c.methods.end(),[](const auto& method){return method.is_constructor;}))default_constructible(f.type,f.token);
    }
}

void Analyzer::analyze_reachable_classes() {
    std::unordered_set<std::string> fields_done;
    bool progress=true;
    bool exception_dependencies=false;
    while(progress) {
        progress=false;
        // Direct catch matching registers the native exception hierarchy.
        // Prepare its ordinary defaults too, even when the only throw is
        // in a destructor and no catch mentions these otherwise-unused types.
        if(!exception_dependencies&&std::find(program.cpp_features.begin(),program.cpp_features.end(),"exception handling")!=program.cpp_features.end()) {
            exception_dependencies=true;
            for(std::size_t i=0;i<program.classes.size();++i) {
                auto& exception=program.classes[i];
                if(exception.is_native&&exception.token.at.file.find("Exceptions.bolt")!=std::string::npos)mark_type(exception.name);
            }
        }
        for(std::size_t i=0;i<program.classes.size();++i) {
            auto& c=program.classes[i];
            if((!c.generic_parameters.empty()||!c.pack_parameter.empty())||!emitted_class(c))continue;
            if(deferred_iteration_classes.contains(&c)&&!c.reachable)continue;
            prepare(c);
            if(!c.base.empty())mark_type(c.base);for(const auto& b:c.interfaces)mark_type(b);for(const auto& f:c.fields)mark_type(f.type);
            if(c.is_abstract&&!c.is_interface&&!c.is_native && c.token.at.file!="<bolt-stdlib>/System/IO/StandaloneStreams.bolt" && c.token.at.file!="<bolt-stdlib>/System/Ecs.bolt")program.require_cpp("abstract classes");
            if(!c.is_native&&(c.is_reflected||c.is_struct))program.require_cpp(c.is_reflected?"opt-in reflection":"struct value layout");
            if(c.is_native){for(auto& m:c.methods)if(m.prepared){for(auto& parameter:m.parameters)check_default(c,parameter);m.analyzed=true;}fields_done.insert(c.name);continue;}
            if(fields_done.insert(c.name).second) {
                if(c.token.at.file=="<bolt-stdlib>/System/Ownership.bolt" && generic_base(c.name)=="System.Memory.Owner")
                    for(auto& method:c.methods)if(method.name=="CopyValue")method.referenced=true;
                if(c.name=="Bolt.Runtime.ThreadHandle")mark_type("Bolt.Runtime.ThreadKernel");
                if(c.name=="Bolt.Runtime.ThreadKernel")for(auto& method:c.methods)method.referenced=true;
                analyze_field_initializers(c);
                progress=true;
            }
            for(const auto& a:c.attributes)if(a.name=="SerializeValue") {
                if(a.arguments.size()!=2)throw Error(a.token.at,"SerializeValue requires export and import method names", "BLT2410");
                for(auto& method:c.methods)if(method.name==a.arguments[0]||method.name==a.arguments[1])method.referenced=true;
            }
            for(const auto& a:c.attributes)if(a.name=="SerializeSequence"||a.name=="SerializeMap") {
                if(a.arguments.size()!=3)throw Error(a.token.at,"serialization collection contract requires count, element accessor and insertion member names", "BLT2410");
                for(auto& m:c.methods)if(m.name==a.arguments[1]||m.name==a.arguments[2]||m.property_name==a.arguments[0])m.referenced=true;
            }
            for(std::size_t j=0;j<c.methods.size();++j) {
                auto& m=c.methods[j];
                if(deferred_iteration_methods.contains(&m)&&!m.referenced)continue;
                if(!m.analyzed&&m.generic_parameters.empty()&&m.pack_parameter.empty()&&
                   (!c.token.at.file.starts_with("<bolt-stdlib>")||m.referenced||m.is_constructor||m.is_virtual||m.is_destructor||c.is_reflected||&m==equality_contract(c)||&m==hash_contract(c))) {
                    analyze_method(c,m);
                    progress=true;
                }
            }
        }
    }
}

void Analyzer::select_entry_point() {
    Class* entry=nullptr;
    std::string selected=program.selected_entry;
    if(selected=="Main")selected.clear();
    if(selected.ends_with(".Main"))selected.resize(selected.size()-5);
    for(auto& c:program.classes)if((selected.empty()?c.short_name=="Program":(c.name==selected||c.short_name==selected))&&(c.generic_parameters.empty()&&c.pack_parameter.empty())&&std::find(program.external_sources.begin(),program.external_sources.end(),c.token.at.file)==program.external_sources.end()) {
        if(entry)throw Error(c.token.at,"multiple entry classes; set project.entry to a qualified class Main", "BLT2000");
        entry=&c;
    }
    if(!entry)throw Error( {
        "<program>",1,1
    },"entry class Program is missing", "BLT2000");
    const Method* main=nullptr;
    for(const auto& m:entry->methods)if(m.name=="Main"&&(m.parameters.empty()||(m.parameters.size()==1&&m.parameters[0].type=="string[]"&&!m.parameters[0].variadic))&&m.generic_parameters.empty()&&m.pack_parameter.empty()) {
        if(main)throw Error(m.token.at,"ambiguous Program.Main", "BLT2000");
        main=&m;
    }
    const bool task_main=main&&main->is_async&&generic_base(main->type)=="System.Threading.Tasks.Task"&&(generic_args(main->type)==std::vector<std::string>{"void"}||generic_args(main->type)==std::vector<std::string>{"int"});
    if(!main||!main->is_static||!main->is_public||(main->type!="void"&&main->type!="int"&&!task_main))throw Error(entry->token.at,"entry must be public static void/int Main() or Main(string[] args); async Task/Task<int> Main is also supported", "BLT2000");
    program.entry_class=entry->name;program.entry_accepts_arguments=!main->parameters.empty();
    if(program.entry_accepts_arguments)program.require_cpp("command-line entry arguments");
}

} // namespace bolt::semantic

namespace bolt {

void recheck_host_classes(Program& p,const std::vector<Class*>& classes) { semantic::Analyzer(p).recheck_all(classes); }
void recheck_host_method(Program& p,Class& c,Method& m) { semantic::Analyzer(p).recheck(c,m); }
void bind_navigation(Program& program) { semantic::Analyzer(program).navigation_only(); }
static void analyze_program(Program& program,bool require_entry,bool collect_references) {
    add_standard_library(program);
    infer_getter_receivers(program);
    semantic::Analyzer(program).run(require_entry,collect_references);
}
void analyze(Program& program,bool require_entry) {
    analyze_program(program,require_entry,true);
}
void analyze_for_execution(Program& program,bool require_entry) {
    try { analyze_program(program,require_entry,false); }
    catch(Error& error) {
        if(auto it=program.sources.find(error.location.file);it!=program.sources.end())
            attach_source(error.diagnostic,it->second);
        throw;
    }
}

} // namespace bolt
