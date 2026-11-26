#pragma once
#include <bolt/Ast.hpp>
#include <bolt/Types.hpp>
#include <bolt/native/Library.hpp>
#include <algorithm>
#include <unordered_map>

namespace bolt::ir {

// A final by-value string argument can borrow the caller's bytes when the
// entire statically resolved call cannot change or expose any managed value.
// Unknown operations, including user equality/hashing and virtual dispatch,
// require the ordinary owning snapshot.
class ReadOnlyAnalysis {
    using K = Expr::Kind;
    using B = Expr::Binding;
    const Program& program_;
    std::unordered_map<const Method*, bool> methods_;

    static bool scalar(const std::string& type) {
        return is_numeric(type) || type == "bool" || type == "char";
    }
    bool call(const Expr& e) {
        if (e.args.empty()) return false;
        if (e.binding == B::Cast)
            return e.args.size() == 2 && scalar(e.type) && scalar(e.args[1]->type) && expression(*e.args[1]);
        if (e.binding != B::Method) return false;
        auto* cls = program_.find_class(e.owner);
        const auto* method = cls ? method_by_id(*cls, e.resolved) : nullptr;
        if (!method || e.args.size() != method->parameters.size() + 1) return false;
        const auto& target = *e.args.front();
        if (!method->is_static && target.kind == K::Member &&
            (target.args.size() != 1 || !expression(*target.args[0]))) return false;
        for (std::size_t i = 0; i < method->parameters.size(); ++i) {
            const auto& type = method->parameters[i].type;
            if (!is_reference(type) && !scalar(type) && type != "string") return false;
            if (!expression(*e.args[i + 1])) return false;
        }
        if (cls->is_native) {
            if (cls->native_cpp == "bolt::platform::Failure")
                return method->type == "void" && method->parameters.size() == 1 && method->parameters[0].type == "string";
            if (cls->native_cpp != "bolt::platform::Values" ||
                !native_abi::library_primitive_method(*cls, *method) ||
                (method->name != "Hash" && method->name != "Equal")) return false;
            return e.args.size() > 1 && (scalar(e.args[1]->type) || e.args[1]->type == "string");
        }
        return reads_only(*method);
    }
    bool expression(const Expr& e) {
        if (e.arena) return false;
        switch (e.kind) {
        case K::Literal: break;
        case K::Name: case K::Member:
            if (e.binding == B::Field && !e.is_static) break;
            if (e.binding == B::Local || e.binding == B::This || e.binding == B::EnumValue) break;
            if (e.binding == B::Property && (is_array(e.owner) || is_collection(e.owner)) &&
                (e.text == "Length" || e.text == "Count" || e.text == "Capacity" || e.text == "Rank")) break;
            return false;
        case K::Index:
            if (e.binding != B::None || (!is_array(e.owner) && collection_kind(e.owner) != "List" && e.owner != "string"))
                return false;
            break;
        case K::Call: return call(e);
        case K::Unary: case K::Binary:
            if (e.kind == K::Binary && e.binding == B::None && (e.text == "==" || e.text == "!=") &&
                e.args.size() == 2 && e.args[0]->type == "string" && e.args[1]->type == "string") break;
            if (e.binding != B::None || !scalar(e.type) ||
                !std::all_of(e.args.begin(), e.args.end(), [](const auto& a) { return scalar(a->type); })) return false;
            // Increments (including prefix) mutate their operand.
            if (e.text == "++" || e.text == "--") return false;
            break;
        case K::Assign: case K::Postfix: {
            if (e.binding != B::None || e.args.empty()) return false;
            const auto& lhs = *e.args[0];
            if (lhs.kind != K::Name || lhs.binding != B::Local || is_reference(lhs.declared_type) || !scalar(lhs.type)) return false;
            break;
        }
        default: return false;
        }
        return std::all_of(e.args.begin(), e.args.end(), [&](const auto& a) { return expression(*a); });
    }
    bool statement(const Stmt& s) {
        using S = Stmt::Kind;
        switch (s.kind) {
        case S::Variable:
            if (!scalar(s.type) && !(s.constant && is_reference(s.type) && s.expression && s.expression->lvalue)) return false;
            break;
        case S::Block: case S::Declarations: case S::Expression: case S::If:
        case S::While: case S::For: case S::Return: case S::Break: case S::Continue: case S::Empty: break;
        default: return false;
        }
        for (auto* arg : {s.expression.get(), s.condition.get(), s.increment.get()})
            if (arg && !expression(*arg)) return false;
        for (auto* child : {s.init.get(), s.body.get(), s.alternative.get()})
            if (child && !statement(*child)) return false;
        return std::all_of(s.statements.begin(), s.statements.end(), [&](const auto& child) { return statement(*child); });
    }

public:
    explicit ReadOnlyAnalysis(const Program& p) : program_(p) {}
    bool reads_only(const Stmt& region) { return statement(region); }
    bool reads_only(const Method& method) {
        if (method.is_virtual || method.is_extern || method.is_constructor || method.is_destructor ||
            method.is_async || !method.body) return false;
        auto result = referred(method.type);
        if (result != "void" && result != "string" && !scalar(result)) return false;
        if (const auto found = methods_.find(&method); found != methods_.end()) return found->second;
        // Re-entering the same method conservatively rejects recursion.
        methods_.emplace(&method, false);
        const bool readonly = statement(*method.body);
        methods_[&method] = readonly;
        return readonly;
    }
};
}
