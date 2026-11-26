#pragma once
#include "Ast.hpp"
#include "Types.hpp"
#include <set>

namespace bolt {
    // The compiler and editor share declaration visibility. Namespace prefixes do
    // not confer access: only actual lexical type owners do.
    inline std::string type_access_error(const Program& program, std::string name,
                                         const std::string& source_file,
                                         const std::string& accessing_type = {}) {
        while(is_reference(name)||is_pointer(name)||is_array(name)||is_optional(name))
            name=element_type(name);
        std::set<std::string> checked;
        while(!name.empty()&&checked.insert(name).second) {
            const auto* c=program.find_class(name);
            const auto* e=program.find_enum(name);
            if(!c&&!e) {
                auto base=generic_base(name);
                if(base==name)base=generic_declaration_name(name);
                if(base==name)break;
                name=base;
                continue;
            }
            const auto& owner=c?c->declaring_type:e->declaring_type;
            const auto& declaration=c?c->token:e->token;
            const bool is_public=c?c->is_public:e->is_public;
            if(!is_public) {
                if(owner.empty()) {
                    if(source_file!=declaration.at.file) {
                        const auto separator=declaration.at.file.find_last_of("/\\");
                        const auto filename=declaration.at.file.substr(separator==std::string::npos?0:separator+1);
                        return "Type '"+name+"' is private to its declaring file '"+filename+"'.";
                    }
                }else {
                    bool inside=false;
                    std::set<std::string> visited;
                    for(auto context=accessing_type;!context.empty()&&visited.insert(context).second;) {
                        if(context==owner||generic_base(context)==owner){inside=true;break;}
                        auto* scope=program.find_class(context);
                        if(!scope)scope=program.find_class(generic_base(context));
                        if(!scope)scope=program.find_class(generic_declaration_name(context));
                        if(!scope)break;
                        context=scope->declaring_type;
                    }
                    if(!inside)return "Type '"+name+"' is private to its declaring type '"+owner+"'.";
                }
            }
            name=owner;
        }
        return {};
    }

    inline bool type_accessible(const Program& program, const std::string& name,
                                const std::string& source_file,
                                const std::string& accessing_type = {}) {
        return type_access_error(program,name,source_file,accessing_type).empty();
    }
}
