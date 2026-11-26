#pragma once
#include "HostMethodEdits.hpp"
#include <bolt/CompilerHost.h>
#include <bolt/Value.hpp>
#include <bolt/Types.hpp>
#include <bolt/native/Jit.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <unordered_set>

namespace bolt::host {
// A source-set compilation retains the base image for literal substitutions
// and independent method objects. Every replacement links a fresh generation.
class FastEditTemplate {
    struct Slot {
        std::size_t offset, length;
        std::string symbol, type;
        int line, column;
    };
    struct Unit { std::string name; std::vector<Slot> slots; };
    std::vector<Unit> units;
    std::unordered_map<std::string,std::string> original;
    std::string entry;
    std::size_t files_checked;
    using Sites=decltype(ir::EditData::sites);
    Sites sites;
    std::unordered_map<std::string,HostMethodFile> method_files;
    std::optional<Program> method_world;
    std::shared_ptr<const ir::MethodEnvironment> method_environment;
    std::shared_ptr<const native_backend::FastRecipe> recipe;

    static bool digit(char c) { return c>='0' && c<='9'; }
    static bool decimal(std::string_view s) {
        return !s.empty() && digit(s.front()) &&
            std::all_of(s.begin(),s.end(),[](char c){return digit(c)||c=='.'||c=='u'||c=='l'||c=='f';});
    }
    // Only decimal digit groups may change. Keep punctuation and suffixes so
    // the lexer sees the same token boundaries and the literal parser checks
    // the actual numeric type and overflow again.
    static std::size_t decimal_extent(std::string_view original,std::string_view input) {
        std::size_t a=0,b=0;
        while(a<original.size()) {
            if(b==input.size()) return 0;
            if(digit(original[a])) {
                if(!digit(input[b])) return 0;
                while(a<original.size() && digit(original[a])) ++a;
                while(b<input.size() && digit(input[b])) ++b;
            } else if(original[a++]!=input[b++]) return 0;
        }
        return b;
    }
    static std::uint64_t bits(const Value& v) {
        if(auto n=std::get_if<float>(&v.data)) return std::bit_cast<std::uint32_t>(*n);
        if(auto n=std::get_if<double>(&v.data)) return std::bit_cast<std::uint64_t>(*n);
        if(auto n=std::get_if<std::uint64_t>(&v.data)) return *n;
        return static_cast<std::uint64_t>(v.as_int());
    }
    struct Operands {
        ir::EditData data;
        std::vector<Expr*> expressions;
        std::unordered_set<std::size_t> syntax_literals;

        void syntax(const Stmt& stmt) {
            if(stmt.kind==Stmt::Kind::Block || stmt.kind==Stmt::Kind::Declarations) {
                for(const auto& child:stmt.statements) syntax(*child);
                return;
            }
            if(!stmt.expression) return;
            const auto& e=*stmt.expression;
            auto record=[&](const Expr& operand) {
                if(operand.kind==Expr::Kind::Literal && operand.token.kind==Token::Kind::Number)
                    syntax_literals.insert(operand.token.at.offset);
            };
            if(stmt.kind==Stmt::Kind::Variable && !stmt.constant) record(e);
            if(stmt.kind==Stmt::Kind::Expression && e.kind==Expr::Kind::Assign && e.args.size()==2) record(*e.args[1]);
            if((stmt.kind==Stmt::Kind::Expression || stmt.kind==Stmt::Kind::Variable) && e.kind==Expr::Kind::Call)
                for(std::size_t i=1;i<e.args.size();++i) record(*e.args[i]);
        }
        void add(Expr& expr,Unit& unit,const std::string& source,const std::string& expected) {
            if(expr.kind!=Expr::Kind::Literal || expr.token.kind!=Token::Kind::Number ||
               expr.type!=expected || expr.token.at.file!=unit.name || expr.token.text!=expr.text ||
               !syntax_literals.contains(expr.token.at.offset) || !expr.constant_owner.empty() ||
               !decimal(expr.text) || expr.token.at.offset>=source.size() || source.compare(expr.token.at.offset,expr.text.size(),expr.text)!=0) return;
            auto value=literal_value(expr);
            if(value.type!=expected) return;
            const auto name="b_edit_value_"+std::to_string(expressions.size());
            expressions.push_back(&expr);
            data.slots.emplace(&expr,ir::EditData::Slot{name,bits(value)});
            data.sites[unit.name][expr.token.at.line];
            unit.slots.push_back({expr.token.at.offset,expr.text.size(),name,expr.type,expr.token.at.line,expr.token.at.column});
        }
        void collect(const Program& p,Stmt& stmt,Unit& unit,const std::string& source) {
            if(stmt.kind==Stmt::Kind::Block || stmt.kind==Stmt::Kind::Declarations) {
                for(auto& child:stmt.statements) collect(p,*child,unit,source);
                return;
            }
            if(!stmt.expression) return;
            auto& e=*stmt.expression;
            if(stmt.kind==Stmt::Kind::Expression && e.kind==Expr::Kind::Assign && e.args.size()==2 &&
               e.binding==Expr::Binding::None && (e.text=="="||e.text=="+="||e.text=="-="||e.text=="*=")) {
                auto& dest=*e.args[0];
                if(dest.binding==Expr::Binding::Field || dest.binding==Expr::Binding::Local)
                    add(*e.args[1],unit,source,dest.type);
            }
            if(stmt.kind==Stmt::Kind::Variable && !stmt.constant) add(e,unit,source,stmt.type);
            if((stmt.kind!=Stmt::Kind::Variable && stmt.kind!=Stmt::Kind::Expression) ||
               e.kind!=Expr::Kind::Call || e.binding!=Expr::Binding::Method || e.is_extension) return;
            auto* owner=p.find_class(e.owner);
            auto* method=owner?method_by_id(*owner,e.resolved):nullptr;
            if(!method || method->native_abi!="C" || !method->is_static || method->parameters.size()+1!=e.args.size()) return;
            if(std::count_if(owner->methods.begin(),owner->methods.end(),[&](const Method& m){return m.name==method->name;})!=1) return;
            for(std::size_t i=0;i<method->parameters.size();++i) add(*e.args[i+1],unit,source,method->parameters[i].type);
        }
        // Hide these literals from constant-folding and loop-pattern recognition
        // for the entire lowering pass. The analyzed AST remains reusable.
        struct Scope {
            Operands& operands;
            explicit Scope(Operands& o):operands(o){for(auto* e:o.expressions)e->kind=Expr::Kind::RuntimeOperand;}
            ~Scope(){for(auto* e:operands.expressions)e->kind=Expr::Kind::Literal;}
        };
    };
public:
    FastEditTemplate(const Program& p,const BoltSource* inputs,std::size_t count,std::size_t checked)
        :original(p.sources),entry(p.selected_entry),files_checked(checked) {
        for(std::size_t i=0;i<count;++i) units.push_back({inputs[i].name,{}});
    }
    std::shared_ptr<NativeProgram> compile(Program& p,const JitOptions& options,const HostFunctions& bindings,const ParseCache& cache) {
        Operands operands;
        std::unordered_map<std::string,std::unordered_map<std::size_t,std::size_t>> origins;
        for(const auto& c:p.classes) for(const auto& m:c.methods)
            if(m.body) ++origins[m.token.at.file][m.token.at.offset];
        for(auto& unit:units) {
            const auto& source=original.at(unit.name);
            if(source.find('#')!=source.npos || source!=p.active_sources.at(unit.name)) continue;
            const auto* syntax=cache.syntax(unit.name);
            if(!syntax) continue;
            if(auto plan=HostMethodFile::describe(*syntax,unit.name)) {
                if(const auto* owner=p.find_class(plan->owner);owner && owner->methods.size()==plan->bodies.size()) {
                    ir::ReadOnlyAnalysis effects(p);
                    for(const auto& m:owner->methods) if(effects.reads_only(m))plan->readonly_methods.insert(m.id);
                    method_files.emplace(unit.name,std::move(*plan));
                }
            }
            operands.syntax_literals.clear();
            for(const auto& c:syntax->classes) for(const auto& m:c.methods)
                if(m.body && !m.is_constant_template && !m.is_async && !c.is_interface &&
                   c.generic_parameters.empty() && c.pack_parameter.empty() && m.generic_parameters.empty() && m.pack_parameter.empty())
                    operands.syntax(*m.body);
            for(auto& c:p.classes) {
                if(c.token.at.file!=unit.name || c.is_template_member || c.is_interface ||
                   !c.generic_parameters.empty() || !c.pack_parameter.empty() || c.name.find('<')!=c.name.npos) continue;
                for(auto& m:c.methods) if(m.body && m.token.at.file==unit.name && origins.at(unit.name).at(m.token.at.offset)==1 && m.analyzed && !m.is_constant_template && !m.is_async &&
                    !m.is_default_implementation && !m.is_specialization && m.generic_parameters.empty() && m.pack_parameter.empty())
                    operands.collect(p,*m.body,unit,source);
            }
            std::sort(unit.slots.begin(),unit.slots.end(),[](const Slot& a,const Slot& b){return a.offset<b.offset;});
        }
        Operands::Scope scope(operands);
        auto image=std::make_shared<NativeProgram>(p,options,bindings,operands.data,recipe);
        std::unordered_set<std::string> used;
        for(const auto& [operand,slot]:operands.data.slots) if(slot.emitted) used.insert(slot.symbol);
        for(auto& unit:units) std::erase_if(unit.slots,[&](const Slot& s){return !used.contains(s.symbol);});
        method_environment=std::move(operands.data.methods);
        sites=std::move(operands.data.sites);
        return image;
    }
    bool accepts_methods() const {return !method_files.empty();}
    void retain(std::optional<Program> checked) {method_world=std::move(checked);}
    std::shared_ptr<NativeProgram> instantiate(const JitOptions& options,const BoltSource* inputs,std::size_t count,
                                                const char* selected,BoltCompileStats& stats,std::size_t& compiled_methods) {
        if(count!=units.size() || entry!=(selected?selected:"")) return {};
        const auto start=std::chrono::steady_clock::now();
        struct Replacement { const Slot* slot; std::uint64_t value; std::size_t length; };
        struct DiagnosticText { const std::string* symbol; std::string text; };
        std::vector<Replacement> values;
        std::vector<DiagnosticText> diagnostics;
        auto sources=original;
        const Unit* changed_methods=nullptr;
        for(std::size_t i=0;i<count;++i) {
            const auto& unit=units[i];
            if(unit.name!=inputs[i].name) return {};
            const std::string_view after(inputs[i].source,inputs[i].length);
            auto& before=sources.at(unit.name);
            if(before==after) continue;
            auto value_count=values.size(), site_count=diagnostics.size();
            auto match_operands=[&]() -> bool {
                std::size_t a=0,b=0;
                std::unordered_map<int,std::vector<Replacement>> changes;
                for(const auto& s:unit.slots) {
                    if(s.offset<a || after.substr(b,s.offset-a)!=std::string_view(before).substr(a,s.offset-a)) return false;
                    b+=s.offset-a;
                    const auto old=std::string_view(before).substr(s.offset,s.length);
                    const auto length=decimal_extent(old,after.substr(b));
                    if(!length) return false;
                    const auto text=after.substr(b,length);
                    a=s.offset+s.length;
                    b+=length;
                    if(text==old) continue;
                    Expr literal(Expr::Kind::Literal,Token{Token::Kind::Number,std::string(text),{}},std::string(text));
                    try {
                        auto value=literal_value(literal);
                        if(value.type!=s.type) return false;
                        Replacement change{&s,bits(value),length};
                        values.push_back(change);
                        changes[s.line].push_back(change);
                    } catch(const Error&) { return false; }
                }
                if(after.substr(b)!=std::string_view(before).substr(a)) return false;
                for(const auto& [line,edits]:changes) {
                    auto map_column=[&](std::int64_t column) {
                        auto result=column;
                        for(const auto& e:edits) {
                            const auto start=static_cast<std::int64_t>(e.slot->column);
                            const auto end=start+static_cast<std::int64_t>(e.slot->length);
                            if(column>=end) result+=static_cast<std::int64_t>(e.length)-(end-start);
                            else if(column>start) return std::int64_t{-1};
                        }
                        return result;
                    };
                    const auto line_text=source_line_at(after,line);
                    for(const auto& site:sites.at(unit.name).at(line)) {
                        const auto col=map_column(site.column);
                        const auto stop=map_column(static_cast<std::int64_t>(site.column)+site.length);
                        if(col<1 || stop<col || stop>INT32_MAX) return false;
                        auto text=std::to_string(line)+":"+std::to_string(col)+":"+std::to_string(stop-col)+":"+
                            std::to_string(unit.name.size())+":"+unit.name+line_text;
                        if(text.size()+1>site.capacity) return false;
                        diagnostics.push_back({&site.symbol,std::move(text)});
                    }
                }
                return true;
            };
            if(!match_operands()) {
                if(changed_methods || !method_world || !method_files.contains(unit.name) ||
                   after.find('#')!=after.npos)return {};
                changed_methods=&unit;
                values.resize(value_count);diagnostics.resize(site_count);
            }
            before.assign(after);
        }
        const auto checked=std::chrono::steady_clock::now();
        std::chrono::steady_clock::duration method_frontend{};
        std::shared_ptr<NativeProgram> result;
        if(changed_methods) {
            try {
                const auto& file=changed_methods->name;
                auto delta=method_files.at(file).compile(*method_world,file,sources.at(file),*method_environment,method_frontend);
                result=std::make_shared<NativeProgram>(options,std::move(sources),*recipe,&delta);
                compiled_methods=delta.functions.size();
            } catch(const std::exception&) {
                // A speculative bind can discover new dependencies. Discard its
                // semantic state and let the normal compiler produce diagnostics.
                method_world.reset();
                return {};
            }
        } else result=std::make_shared<NativeProgram>(options,std::move(sources),*recipe);
        for(const auto& change:values)
            std::memcpy(result->data_symbol(change.slot->symbol),&change.value,sizeof(change.value));
        for(const auto& change:diagnostics)
            std::memcpy(result->data_symbol(*change.symbol),change.text.c_str(),change.text.size()+1);
        using Ns=std::chrono::nanoseconds;
        stats={changed_methods?1u:0u,files_checked-(changed_methods?1u:0u),static_cast<uint64_t>(std::chrono::duration_cast<Ns>(checked-start+method_frontend).count()),
            static_cast<uint64_t>(std::chrono::duration_cast<Ns>(std::chrono::steady_clock::now()-checked-method_frontend).count())};
        return result;
    }
};
}
