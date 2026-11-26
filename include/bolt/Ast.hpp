#pragma once
#include "Diagnostics.hpp"
#include "Preprocessor.hpp"
#include "Types.hpp"
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
namespace bolt {
    struct Token {
        enum class Kind {
            Identifier, Number, String, Character, Symbol, End
        }
        kind = Kind::End;
        std::string text;
        Location at;
    };
    struct Expr;
    struct Stmt;
    using ExprPtr = std::unique_ptr<Expr>;
    using StmtPtr = std::unique_ptr<Stmt>;
    struct LambdaParameter {
        Token name;
        std::string type;
    };
    struct LambdaSyntax {
        std::vector<LambdaParameter> parameters;
        StmtPtr body;
        bool expression_body = false;
        std::vector<std::string> captures;
        std::vector<std::string> free_names;
    };
    struct Expr {
        enum class Kind {
            Literal, Name, Member, Call, Unary, Binary, Assign, Postfix, Index, Array, New, Delete, Pack, Default, Conditional, Let, OptionalHas, OptionalValue, SizeOf, Lambda, RuntimeOperand
        }
        kind;
        enum class Binding {
            None, Local, This, Field, Method, Class, Builtin, Cast, Construct, Namespace, Enum, EnumValue, Property, Collection, Operator, UserProperty, Event
        }
        binding = Binding::None;
        Token token;
        std::string text;
        // Placement allocation receiver; evaluated before constructor arguments.
        ExprPtr arena;
        std::unique_ptr<LambdaSyntax> lambda;
        std::vector<ExprPtr> args;
        // Rectangular array literals retain their shape after flattening elements.
        std::vector<std::size_t> dimensions;
        std::vector<std::string> type_args;
        std::string callback_owner, callback_method;
        std::string type, owner, resolved, declared_type;
        // Retain the source reference after expanding a generic constant initializer.
        std::string constant_owner;
        Token constant_token;
        std::size_t local_id = 0;
        bool lvalue = false, constant = false, is_static = false, arrow = false;
        // Extension syntax lowers to a static call; retain its source form for tooling.
        bool is_extension = false;
        // Only semantic lowering can access compiler-owned event storage.
        bool authorized_event_access = false;
        Expr(Kind k, Token t, std::string value = {
        }) : kind(k), token(std::move(t)), text(std::move(value)) {
        }
    };
    struct Stmt {
        enum class Kind {
            Block, Declarations, Variable, Expression, If, While, For, ForEach, Try, Catch, Throw, Return, Break, Continue, Empty, Switch, Case, SwitchScope
        }
        kind;
        Token token;
        std::string type, name;
        Token name_token;
        bool constant = false;
        std::size_t local_id = 0;
        ExprPtr expression, condition, increment;
        StmtPtr init, body, alternative;
        std::vector<StmtPtr> statements;
        Stmt(Kind k, Token t) : kind(k), token(std::move(t)) {
        }
    };
    struct Attribute {
        std::string name;
        std::vector<std::string> arguments;
        Token token;
    };
    std::string attribute_text(const std::vector<Attribute>& attributes);
    struct Parameter {
        std::string type, name;
        Token token, name_token;
        bool constant = false, variadic = false, pack = false;
        bool is_this = false;
        std::shared_ptr<Expr> default_value;
        std::size_t local_id = 0;
    };
    struct Constraint {
        std::string parameter;
        std::vector<std::string> concepts;
        Token token;
    };
    struct Field {
        std::string type, name, native_getter, event_type;
        bool is_event = false;
        std::vector<Attribute> attributes;
        Token token;
        bool is_public = false, is_protected = false, is_static = false, constant = false;
        ExprPtr initializer;
    };
    struct Method {
        std::string type, name, id, base_constructor, operator_symbol;
        Token token;
        bool is_public = false, is_protected = false, is_static = false, is_constructor = false, is_sealed = false, is_override = false, is_virtual = false;
        bool referenced = false;
        bool prepared = false, analyzed = false, analyzing = false, is_specialization = false, is_abstract = false;
        bool is_constant_template = false;
        bool is_destructor = false, is_async = false, is_extern = false;
        bool is_extension = false;
        // Conversion members are ordinary zero-argument methods after binding.
        bool is_conversion = false, is_implicit = false, is_default_implementation = false;
        std::string default_owner;
        bool native_pack = false; std::size_t native_pack_min = 0;
        std::string native_name, native_abi;
        std::string property_name;
        bool is_getter = false, is_setter = false, readonly_receiver = false;
        std::vector<std::string> specialization_types;
        std::vector<Attribute> attributes;
        std::vector<std::string> generic_parameters;
        std::string pack_parameter;
        std::vector<Constraint> constraints;
        std::vector<Parameter> parameters;
        std::vector<ExprPtr> base_arguments;
        std::vector<ExprPtr> constructor_initializers;
        std::vector<std::pair<std::string,ExprPtr>> field_initializers;
        bool explicit_base = false;
        StmtPtr body;
    };
    struct Property {
        std::string type, name, getter, setter, backing_field;
        Token token;
        bool is_public = false, is_protected = false, is_static = false, is_indexer = false;
        std::vector<Parameter> parameters;
        std::vector<Attribute> attributes;
    };
    struct Class {
        std::string name, short_name, namespace_name, base;
        // Lexical type owner; nested declarations remain in Program's flat tables.
        std::string declaring_type;
        bool is_public = true;
        bool is_template_member = false;
        Token token;
        bool is_static = false, is_sealed = false, is_global = false, is_interface = false, prepared = false, preparing = false;
        bool reachable = false;
        bool is_struct = false, is_abstract = false, is_reflected = false, is_native = false;
        std::string native_cpp;
        // Compiler-generated delegate adapter can access only its bound handler.
        std::string delegate_owner, delegate_method;
        std::vector<Attribute> attributes;
        std::vector<std::string> interfaces;
        std::vector<std::string> imports, generic_parameters, generic_defaults;
        std::string pack_parameter;
        std::vector<Constraint> constraints;
        std::vector<Field> fields;
        std::vector<Property> properties;
        std::deque<Method> methods;
    };
    inline bool emitted_class(const Class& c) { return !c.is_template_member && (!c.token.at.file.starts_with("<bolt-stdlib>") || c.reachable || c.name=="System.IStringable"); }
    struct EnumMember {
        std::string name;
        Token token;
        ExprPtr expression;
        std::string value;
    };
    struct Enum {
        std::string name, short_name, namespace_name, underlying = "int", native_cpp;
        std::string declaring_type;
        bool is_public = true;
        bool is_template_member = false, prepared = false;
        Token token;
        std::vector<std::string> imports;
        std::vector<EnumMember> members;
        std::vector<Attribute> attributes;
    };
    struct Requirement {
        std::vector<Parameter> parameters;
        ExprPtr expression;
        std::string result_type;
        Token token;
    };
    struct Concept {
        std::string name, parameter, namespace_name;
        std::vector<std::string> parameters;
        std::vector<Requirement> requirements;
        std::vector<std::string> imports;
        Token token;
        ExprPtr expression;
    };
    struct Import {
        std::string name;
        Token token;
        bool is_path = false;
    };
    // Physical type syntax retained by the parser, including nested arguments.
    // Speculative expression parses must discard these alongside their tokens.
    struct SourceType {
        Token token;
        std::string name, owner, namespace_name;
        std::vector<std::string> imports;
        bool concept_name = false;
    };
    struct SemanticReference {
        Token token;
        std::string symbol, name, category, detail, owner;
        Location declaration;
        bool ambiguous = false;
    };
    struct Program {
        std::vector<SourceType> source_types;
        std::vector<SemanticReference> semantic_references;
        // deque preserves addresses while generic instantiation appends declarations.
        std::deque<Class> classes;
        std::deque<Enum> enums;
        std::vector<Concept> concepts;
        std::vector<Import> imports;
        std::vector<std::string> dependencies;
        std::unordered_map<std::string, std::string> sources;
        std::unordered_map<std::string, std::string> active_sources;
        PreprocessorOptions preprocessor;
        std::unordered_map<std::string, PreprocessorOptions> source_preprocessors;
        std::vector<std::size_t> layout_order;
        std::string entry_class;
        std::string selected_entry, project_identity;
        bool prelude_enabled=true, library_mode=false;
        std::vector<std::string> external_sources;
        bool entry_accepts_arguments = false;
        std::vector<std::string> cpp_features, cpp_headers;
        void require_cpp(const std::string& feature) {
            for(const auto& f:cpp_features)if(f==feature)return;
            cpp_features.push_back(feature);
        }
        // Automatically imported ordinary standard-library static classes.
        std::vector<std::string> implicit_static_imports;
        bool standard_library_loaded=false;
        Class* find_class(const std::string& name) {
            for (auto& c : classes) if (c.name == name) return &c;
            return nullptr;
        }
        const Class* find_class(const std::string& name) const {
            for (const auto& c : classes) if (c.name == name) return &c;
            return nullptr;
        }
        Enum* find_enum(const std::string& name) {
            for (auto& e : enums) if (e.name == name) return &e;
            return nullptr;
        }
        const Enum* find_enum(const std::string& name) const {
            for (const auto& e : enums) if (e.name == name) return &e;
            return nullptr;
        }
    };
    inline const Field* find_field(const Class& c, const std::string& name) {
        for (const auto& f : c.fields) if (f.name == name) return &f;
        return nullptr;
    }
    inline const Property* find_property(const Class& c, const std::string& name) {
        for (const auto& p : c.properties) if (p.name == name) return &p;
        return nullptr;
    }
    inline const Method* find_method(const Class& c, const std::string& name) {
        for (const auto& m : c.methods) if (m.name == name && m.generic_parameters.empty() && m.pack_parameter.empty()) return &m;
        return nullptr;
    }
    inline constexpr char float_formatting_class[] = "Bolt.Runtime.FloatFormatting";
    inline const char* float_formatting_method(const std::string& type) {
        return type=="float" ? "Single" : "Double";
    }
    struct PrimitiveFormatter {
        const char* owner;
        const char* method;
    };
    using NumericFormatter = PrimitiveFormatter;
    inline NumericFormatter numeric_formatter(const std::string& type) {
        if(is_floating(type))return {float_formatting_class,float_formatting_method(type)};
        return {"Bolt.Runtime.IntegerFormatting",is_unsigned(type) ? "Unsigned" : "Signed"};
    }
    inline PrimitiveFormatter primitive_formatter(const std::string& type) {
        if(type=="bool")return {"Bolt.Runtime.PrimitiveFormatting","Boolean"};
        if(type=="char")return {"Bolt.Runtime.PrimitiveFormatting","Character"};
        if(is_numeric(type))return numeric_formatter(type);
        return {};
    }
    inline const Method* method_by_id(const Class& c, const std::string& id) {
        for (const auto& m : c.methods) if (m.id == id) return &m;
        return nullptr;
    }
    // Optional logical value contracts. The same ordinary Bolt methods are used by
    // collection keys, nested values and generated compatibility objects.
    inline const Method* equality_contract(const Class& c) {
        for (const auto& m:c.methods) if(m.is_public && m.is_static && m.operator_symbol=="==" && m.type=="bool" && m.parameters.size()==2 && m.generic_parameters.empty()) {
            bool valid=true; for(const auto& a:m.parameters) valid &= a.type==c.name || (a.type==c.name+"&" && a.constant);
            if(valid)return &m;
        }
        return nullptr;
    }
    inline const Method* hash_contract(const Class& c) {
        for(const auto& m:c.methods)if(m.is_public&&!m.is_static&&m.name=="GetHashCode"&&m.type=="ulong"&&m.parameters.empty()&&m.generic_parameters.empty()&&m.readonly_receiver)return &m;
        return nullptr;
    }
    // Nominal class/interface subtyping. A visited set also makes malformed input safe.
    inline bool is_interface(const Program& p,const std::string& name) {
        const auto* c=p.find_class(name); return c&&c->is_interface;
    }
    bool derives_from(const Program&,const std::string& from,const std::string& to);
    bool stringable_type(const Program&,const std::string& type);
    ExprPtr clone(const Expr& e);
    StmtPtr clone(const Stmt& s);
    Method clone(const Method& m);
    Class clone(const Class& c);
}
// namespace bolt
