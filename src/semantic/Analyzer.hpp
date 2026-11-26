#pragma once

#include <bolt/Ast.hpp>
#include <map>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace bolt { class Value; }

namespace bolt::semantic {

struct Substitution;

class Analyzer {
public:
    explicit Analyzer(Program& p);
    void recheck_all(const std::vector<Class*>& classes);
    void recheck(Class& c,Method& m);
    void navigation_only();
    void run(bool require_entry,bool collect_references);

private:
    struct Symbol {
        std::string type;
        bool constant=false;
        std::size_t id=0;
        bool parameter=false;
        bool local_arena_alias=false;
        const Expr* constant_value=nullptr;
    };
    Program& program;
    // Declarations keep their addresses in Program's deques. Index new
    // specializations here as well as the declarations present at entry.
    std::unordered_map<std::string,Class*> classes_by_name;
    std::unordered_map<std::string,Enum*> enums_by_name;
    std::map<std::string,std::string> delegate_adapters;
    std::set<std::pair<std::string,std::size_t>> dependent_source_types;
    std::set<std::pair<std::string,std::size_t>> resolving_defaults;
    Class* current_class=nullptr;
    Method* current_method=nullptr;
    std::span<const std::string> signature_arguments;
    struct SignatureArguments {
        std::span<const std::string>& slot;
        std::span<const std::string> previous;
        SignatureArguments(Analyzer& analyzer,std::span<const std::string> arguments)
            : slot(analyzer.signature_arguments),previous(slot) { slot=arguments; }
        ~SignatureArguments() { slot=previous; }
    };
    bool static_context=false;
    int loops=0, catch_depth=0, finally_depth=0;
    int switches=0;
    std::size_t expression_depth=0,next_local=0,instantiations=0;
    std::vector<std::unordered_map<std::string,Symbol>> scopes;
    std::unordered_map<std::string,bool> concept_active;
    // Failed iteration checks may prepare generic signatures, but must not
    // cause otherwise-unused specialization bodies to become executable.
    std::unordered_set<const Method*> deferred_iteration_methods;
    std::unordered_set<const Class*> deferred_iteration_classes;
    using Facts=std::set<std::size_t>;
    struct FlowState {
        Facts present, escaped;
        std::unordered_map<std::size_t,std::size_t> aliases;
        std::unordered_map<const Expr*,std::pair<Facts,Facts>> conditions;
    } flow;
    const Expr* arena_initializer=nullptr;
    struct DepthGuard {
        Analyzer& a;
        DepthGuard(Analyzer& x,const Location& at):a(x) {
            if(++a.expression_depth>256)throw Error(at,"expression nesting limit exceeded", "BLT2000");
        }
        ~DepthGuard() {
            --a.expression_depth;
        }
    };
    struct UserConversion { Class* owner; Method* method; int cost; bool constructor=false; };
    struct IterationElement {std::string type;bool lvalue=false;bool constant=false;};
    std::size_t property_temporary=0;

    // Program analysis
    void check_declarations_and_imports();
    void specialize_metadata(Class& owner, Method& method);
    bool generated_metadata(const Token& at) const;
    void analyze_field_initializers(Class& c);
    void analyze_reachable_classes();
    void select_entry_point();

    // Type resolution and specialization
    Class* find_class(const std::string& name) const;
    Enum* find_enum(const std::string& name) const;
    void append_class(Class value);
    static std::string fullname(const std::string& ns,const std::string& name);
    Class* member_context() const;
    bool imported(const Class* c,const std::string& name) const;
    std::string resolve_name(const std::string& n,const Class* context) const;
    void require_type_access(const std::string& name,const Class* context,const Token& at) const;
    void validate_source_type_access(bool scopes_known);
    void validate_template_type_access();
    std::string resolve(std::string t,Class* context,const Token& at,bool allow_void=false);
    void instantiate_nested_types(const Class& original,const Class& owner,const Substitution& substitutions);
    bool derives(const std::string& from,const std::string& to) const;
    bool stringable(const std::string& type);

    // Local bindings and optional flow facts
    static Facts intersect(const Facts& a,const Facts& b);
    std::size_t optional_root(const Expr& e) const;
    bool is_present(const Expr& e) const;
    void extract_optional(Expr& e);
    void narrow(Expr& e);
    void kill_escaped();
    void optional_assignment(const Expr& dest,const Expr& source);
    std::pair<Facts,Facts> condition_facts(const Expr& e) const;
    void remember_condition(const Expr& e);
    const Symbol* symbol(const std::string& n) const;
    bool local_arena_alias(const Expr& e,bool storage=false) const;
    void set_local_arena_alias(std::size_t id,bool alias);
    std::size_t add_symbol(const std::string& n,const std::string& t,bool constant,const Token& at,bool parameter=false);

    // Conversions
    void value(const Expr& e,bool allow_void=false) const;
    bool standard_convert(const std::string& target,const Expr& e,bool constant=false);
    std::vector<UserConversion> user_conversions(const std::string& target,const Expr& source,bool explicit_cast);
    bool can_convert(const std::string& target,const Expr& e,bool constant=false);
    bool apply_user_conversion(const std::string& target,Expr& e,bool explicit_cast);
    void validate_conversion_cycles();
    void convertible(const std::string& target,Expr& e,bool constant=false);
    void writable(const Expr& e) const;

    // Member lookup and properties
    bool protected_scope(const Class& owner) const;
    bool member_accessible(const Class& owner,bool pub,bool prot) const;
    void accessible(const Class& owner,bool pub,bool prot,const Expr& e) const;
    std::pair<Class*,const Field*> field(Class& c,const std::string& n);
    std::pair<Class*,const Property*> property(Class& c,const std::string& n);
    std::vector<std::pair<Class*,Method*>> methods(Class& c,const std::string& n,bool constructors=false);
    std::string qualified_expr(const Expr& e) const;
    bool namespace_exists(const std::string& n) const;
    bool bind_type_or_namespace(Expr& e,const std::string& n);
    void bind_member(Expr& e,Class& c,bool class_receiver,bool lvalue,bool constant);
    void bind_constant_template(Expr& e,bool callee);
    ExprPtr temporary_name(const Token& at,const std::string& name);
    ExprPtr property_call(const Expr& source,bool get);
    void read_property(Expr& e);
    void update_property(Expr& e);
    void bind_name(Expr& e, bool callee);
    void bind_member_access(Expr& e, bool callee);

    // Declaration preparation and layout
    static std::string signature(const Method& m);
    void native_headers(const std::vector<Attribute>& attrs);
    void mark_type(std::string t);
    void require_primitive_formatter(const Expr& value);
    void prepare_method(Class& c,Method& m);
    void prepare(Class& c);
    void prepare_enum(Enum& en);
    void enums();
    void layouts();

    // Concepts and constraints
    const Concept* find_concept(const std::string& n,Class* context,const Token& at) const;
    IterationElement iteration_element(const std::string& source,Class* context,const Token& at,bool iterator=false,bool constant=false);
    bool satisfies_types(const std::string& concept_name,const std::vector<std::string>& types,Class* context,const Token& at);
    bool satisfies(const std::string& concept_name,const std::string& t,Class* context,const Token& at);
    void constraints(const Method& m,const Substitution& map,Class& owner,const Token& at);

    // Overload resolution and argument binding
    bool infer(const std::string& pattern,const std::string& actual,const std::vector<std::string>& parameters,Substitution& map);
    Method& instantiate(Class& owner,Method& original,const Substitution& map);
    Method& instantiate_native_pack(Class& owner,Method& original,const Substitution& map,const std::vector<std::string>& types);
    static std::size_t required_arguments(const Method& m);
    static bool variadic(const Method& m);
    static bool value_pack(const Method& m);
    static bool heterogeneous_pack(const Method& m);
    std::pair<Class*,Method*> select(std::vector<std::pair<Class*,Method*>> candidates,const std::vector<std::string>& explicit_types,std::vector<Expr*>& arguments,const Token& at,bool allow_pack=false,bool allow_no_match=false);
    std::vector<std::pair<Class*,Method*>> extension_methods(const std::string& name,const Token& at);
    bool bind_extension_member(Expr& target);
    bool builtin_instance_member(const std::string& type,const std::string& name) const;
    bool lower_extension_call(Expr& call,const std::vector<std::string>& types,bool allow_no_match=false);
    void check_default(Class& owner,Parameter& parameter);
    void normalize_arguments(Class& owner,Method& method,std::vector<ExprPtr>& arguments,std::size_t start);
    std::pair<Class*,Method*> constructor(const std::string& t,std::vector<Expr*>& args,const Token& at,bool base_subobject=false);
    void default_constructible(const std::string& t,const Token& at);

    // Delegates, events and lambdas
    void require_event_owner(const Expr& e) const;
    void lower_event_receiver(Expr& e);
    std::pair<Class*,Method*> delegate_method(const Expr& e,const std::string& action);
    bool delegate_compatible(const Expr& e,const std::string& action);
    void bind_delegate(Expr& e,const std::string& action);
    bool lambda_matches(const Expr& e,const std::string& target);
    std::string infer_lambda(const Expr& e);
    void bind_lambda(Expr& e,const std::string& target);
    bool lambda_uses_receiver(const LambdaSyntax& lambda);

    // Expression analysis
    bool has_operator(const std::string& t,const std::string& op,std::size_t count);
    bool overload(Expr& e,const std::string& op,bool update=false);
    std::string binary_type(const std::string& op,const Expr& a,const Expr& b,const Token& at);
    void size_of(Expr& e,Class* context);
    void finish_expression(Expr& e, bool callee, const std::string& expected, bool assignment_target);
    void expression(Expr& e,bool callee=false,const std::string& expected= {},bool assignment_target=false);
    bool pure(const Expr& e) const;
    bool constant_initializer(const Expr& e) const;

    // Built-in calls
    void collection_call(Expr& e,Expr& target);
    bool bind_implicit_static(Expr& e);
    void builtin_call(Expr& e,Expr& target);

    // Call expressions
    void call_expression(Expr& e, bool callee, const std::string& expected, bool assignment_target);

    // Statements and method bodies
    void scoped(Stmt& s);
    void condition(Expr& e);
    Value case_value(const Expr& e,unsigned depth=0) const;
    void switch_statement(Stmt& s);
    void statement(Stmt& s);
    bool returns(const Stmt& s) const;
    bool has_switch_break(const Stmt& s) const;
    void analyze_method(Class& c,Method& m);

    // Source navigation
    void navigation_bindings(bool collect_references=true);
};

} // namespace bolt::semantic
