#pragma once

#include <bolt/Ast.hpp>
#include <unordered_map>

namespace bolt::semantic {

struct Substitution : std::unordered_map<std::string,std::string> {
    using std::unordered_map<std::string,std::string>::unordered_map;
    std::unordered_map<std::string,std::vector<std::string>> type_packs, value_packs;
    Substitution without_parameters(const std::vector<std::string>& parameters, const std::string& pack) const;
};

bool mentions_type(const std::string& pattern,const std::string& name);
bool mentions_pack(const Expr& e,const std::string& name);
bool mentions_pack(const Stmt& s,const std::string& name);
bool expands_pack(const Stmt& s,const std::string& name);
std::string substitute(const std::string& t,const Substitution& map);
void substitute(Expr& e,const Substitution& map);
void substitute(Method& m,Substitution map);
void substitute(Class& instance, const Substitution& map);

} // namespace bolt::semantic
