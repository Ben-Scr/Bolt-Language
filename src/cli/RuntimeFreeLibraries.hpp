#pragma once
#include <bolt/native/IR.hpp>
#include <bolt/Project.hpp>
#include <bolt/Path.hpp>
#include "../project/BuildRoot.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace bolt::project::freestanding {
using namespace ir;

struct LibraryImage {
    const Model* project;
    Module module;
    std::set<std::size_t> dependencies;
};

// Append a pointer-producing instruction, preserving the original SSA register numbers.
inline Reg pointer(Function& f, std::vector<Instruction>& code, Instruction op) {
    op.result=static_cast<Reg>(f.registers.size());
    f.registers.push_back(Type::Ptr);
    code.push_back(std::move(op));
    return code.back().result;
}

struct Symbols {
    std::unordered_map<std::string,External> functions;
    std::unordered_map<std::string,const Global*> globals;
    std::unordered_map<std::string,std::size_t> owners;
    std::set<std::string> foreign;
};

inline void copy_literal(const Global& literal,const Symbols& symbols,Module& image,std::set<std::string>& copied) {
    if(!copied.insert(literal.name).second)return;
    for(const auto& ref:literal.relocations)copy_literal(*symbols.globals.at(ref.symbol),symbols,image,copied);
    image.globals.push_back(literal);
}

// OS calls and calls down the dependency graph use ordinary linker imports. References
// back into the application (including virtual overrides) and canonical storage use
// slots installed before Bolt static initialization. This also avoids cyclic DLL imports.
inline void link_methods(LibraryImage& lib,std::size_t index,const Symbols& symbols,
                         std::set<std::string>& bindings) {
    auto& image=lib.module;
    std::set<std::string> imported,literals;
    for(auto& fn:image.functions)for(auto& block:fn.blocks) {
        std::vector<Instruction> code;
        for(auto& op:block.code) {
            if(op.op==Op::Address || op.op==Op::Call){
                const auto& name=op.text;
                const auto owner=symbols.owners.find(name);
                if(auto data=symbols.globals.find(name);data!=symbols.globals.end() && name.starts_with("b_string_")){
                    copy_literal(*data->second,symbols,image,literals);
                }else if((owner!=symbols.owners.end() && lib.dependencies.contains(owner->second)) ||
                         (symbols.foreign.contains(name) && name!="bolt_diagnostic_backtrace")){
                    if(imported.insert(name).second)image.externals.push_back(symbols.functions.at(name));
                }else if(owner==symbols.owners.end() || owner->second!=index){
                    bindings.insert(name);
                    const auto slot=pointer(fn,code,{.op=Op::Address,.text="bolt_slot_"+name,.at=op.at});
                    if(op.op==Op::Address){
                        op.op=Op::Load;op.text.clear();op.args={slot};
                    }else{
                        const auto target=pointer(fn,code,{.op=Op::Load,.args={slot},.at=op.at});
                        op.op=Op::CallIndirect;op.text.clear();op.storage_move_stride=0;
                        op.args.insert(op.args.begin(),target);
                    }
                }
            }
            code.push_back(std::move(op));
        }
        block.code=std::move(code);
    }
}

// The name also identifies the ordered, typed binding table. The system loader
// diagnoses an incompatible library before any user initializer can touch its data.
inline void bind_library(LibraryImage& lib,Module& app,Function* entry,const Symbols& symbols,
                         const std::set<std::string>& bindings) {
    Function bind;
    bind.exported=true;bind.parameters={Type::Ptr};bind.registers.push_back(Type::Ptr);
    auto& code=bind.blocks.emplace_back().code;
    Global table;table.alignment=8;
    std::string schema="Bolt shared source ABI 1\n"+lib.project->name+"\n";
    for(const auto& name:bindings){
        schema+=name+" ";
        if(auto it=symbols.functions.find(name);it!=symbols.functions.end()){
            schema+=ir::name(it->second.result);
            for(auto t:it->second.parameters)schema+=" "+std::string(ir::name(t));
        }
        schema+="\n";
        auto offset=table.relocations.size()*8;
        table.relocations.push_back({offset,name,0});
        const auto slot="bolt_slot_"+name;
        lib.module.globals.push_back({slot,std::vector<std::uint8_t>(8),true,8,{}});
        const auto address=pointer(bind,code,{.op=Op::Offset,.args={1},.immediate=offset});
        const auto value=pointer(bind,code,{.op=Op::Load,.args={address}});
        const auto storage=pointer(bind,code,{.op=Op::Address,.text=slot});
        code.push_back({.op=Op::Store,.args={storage,value}});
    }
    code.push_back({.op=Op::Return});
    bind.name="bolt_bind_"+build_paths::hash_text(schema);
    if(entry){
        table.name=bind.name+"_table";table.bytes.resize(std::max<std::size_t>(1,bindings.size())*8);
        std::vector<Instruction> setup;
        const auto data=pointer(*entry,setup,{.op=Op::Address,.text=table.name,.at=entry->at});
        setup.push_back({.op=Op::Call,.args={data},.text=bind.name,.at=entry->at});
        auto& start=entry->blocks.front().code;
        start.insert(start.begin(),setup.begin(),setup.end());
        app.globals.push_back(std::move(table));
        app.externals.push_back({bind.name,Type::Void,{Type::Ptr}});
    }
    lib.module.functions.push_back(std::move(bind));
}

inline std::vector<LibraryImage> split(Module& app,const Graph& graph) {
    std::vector<LibraryImage> images;
    std::unordered_map<std::string,std::size_t> sources;
    std::map<fs::path,std::set<std::size_t>> closure;
    auto add=[&](const Model& model){
        auto& reachable=closure[model.file];
        for(const auto& [alias,manifest]:model.dependencies){
            const auto& deps=closure.at(fs::weakly_canonical(manifest));
            reachable.insert(deps.begin(),deps.end());
        }
        if(model.kind!="shared-library")return;
        auto index=images.size();
        for(const auto& file:model.sources)sources.emplace(path_to_utf8(file),index);
        images.push_back({&model,{},reachable});
        images.back().module.runtime_free_library=true;
        reachable.insert(index);
    };
    for(const auto& dep:graph.dependencies)add(dep);
    add(graph.main);

    Symbols symbols;
    for(const auto& g:app.globals)symbols.globals.emplace(g.name,&g);
    for(const auto& e:app.externals){symbols.functions.emplace(e.name,e);symbols.foreign.insert(e.name);}
    std::vector<Function> retained;
    for(auto& f:app.functions){
        External signature{f.name,f.result,f.parameters};
        symbols.functions.emplace(f.name,signature);
        auto owner=sources.find(f.at.file);
        if(owner==sources.end() || f.exported){
            retained.push_back(std::move(f));
        }else{
            symbols.owners.emplace(f.name,owner->second);
            f.exported=true;
            images[owner->second].module.functions.push_back(std::move(f));
            app.externals.push_back(std::move(signature));
        }
    }
    app.functions=std::move(retained);
    std::vector<std::set<std::string>> bindings(images.size());
    for(std::size_t i=0;i<images.size();++i)link_methods(images[i],i,symbols,bindings[i]);

    auto entry=std::find_if(app.functions.begin(),app.functions.end(),[](const auto& f){return f.name=="bolt_program_entry";});
    for(std::size_t i=0;i<images.size();++i)
        bind_library(images[i],app,entry==app.functions.end()?nullptr:&*entry,symbols,bindings[i]);
    return images;
}
} // namespace bolt::project::freestanding
