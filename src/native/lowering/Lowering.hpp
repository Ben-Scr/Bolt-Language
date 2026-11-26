#pragma once

#include "../ReadOnlyAnalysis.hpp"
#include <bolt/native/ABI.hpp>
#include <bolt/native/IR.hpp>
#include <bolt/native/Layout.hpp>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace bolt::ir::lowering {

using K = Expr::Kind;
using B = Expr::Binding;

std::string mangle(const std::string& text);
std::string method_symbol(const Class& c, const Method& m);
bool concrete(const Method& m);
bool pure_scalar_value(const Expr& e);
bool reads_integer_value(const Expr& e);
bool cannot_change_string(const Expr& e);

using FieldLayout=native_abi::ValueFieldLayout;
struct Layout {
    std::string name,symbol;
    std::size_t id=0,size=0,alignment=1,marker=static_cast<std::size_t>(-1);
    int kind=0;
    int standalone_kind(const Class* cls) const;
    Type ir=Type::Void;
    bool aggregate=false;
    bool plain_struct=false;
    // For small non-owning structs with padding, one bit per declared field byte.
    std::uint64_t padded_field_bytes=0;
    // Small value classes copy scalar fields and rebuild each static class header.
    std::uint64_t class_field_bytes=0;
    std::vector<std::pair<std::size_t,std::string>> class_headers;
    bool inline_optional=false;
    bool zero_default=false;
    std::vector<FieldLayout> fields;
};
struct Value {std::string type;Reg value=0;}; // aggregate values are addresses; scalars are registers
class Lower;

// One builder emits a function and tracks its registers, control flow and cleanup.
class Builder {
public:
    Lower& lower;
    Function fn;
    Label current=0,exit=0,error=0,failure=0;
    Reg context=1,self=0,return_slot=0,structure_return=0,entered_slot=0;
    std::string return_type="void",owner,completes_type;
    Location at;
    std::vector<Instruction> prologue;
    struct OwnedSlot {
        std::string type;
        Reg address;
        Reg live;
    };
    std::vector<OwnedSlot> cleanup;
    std::unordered_map<std::size_t,Reg> locals;
    struct ElementTag { std::string storage,type; };
    std::unordered_map<Reg,ElementTag> element_tags;
    std::pair<const Expr*, Reg> loop_edit_ranges{};
    std::pair<const Expr*, Reg> utf8_write_index{};
    std::unordered_map<const Expr*,Reg> forwarded_utf8_reads;
    bool single_utf8_edit(const Stmt& scope,std::size_t start,std::size_t finish,const Expr& source);
    std::size_t lower_utf8_edits(const Stmt& scope, std::size_t start);
    struct ExitAction { const Stmt* finalizer=nullptr;bool catch_end=false;std::size_t cleanup_start=0;Label outer=0; };
    std::vector<ExitAction> exit_actions;
    void leave_actions(std::size_t first);
    void try_statement(const Stmt&);
    struct Loop { Label done,again; std::size_t cleanup_start,action_depth; bool switch_scope=false; };
    std::vector<Loop> loops;
    bool frames=true, kernel_body=false;
    std::unordered_map<Reg,std::string> core_types;
    std::unordered_map<Reg,std::string> core_strings;
    Builder(Lower&,std::string,Type,std::vector<Type>,Location);
    Reg reg(Type t){fn.registers.push_back(t);return static_cast<Reg>(fn.registers.size()-1);}
    Label block(){fn.blocks.emplace_back();return static_cast<Label>(fn.blocks.size()-1);}
    bool ended()const{const auto& c=fn.blocks[current].code;return !c.empty()&&(c.back().op==Op::Jump||c.back().op==Op::Branch||c.back().op==Op::Return);}
    void emit(Instruction i){i.at=at;fn.blocks[current].code.push_back(std::move(i));}
    Reg instruction(Op op,Type ty,std::vector<Reg> args={},std::uint64_t immediate=0,std::string text={});
    Reg integer(Type t,std::uint64_t bits){if(width(t)<8&&width(t))bits&=(std::uint64_t(1)<<(8*width(t)))-1;return instruction(Op::Constant,t,{},bits);}
    Reg null(){return integer(Type::Ptr,0);}
    Reg address(const std::string& name){return instruction(Op::Address,Type::Ptr,{},0,name);}
    Reg offset(Reg base,std::uint64_t n){if(!n)return base;return instruction(Op::Offset,Type::Ptr,{base},n);}
    // Only these compiler-created addresses are known to denote disjoint
    // runtime objects. Arbitrary user and foreign pointers carry no alias tag.
    Reg load(Type type, Reg ptr, const char* object = "") {
        return instruction(Op::Load,type,{ptr},0,object);
    }
    Reg load_sequence_data(Reg header) {
        return load(Type::Ptr, offset(header,offsetof(BrSequenceHeader,data)), "sequence header");
    }
    Reg buffer_data(Reg header);
    Reg load_buffer_size(Reg ptr) {
        constexpr auto upper = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
        return instruction(Op::Load,Type::I64,{ptr},upper,"sequence header");
    }
    void store(Reg ptr, Reg value, const char* object = "") {
        instruction(Op::Store,Type::Void,{ptr,value},0,object);
    }
    Reg cmp(const std::string& op,Reg a,Reg b){return instruction(Op::Compare,Type::I32,{a,b},0,op);}
    Reg cast(Reg a,Type type,const char* op){if(fn.registers[a]==type)return a;return instruction(Op::Cast,type,{a},0,op);}
    Reg boolean(Reg a){return cast(a,Type::I8,"trunc");}
    Reg alloc(std::size_t bytes, bool zero = true);
    Reg temp(const std::string& type,bool owned=true);
    Reg type(const std::string& t);
    Reg string(const std::string& s);
    Reg project_symbol(const std::string& key);
    Reg dispatch_slot(const std::string& id);
    Reg site();
    void jump(Label b, unsigned unroll_count=0){Instruction i;i.op=Op::Jump;i.yes=b;i.immediate=unroll_count;emit(i);}
    void branch(Reg value, Label yes, Label no, std::size_t reduction_element_bytes = 0);
    Reg call(const std::string&,Type,std::vector<Reg>,bool check=true,bool external=true);
    void check();
    void guard(Reg invalid,const std::string& message);
    void tick();
    void finish();
    Value expression(const Expr&);
    Reg reference(const Expr&);
    Reg borrow(const std::string& source,const std::string& target,Reg address);
    Reg sequence_index(const std::string& owner,const std::string& element,Reg slot,Reg index);
    Reg sequence_size(Reg slot,std::size_t member,bool bounded_buffer = false);
    Reg receiver(const Expr& member);
    Value converted(Value,const std::string&);
    Reg checked_float_to_integer(Reg value, const Layout& src, const Layout& dst);
    Reg check_finite(Reg value);
    Value snapshot(Value);
    bool fresh_handle(const Expr&);
    Value new_vector(const Expr&, Reg* length = nullptr);
    void initialize_array_elements(const std::string& array,Reg slot);
    Reg text_header(Reg object);
    Reg string_equal(Reg lhs,Reg rhs);
    Reg literal_affix(Reg slot,const std::string& pattern,bool suffix);
    Value byte_length(Reg slot);
    Reg memory(Value);
    Value from_memory(const std::string&,Reg);
    void assign(const std::string&,Reg,Value);
    void take_handle(Reg destination, Value source);
    void copy_value(const std::string&,Reg destination,Reg source);
    void copy_small_value(const Layout&,Reg destination,Reg source);
    void clear_value(const std::string&,Reg address,const char* runtime_operation);
    Value arithmetic(const std::string&,Value,Value,bool known_finite=false);
    std::set<const Expr*> finite_operations;
    std::set<const Expr*> bounded_additions;
    std::set<const Stmt*> unroll_tallies;
    void prove_counted_additions(const Stmt&);
    void prove_finite_loops(const Stmt& block);
    Value bitwise(const std::string&,Value,Value,const std::string&);
    Value call_expr(const Expr&);
    struct MemoryRegion {
        const Expr* fill = nullptr;
        const Expr* copy = nullptr;
        const Expr* write = nullptr;
        const Expr* compare = nullptr;
        const Expr* move = nullptr;
        std::vector<Reg> filled;
        std::vector<Reg> copied;
        Reg uniform = 0, written_address = 0, written_byte = 0;
    };
    MemoryRegion* memory_region = nullptr;
    std::size_t lower_memory_transfers(const Stmt& block, std::size_t index);
    void narrow_comparison(std::vector<Reg>& arguments);
    void transfer_filled(const Expr&, const Class&, const Method&, std::vector<Reg>&, bool move);
    void add_plain(const Expr& e, Reg slot, Value item);
    void edit_scalar_buffer(const Expr&, Reg object, const std::string& element, const Method* list_method=nullptr);
    void move_within_storage(Reg dest, Reg source, Reg bytes, std::size_t stride);
    std::size_t paired_list_edit(const Stmt& block, std::size_t index);
    // A pure scalar calculation can retry its original statements on failure.
    std::optional<Label> guard_fallback;
    void append_plain_range(const Expr&, Reg slot, Reg array, Reg index, Reg amount, std::size_t stride);
    void truncate_plain(const Expr& e, Reg slot, Value retained);
    bool optimize_counted_loop(const Stmt&);
    bool queue_work_loop(const Stmt&, const Expr& limit);
    bool version_array_loop(const Stmt&, std::int32_t stride);
    bool select_array_update(const Stmt&);
    bool select_array_cursor(const Stmt&);
    bool conditional_increment(const Stmt&);
    bool updates_tested_element(const Stmt&) const;
    bool modular_sequence_sum(const Stmt& loop, const Expr& limit);
    bool widening_sequence_sum(const Stmt& loop, const Expr& limit);
    const Expr* stable_loop_limit = nullptr;
    Reg stable_loop_value = 0;
    std::unordered_map<const Expr*, Reg> affine_data;
    std::unordered_set<const Expr*> affine_ops;
    std::unordered_set<std::size_t> stack_ints;
    bool reverse_append_loop(const Stmt&);
    bool budgeted_scalar_loop(const Stmt&);
    void ordinary_for(const Stmt&, std::int32_t proven_stride = 0, bool keep_rolled = false, bool budget_reserved = false);
    bool optimize_search(const Stmt&,const Stmt&);
    struct TextView { Reg data, count; };
    std::unordered_map<std::size_t, TextView> text_views;
    std::vector<std::size_t> prepare_text_views(const Stmt&);
    // Snapshots of fresh, fully consumed stack buffers can assume ownership.
    std::set<const Expr*> consumed_snapshots;
    struct PrivateArray {
        Reg length = 0;
        std::optional<std::uint32_t> maximum = 0;
        Reg data = 0;
        bool nullable = false;
        bool confined_elements = false;
        Reg frozen_min = 0, frozen_max = 0;
    };
    std::unordered_map<std::size_t, PrivateArray> private_arrays;
    struct FrozenTable { std::size_t local, bytes; };
    std::unordered_map<const Stmt*,FrozenTable> frozen_tables;
    std::size_t table_bytes = 0;
    void find_frozen_tables(const Stmt&);
    void find_private_arrays(const Stmt&);
    void load_array_view(PrivateArray&, Reg header);
    std::vector<std::size_t> loop_array_views(const Stmt&);
    bool peel_scalar_scan(const Stmt& seed,const Stmt& loop);
    PrivateArray* private_array(const Expr& access);
    void find_consumed_snapshots(const Stmt& block);
    Value take_snapshot(const Expr&, Reg object);
    struct IndexedArray {
        std::size_t index;
        const Expr* formula;
        Reg length;
        std::map<std::size_t,Reg> captured;
        bool complementary_sum;
    };
    std::map<std::size_t, IndexedArray> indexed_arrays;
    bool indexed_snapshot(const Stmt&, std::size_t);
    Value indexed_element(const IndexedArray&, Reg index);
    struct ListProjection {
        const Stmt* fill = nullptr;
        const Expr* formula = nullptr;
        std::int32_t length = 0;
        Reg count = 0;
        std::map<std::size_t, Reg> inputs;
    };
    std::map<std::size_t, ListProjection> projected_lists;
    std::unordered_map<const Stmt*,std::size_t> projected_fills;
    std::unordered_map<const Expr*,std::size_t> projected_calls;
    void find_list_projections(const Stmt&);
    void fill_projection(ListProjection&);
    Value projected_call(const Expr&, ListProjection&);
    bool projected_sum(const Stmt&, const Expr& limit, const Expr& body);
    bool scalar_append_loop(const Stmt&, const Expr& limit, const Expr& body, const Stmt* record_body = nullptr);
    struct PrivateRecordBuffer { std::size_t id; std::int32_t capacity; };
    std::unordered_map<const Stmt*,PrivateRecordBuffer> private_record_clears;
    void find_private_record_buffers(const Stmt& scope);
    bool elide_record_refill(const Stmt& scope, std::size_t position);
    struct RecordFill {
        std::size_t source;
        std::uint32_t count;
        std::map<std::string,std::uint32_t> masks;
    };
    std::unordered_map<const Stmt*,const Stmt*> following_for;
    std::unordered_map<const Stmt*,RecordFill> record_fills;
    bool record_field_sum(const Stmt& loop, const Expr& limit, const Expr& body);
    bool checked_array_sum(const Stmt&, const Expr& limit, const Expr& body);
    Value library_call(const std::string&,const std::string&,Reg,const std::vector<std::pair<std::string,Reg>>&,Reg output=0);
    Value string_call(const std::string&,const std::string&,std::vector<Value>,Reg output = 0);
    Value invoke(const Class&,const Method&,Reg,const std::vector<ExprPtr>&,std::size_t,bool dispatch=true,const std::vector<Reg>* prepared=nullptr);
    Value invoke_operator(const Class&,const Method&,std::vector<Value>);
    void initialize(const std::string&,Reg);
    void initialize_class(const Class&,const Method*);
    void drop_since(std::size_t first);
    struct ConditionalSwap {Label header,done;Reg condition,first,second;};
    std::vector<ConditionalSwap> conditional_swaps;
    bool lower_pointer_swap(const Stmt&);
    void promote_private_swaps();
    void statement(const Stmt&);
};

// Builders share the module's layouts, symbols and dispatch slots through Lower.
class Lower {
public:
    const Program& program;
    Module module;
    std::map<std::string,Layout> layouts;
    std::unordered_map<std::string,std::string> strings;
    std::unordered_map<std::string,std::size_t> external_indices;
    std::unordered_map<std::string,std::vector<std::size_t>> line_starts;
    std::string_view source_line(const std::string& file,int line);
    std::unordered_map<std::string,std::map<std::array<int,3>,std::string>> site_symbols;
    std::map<std::string,int> virtual_slots;
    std::unordered_set<std::string> dispatched;
    std::unordered_set<std::string> worker_callbacks;
    int virtual_slot(const std::string& id){auto [it,added]=virtual_slots.emplace(id,static_cast<int>(virtual_slots.size()));(void)added;return it->second;}
    int dispatch_slot(const std::string& id){dispatched.insert(id);return virtual_slot(id);}
    std::vector<std::pair<const Class*,const Field*>> statics;
    LowerOptions options;
    native_abi::ValueLayouts target_layouts;
    ReadOnlyAnalysis read_only;
    std::vector<std::string> pending_layouts;
    explicit Lower(const Program& p,LowerOptions o):program(p),options(o),target_layouts(p),read_only(p){
        module.instrument_budget=o.instrument_budget;
        if(o.source_core){
            auto* c=p.find_class("Bolt.Runtime.StandaloneCore");
            if(c && c->token.at.file=="<bolt-stdlib>/System/StandaloneCore.bolt")source_core_class=c;
        }
    }
    std::size_t layout_depth=0;

    void finish_layouts();
    Layout& layout(std::string t);
    Layout& layout_raw(std::string t);
    std::set<std::string> text_literals;
    const Class* source_core_class=nullptr;
    const Class* core() const {return source_core_class;}
    Reg core_call(Builder&,const std::string&,Type,std::vector<Reg>,bool);
    std::set<std::string> thread_entries, lifetime_entries, boxed_formatters;
    // Descriptor callbacks are needed only if their typed invoker is reachable.
    std::map<std::string,std::string> value_contract_invokers;
    Reg thread_call(Builder&,const std::string&,Type,std::vector<Reg>,bool inspect=false);
    std::string thread_entry();
    std::string job_adapter(const Class&,const Method&);
    void core_descriptors();
    std::string text_literal(const std::string& value);
    std::string literal(const std::string& s);
    void external(const std::string& name,Type result,const std::vector<Type>& parameters);
    std::string static_name(const std::string& c,const std::string& n){return mangle(c+"::field::"+n);}
    std::string getter(const std::string& c,const std::string& n){return mangle(c+"::get::"+n);}
    std::string init_name(const std::string& t){return mangle("default::"+t);}
    Reg field(Builder& b,const std::string& c,const std::string& n,Reg self);
    const Method* default_constructor(const Class& c);
    Type result_type(const Method& m);
    std::vector<Type> parameters(const Method& m);
    void methods();
    void project_functions();
    void method(const Class&,const Method&);
    void async_method(const Class&,const Method&);
    void stringable_adapters();
    void initializers();
    void native_initializers();
    void native_virtual_adapters();
    void static_initializers();
    void entry();
#if defined(__APPLE__) || defined(__FreeBSD__)
    void libc_main(const Location&);
#endif
    bool prove_runtime_free(const Class* owner, const Method* main);
    Module run();
};

} // namespace bolt::ir::lowering
