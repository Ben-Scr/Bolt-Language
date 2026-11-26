#include "Lowering.hpp"
#include <bolt/Value.hpp>
#include <cmath>
#include <bit>
#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

namespace bolt::ir::lowering {

#include "../QueueWorkLoop.inc"

// Abstract interpretation of a straight-line numeric loop. A carried double
// starts in [-limit,limit]. To omit a finite check, prove that the complete
// loop maps that interval into itself, including rounding after each operation.
// Aliases, calls, nonlocal stores and non-numeric expressions end the proof.
struct FloatLoopProof {
    static constexpr double limit = 0x1p128;
    std::map<std::size_t,double> bounds;
    std::set<const Expr*> proven;
    double bound(const Expr& e) {
        const double unknown = std::numeric_limits<double>::infinity();
        if (is_integer(e.type) && pure_scalar_value(e)) return std::ldexp(1.0,bits(e.type));
        if (e.type != "double") return unknown;
        if (e.kind == K::Literal) return std::abs(std::get<double>(literal_value(e).data));
        if (e.kind == K::Name && e.binding == B::Local && !is_reference(e.declared_type)) {
            const auto found = bounds.find(e.local_id);
            return found == bounds.end() ? unknown : found->second;
        }
        if (e.kind == K::Call && e.binding == B::Cast && e.args.size()==2 &&
            is_integer(e.args[1]->type)) return bound(*e.args[1]);
        if (e.kind != K::Binary || e.binding != B::None || e.args.size()!=2 ||
            (e.text!="*" && e.text!="+" && e.text!="-")) return unknown;
        const double left = bound(*e.args[0]), right = bound(*e.args[1]);
        if (!std::isfinite(left) || !std::isfinite(right)) return unknown;
        const double rounded = e.text=="*" ? left*right : left+right;
        const double upper = std::nextafter(rounded,unknown);
        if (std::isfinite(upper)) proven.insert(&e);
        return upper;
    }
    bool statement(const Stmt& s) {
        using S = Stmt::Kind;
        if (s.kind == S::Block || s.kind == S::Declarations)
            return std::all_of(s.statements.begin(),s.statements.end(),[&](const auto& child){return statement(*child);});
        if (s.kind==S::Variable && s.type=="double") {
            const double value = s.expression ? bound(*s.expression) : 0.0;
            bounds[s.local_id]=value;
            return std::isfinite(value);
        }
        if (s.kind!=S::Expression || !s.expression) return false;
        const Expr& e=*s.expression;
        if (e.kind!=K::Assign || e.text!="=" || e.binding!=B::None || e.args.size()!=2) return false;
        const Expr& dest=*e.args[0];
        if (dest.kind!=K::Name || dest.binding!=B::Local || is_reference(dest.declared_type) ||
            !bounds.contains(dest.local_id)) return false;
        const double value=bound(*e.args[1]);
        if (!std::isfinite(value) || value>limit) return false;
        bounds[dest.local_id]=value;
        return true;
    }
};
void Builder::prove_finite_loops(const Stmt& block) {
    using S=Stmt::Kind;
    if (!lower.options.optimize_loops) return;
    std::map<std::size_t,double> fresh;
    for (const auto& child:block.statements) {
        const auto& s=*child;
        if (s.kind==S::Variable && s.type=="double" &&
            (!s.expression || (s.expression->type=="double" && s.expression->kind==K::Literal &&
             std::abs(std::get<double>(literal_value(*s.expression).data))<=FloatLoopProof::limit))) {
            fresh[s.local_id]=FloatLoopProof::limit;
            continue;
        }
        if (!fresh.empty() && s.kind==S::For && s.init && s.init->kind==S::Variable && s.init->type=="int" &&
            s.init->expression && pure_scalar_value(*s.init->expression) && s.condition && pure_scalar_value(*s.condition) &&
            s.increment && s.increment->kind==K::Postfix && s.increment->binding==B::None &&
            s.increment->text=="++" && s.increment->args.size()==1 && s.increment->args[0]->kind==K::Name &&
            s.increment->args[0]->binding==B::Local && s.increment->args[0]->local_id==s.init->local_id && s.body) {
            FloatLoopProof proof{fresh,{}};
            if (proof.statement(*s.body)) finite_operations.insert(proof.proven.begin(),proof.proven.end());
        }
        fresh.clear();
    }
}

// Restrict the trip-count proof to ordinary local scalar updates. Calls,
// reference writes and nested control flow can change the induction variable
// or the accumulator independently, so they keep the checked addition.
bool reads_integer_value(const Expr& e) {
    if (!is_integer(e.type)) return false;
    if (e.kind == K::Literal || (e.kind == K::Name && e.binding == B::Local)) return true;
    if (e.kind == K::Call)
        return e.binding == B::Cast && e.args.size() == 2 && reads_integer_value(*e.args[1]);
    if (e.binding != B::None) return false;
    if (e.kind == K::Index && e.args.size() == 2)
        return array_rank(e.owner) == 1 && e.args[0]->kind == K::Name &&
               e.args[0]->binding == B::Local && reads_integer_value(*e.args[1]);
    if (e.kind == K::Binary && e.args.size() == 2)
        return reads_integer_value(*e.args[0]) && reads_integer_value(*e.args[1]);
    if (e.kind == K::Unary && e.args.size() == 1 && (e.text == "-" || e.text == "+" || e.text == "~"))
        return reads_integer_value(*e.args[0]);
    return false;
}

// Each path through this loop body takes at most one unit step. Keep
// conditions read-only so neither the induction variable nor the tally can
// change outside the updates being proved.
static bool unit_steps(const Stmt& s, std::vector<const Expr*>& updates) {
    using S=Stmt::Kind;
    if (s.kind==S::Block && s.statements.size()==1) return unit_steps(*s.statements.front(),updates);
    if (s.kind==S::If && s.condition && s.body) {
        const auto& c=*s.condition;
        const bool scalar_test=pure_scalar_value(c) ||
            (c.kind==K::Binary && c.binding==B::None && c.type=="bool" && c.args.size()==2 &&
             reads_integer_value(*c.args[0]) && reads_integer_value(*c.args[1]));
        return scalar_test && unit_steps(*s.body,updates) && (!s.alternative || unit_steps(*s.alternative,updates));
    }
    if (s.kind!=S::Expression || !s.expression) return false;
    const auto& e=*s.expression;
    if ((e.kind!=K::Postfix && e.kind!=K::Unary) || e.binding!=B::None ||
        (e.text!="++" && e.text!="--") || e.args.size()!=1) return false;
    const auto& local=*e.args[0];
    if (local.kind!=K::Name || local.binding!=B::Local || local.type!="int" ||
        is_reference(local.declared_type) || (!updates.empty() && local.local_id!=updates[0]->args[0]->local_id)) return false;
    updates.push_back(&e);
    return true;
}

static bool independent_of(const Expr& e,std::size_t id) {
    return e.kind!=K::Lambda && !(e.binding==B::Local && e.local_id==id) &&
        (!e.arena || independent_of(*e.arena,id)) &&
        std::all_of(e.args.begin(),e.args.end(),[&](const auto& a){return independent_of(*a,id);});
}

void Builder::prove_counted_additions(const Stmt& block) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget) return;
    for (std::size_t i = 1; i < block.statements.size(); ++i) {
        const auto& accumulator = *block.statements[i - 1];
        const auto& loop = *block.statements[i];
        if (loop.kind != S::For ||
            !loop.init || loop.init->kind != S::Variable || loop.init->type != "int" ||
            !loop.init->expression || loop.init->expression->kind != K::Literal || loop.init->expression->type != "int" ||
            !loop.condition || !loop.increment || !loop.body) continue;
        const auto& cond = *loop.condition;
        const auto& step = *loop.increment;
        auto is_counter = [&](const Expr& e) {
            return e.kind == K::Name && e.binding == B::Local && e.local_id == loop.init->local_id;
        };
        if (cond.kind != K::Binary || cond.binding != B::None || cond.text != "<" || cond.args.size() != 2 ||
            !is_counter(*cond.args[0]) || cond.args[1]->kind != K::Literal || cond.args[1]->type != "int" ||
            (step.kind != K::Postfix && step.kind != K::Unary) || step.binding != B::None ||
            step.text != "++" || step.args.size() != 1 || !is_counter(*step.args[0])) continue;

        const auto start = literal_value(*loop.init->expression).as_int();
        const auto stop = literal_value(*cond.args[1]).as_int();
        const auto trips = std::max<std::int64_t>(0,stop-start);
        std::vector<const Expr*> steps;
        if (unit_steps(*loop.body,steps) && !is_counter(*steps.front()->args[0])) {
            const auto id=steps.front()->args[0]->local_id;
            for (auto j=i; j>0; --j) {
                const auto& decl=*block.statements[j-1];
                if (decl.kind!=S::Variable) break;
                if (decl.local_id==id) {
                    if (decl.type!="int" || (decl.expression && (decl.expression->kind!=K::Literal ||
                        decl.expression->type!="int"))) break;
                    const auto initial=decl.expression?literal_value(*decl.expression).as_int():0;
                    if (initial-trips>=INT32_MIN && initial+trips<=INT32_MAX) {
                        bounded_additions.insert(steps.begin(),steps.end());
                        bounded_additions.insert(&step); // Only runs while the induction value is below stop.
                        if (steps.size()==2 && trips>=64) unroll_tallies.insert(&loop);
                    }
                    break;
                }
                // No intervening declaration may change or take the address of the tally.
                if (decl.expression && !independent_of(*decl.expression,id)) break;
            }
        }
        if (accumulator.kind!=S::Variable || accumulator.type!="ulong" ||
            (accumulator.expression && (accumulator.expression->kind!=K::Literal ||
             !is_integer(accumulator.expression->type)))) continue;
        std::uint64_t per_iteration = 0;
        std::vector<const Expr*> additions;
        const auto& body=*loop.body;
        const Expr* queue=nullptr;
        if (body.kind==S::Block && !body.statements.empty()) {
            const auto& first=*body.statements.front();
            if (first.kind==S::Variable && is_numeric(first.type) && first.expression &&
                first.type==first.expression->type) {
                queue=queue_receiver(*first.expression,"Dequeue",0);
                const auto* cls=queue ? lower.program.find_class(first.expression->owner) : nullptr;
                if (!cls || cls->token.at.file!="<bolt-stdlib>/System/Collections/Generic/Queue.bolt") queue=nullptr;
            }
        }
        auto check_update = [&](const Stmt& st) {
            const auto* e = st.expression.get();
            if (queue && e) {
                if (&st==body.statements.front().get() || (st.kind==S::Variable && is_numeric(st.type) && queue_scalar(*e)))
                    return true;
                const auto* target=st.kind==S::Expression ? queue_receiver(*e,"Enqueue",1) : nullptr;
                if (target && target->local_id==queue->local_id && queue_scalar(*e->args[1])) return true;
            }
            if (st.kind != S::Expression || !e || e->kind != K::Assign || e->binding != B::None ||
                e->args.size() != 2) return false;
            const auto& dest = *e->args[0];
            const auto& rhs = *e->args[1];
            if (dest.kind != K::Name || dest.binding != B::Local || is_reference(dest.declared_type) ||
                !is_integer(dest.type) || is_counter(dest) || !reads_integer_value(rhs)) return false;
            if (dest.local_id != accumulator.local_id) return e->text == "=";
            if (e->text != "+=" || (!is_unsigned(rhs.type) && rhs.type != "ulong")) return false;
            const Expr* gain = &rhs;
            // Checked widening of a signed integer has already rejected negative values.
            while (gain->type == "ulong" && gain->kind == K::Call && gain->binding == B::Cast)
                gain = gain->args[1].get();
            if (!is_integer(gain->type) || bits(gain->type) > 32) return false;
            const auto bound = (std::uint64_t{1} << (bits(gain->type) - (is_unsigned(gain->type) ? 0 : 1))) - 1;
            if (UINT64_MAX - per_iteration < bound) return false;
            per_iteration += bound;
            additions.push_back(e);
            return true;
        };
        const bool safe = loop.body->kind == S::Block ?
            std::all_of(loop.body->statements.begin(), loop.body->statements.end(),
                        [&](const auto& st) { return check_update(*st); }) : check_update(*loop.body);
        if (!safe || additions.empty()) continue;
        const auto initial = accumulator.expression ? std::stoull(literal_value(*accumulator.expression).to_string()) : 0;
        if (!trips || per_iteration <= (UINT64_MAX - initial) / trips)
            bounded_additions.insert(additions.begin(), additions.end());
    }
}

// Recognize a first-match scan over an unchanged rank-one array or Buffer.
// The key and sequence are plain bindings, both the comparison and the body are
// side-effect-free, and the general loop remains in place for user operators.
bool Builder::optimize_search(const Stmt& loop, const Stmt& test) {
    using S = Stmt::Kind;
    if (return_type != "int" || test.kind != S::If || test.alternative || !test.condition || !test.body ||
        !loop.init->expression || loop.init->expression->kind != K::Literal ||
        literal_value(*loop.init->expression).as_int() != 0) return false;
    const auto* ret = test.body.get();
    if (ret->kind == S::Block && ret->statements.size() == 1) ret = ret->statements[0].get();
    const auto counter = [&](const Expr& e) {
        return e.kind == K::Name && e.binding == B::Local && e.local_id == loop.init->local_id;
    };
    if (ret->kind != S::Return || !ret->expression || !counter(*ret->expression)) return false;
    const auto& length = *loop.condition->args[1];
    if (length.kind != K::Member || length.binding != B::Property || length.arrow ||
        (length.text != "Length" && length.text != "Count") || length.args.size() != 1) return false;
    const auto& source = *length.args[0];
    const bool sequence = (is_array(source.type) && array_rank(source.type) == 1) || collection_kind(source.type) == "List";
    if (!sequence || source.kind != K::Name || source.is_static ||
        (source.binding != B::Local && source.binding != B::Field)) return false;
    const auto& equal = *test.condition;
    const Expr* indexed;
    const Expr* wanted;
    if (equal.kind == K::Binary && equal.binding == B::None && equal.text == "==" && equal.args.size() == 2) {
        indexed = equal.args[0].get(); wanted = equal.args[1].get();
    } else if (equal.kind == K::Call && equal.binding == B::Method && equal.args.size() == 3) {
        const auto* cls = lower.program.find_class(equal.owner);
        const auto* method = cls ? method_by_id(*cls, equal.resolved) : nullptr;
        if (!method || cls->native_cpp != "bolt::platform::Values" || method->name != "Equal" ||
            !native_abi::library_primitive_method(*cls, *method)) return false;
        indexed = equal.args[1].get(); wanted = equal.args[2].get();
    } else return false;
    if (wanted->kind == K::Index) std::swap(indexed, wanted);
    if (indexed->kind != K::Index || indexed->binding != B::None || indexed->args.size() != 2 ||
        !counter(*indexed->args[1]) || indexed->type != wanted->type ||
        (!is_integer(indexed->type) && indexed->type != "char" && indexed->type != "bool")) return false;
    const auto& owner = *indexed->args[0];
    if (owner.kind != K::Name || owner.binding != source.binding || owner.type != source.type ||
        owner.local_id != source.local_id || owner.owner != source.owner || owner.text != source.text) return false;
    if (wanted->kind != K::Literal && (wanted->kind != K::Name || wanted->binding != B::Local || counter(*wanted))) return false;

    const auto slot = reference(source);
    const auto key = wanted->kind == K::Name ? reference(*wanted) : memory(expression(*wanted));
    const auto result = temp("int", false);
    at = test.token.at;
    call("br_collection", Type::Void, {context, site(), type(source.type), slot, string("FindInteger"), result, key, null()});
    const auto index = load(Type::I32, result);
    const auto hit = block(), miss = block();
    branch(cmp("sge", index, integer(Type::I32, 0)), hit, miss);
    current = hit; store(return_slot, index); leave_actions(0); if (!ended()) jump(exit);
    current = miss;
    return true;
}

// A descending, bounds-checked copy can fill a reserved destination directly.
// Self-append, growth, zero handles and live iterators use the ordinary loop.
bool Builder::reverse_append_loop(const Stmt& loop) {
    using S = Stmt::Kind;
    const auto* start = loop.init->expression.get();
    const auto& cond = *loop.condition;
    const auto& step = *loop.increment;
    const auto induction = loop.init->local_id;
    const auto counter = [&](const Expr& x) {
        return x.kind == K::Name && x.binding == B::Local && x.local_id == induction;
    };
    const auto integer_literal = [](const Expr& x, int n) {
        return x.kind == K::Literal && x.type == "int" && literal_value(x).as_int() == n;
    };
    if (!start || start->kind != K::Binary || start->binding != B::None || start->text != "-" ||
        start->args.size() != 2 || !integer_literal(*start->args[1],1) ||
        cond.kind != K::Binary || cond.binding != B::None || cond.text != ">=" || cond.args.size() != 2 ||
        !counter(*cond.args[0]) || !integer_literal(*cond.args[1],0) ||
        (step.kind != K::Postfix && step.kind != K::Unary) || step.binding != B::None || step.text != "--" ||
        step.args.size() != 1 || !counter(*step.args[0])) return false;
    const auto& length = *start->args[0];
    if (length.kind != K::Member || length.binding != B::Property || length.text != "Length" ||
        length.args.size() != 1 || (array_rank(length.owner) != 1 && collection_kind(length.owner) != "List"))
        return false;
    const auto simple_slot = [](const Expr& x) {
        return x.kind == K::Name && !is_reference(x.declared_type) &&
            (x.binding == B::Local || (x.binding == B::Field && !x.is_static));
    };
    const auto& source = *length.args[0];
    if (!simple_slot(source)) return false;
    const auto* body = loop.body.get();
    if (body->kind == S::Block && body->statements.size() == 1) body = body->statements.front().get();
    if (body->kind != S::Expression || !body->expression) return false;
    const auto& add = *body->expression;
    if (add.kind != K::Call || add.binding != B::Collection || add.text != "Add" ||
        collection_kind(add.owner) != "List" || add.args.size() != 2) return false;
    const auto& member = *add.args[0];
    const auto& item = *add.args[1];
    if (member.kind != K::Member || member.arrow || member.args.size() != 1 || !simple_slot(*member.args[0]) ||
        item.kind != K::Index || item.binding != B::None || item.args.size() != 2 || !counter(*item.args[1]))
        return false;
    const auto& indexed = *item.args[0];
    if (!simple_slot(indexed) || indexed.binding != source.binding || indexed.type != source.type ||
        (source.binding == B::Local ? indexed.local_id != source.local_id : indexed.owner != source.owner || indexed.text != source.text) ||
        generic_args(add.owner).at(0) != item.type) return false;
    const auto& element = lower.layout(item.type);
    if (!native_abi::scalar_storage_kind(element.kind) && !element.plain_struct) return false;

    const auto done = block(), inspect = block(), nonempty = block(), room = block();
    const auto begin = block(), copy = block(), commit = block(), slow = block();
    const auto from = load(Type::Ptr,reference(source));
    branch(cmp("eq",from,null()),done,inspect);
    current = inspect;
    const auto n = load(Type::I64,offset(from,offsetof(BrSequenceHeader,count)));
    branch(cmp("eq",n,integer(Type::I64,0)),done,nonempty);
    current = nonempty;
    const auto to = load(Type::Ptr,reference(*member.args[0]));
    const auto blocked = instruction(Op::Or,Type::I32,{cmp("eq",to,null()),cmp("eq",to,from)});
    const auto observers = load(Type::I64,offset(context,offsetof(BrContext,active_ranges)),"execution context");
    branch(instruction(Op::Or,Type::I32,{blocked,cmp("ne",observers,integer(Type::I64,0))}),slow,room);
    current = room;
    const auto count_slot = offset(to,offsetof(BrSequenceHeader,count));
    const auto prior = load_buffer_size(count_slot);
    const auto cap = load_buffer_size(offset(to,offsetof(BrSequenceHeader,capacity)));
    // Buffer capacity is always <= int.MaxValue, including the destination.
    const auto available = instruction(Op::Sub,Type::I64,{cap,prior});
    branch(cmp("ule",n,available),begin,slow);
    current = begin;
    const auto input = load_sequence_data(from);
    const auto output = load_sequence_data(to);
    const auto stride = integer(Type::I64,element.size);
    const auto final_count = instruction(Op::Add,Type::I64,{prior,n});
    const auto remaining = alloc(8);
    store(remaining,n); jump(copy);
    current = copy;
    const auto left = load(Type::I64,remaining);
    const auto index = instruction(Op::Sub,Type::I64,{left,integer(Type::I64,1)});
    const auto source_byte = instruction(Op::Mul,Type::I64,{index,stride});
    const auto target_index = instruction(Op::Sub,Type::I64,{final_count,left});
    const auto target_byte = instruction(Op::Mul,Type::I64,{target_index,stride});
    const auto src = instruction(Op::Offset,Type::Ptr,{input,source_byte});
    const auto dst = instruction(Op::Offset,Type::Ptr,{output,target_byte});
    if (element.plain_struct) copy_value(item.type,dst,src);
    else store(dst,load(element.ir,src));
    store(remaining,index); branch(cmp("ne",index,integer(Type::I64,0)),copy,commit);
    current = commit; store(count_slot,final_count); jump(done);
    current = slow; ordinary_for(loop); if (!ended()) jump(done);
    current = done; return true;
}

#include "../ArrayLoopVersioning.inc"
#include "../LoopArrayViews.inc"
#include "../ScalarScan.inc"
#include "../ConditionalArrayUpdate.inc"
#include "../WideningSequenceSum.inc"
// These loops cannot call user code or share their locals. Charge the exact
// number of condition tests on exit, including a checked arithmetic failure.
static bool scalar_loop_value(const Expr& e) {
    if(!is_integer(e.type)) return false;
    if(e.kind==K::Literal) return true;
    if(e.kind==K::Name) return e.binding==B::Local && !is_reference(e.declared_type);
    if(e.kind!=K::Binary || e.binding!=B::None || e.args.size()!=2) return false;
    return scalar_loop_value(*e.args[0]) && scalar_loop_value(*e.args[1]);
}
static bool scalar_loop_body(const Stmt& s,std::size_t counter) {
    if(s.kind==Stmt::Kind::Block)
        return std::all_of(s.statements.begin(),s.statements.end(),[&](const auto& child){return scalar_loop_body(*child,counter);});
    if(s.kind!=Stmt::Kind::Expression || !s.expression) return false;
    const auto& e=*s.expression;
    if(e.kind!=K::Assign || e.binding!=B::None || e.text!="=" || e.args.size()!=2) return false;
    const auto& dest=*e.args[0];
    return dest.kind==K::Name && scalar_loop_value(dest) && dest.local_id!=counter && scalar_loop_value(*e.args[1]);
}
// A constant mask or remainder bounds the value carried into the next
// iteration. Prove each intermediate fits before selecting 32-bit operations.
struct NarrowLoop {
    const Expr& value;
    const Expr& target;
    std::uint64_t limit;

    static std::optional<std::uint64_t> constant(const Expr& e) {
        if(e.kind!=K::Literal || (e.type!="uint" && e.type!="ulong")) return {};
        return std::get<std::uint64_t>(literal_value(e).data);
    }
    std::optional<std::uint64_t> maximum(const Expr& e) const {
        if(e.type!=target.type) return {};
        if(e.kind==K::Name && e.binding==B::Local && e.local_id==target.local_id) return limit;
        if(auto n=constant(e); n && *n<=UINT32_MAX) return n;
        if(e.kind!=K::Binary || e.binding!=B::None || e.args.size()!=2) return {};
        auto left=maximum(*e.args[0]), right=maximum(*e.args[1]);
        if(!left || !right) return {};
        if(e.text=="+" && *left+*right<=UINT32_MAX) return *left+*right;
        if(e.text=="*" && (*left)*(*right)<=UINT32_MAX) return (*left)*(*right);
        if(e.text=="&") return std::min(*left,*right);
        if(e.text=="%" && *right && constant(*e.args[1])) return std::min(*left,*right-1);
        return {};
    }
    static std::optional<NarrowLoop> match(const Stmt& body) {
        if(body.kind==Stmt::Kind::Block)
            return body.statements.size()==1 ? match(*body.statements.front()) : std::nullopt;
        const auto& assign=*body.expression; // scalar_loop_body has checked the assignment.
        const auto& target=*assign.args[0];
        const auto& rhs=*assign.args[1];
        if(target.type!="ulong" || rhs.kind!=K::Binary || rhs.args.size()!=2 ||
           (rhs.text!="%" && rhs.text!="&")) return {};
        const auto bound=constant(*rhs.args[1]);
        if(!bound || *bound>UINT32_MAX || (!*bound && rhs.text=="%")) return {};
        NarrowLoop proof{rhs,target,*bound-(rhs.text=="%"?1u:0u)};
        return proof.maximum(rhs) ? std::optional<NarrowLoop>{proof} : std::nullopt;
    }
    Reg evaluate(Builder& b,const Expr& e,Reg carried) const {
        if(e.kind==K::Name) return carried;
        if(auto n=constant(e)) return b.integer(Type::I32,*n);
        const auto l=evaluate(b,*e.args[0],carried), r=evaluate(b,*e.args[1],carried);
        const auto op=e.text=="+"?Op::Add:e.text=="*"?Op::Mul:e.text=="&"?Op::And:Op::URem;
        return b.instruction(op,Type::I32,{l,r});
    }
    void emit(Builder& b,std::int64_t count,Label fallback) const {
        const auto address=b.locals.at(target.local_id);
        const auto initial=b.load(Type::I64,address);
        const auto start=b.block(), body=b.block(), done=b.block();
        b.branch(b.cmp("ule",initial,b.integer(Type::I64,limit)),start,fallback);
        b.current=start;
        const auto index=b.alloc(4,false), state=b.alloc(4,false);
        b.store(index,b.integer(Type::I32,0));
        b.store(state,b.cast(initial,Type::I32,"trunc"));
        b.jump(body);
        b.current=body;
        b.store(state,evaluate(b,value,b.load(Type::I32,state)));
        const auto next=b.instruction(Op::Add,Type::I32,{b.load(Type::I32,index),b.integer(Type::I32,1)},0,"nuw");
        b.store(index,next);
        b.branch(b.cmp("ult",next,b.integer(Type::I32,count)),body,done);
        b.current=done;
        b.store(address,b.cast(b.load(Type::I32,state),Type::I64,"zext"));
    }
};
bool Builder::budgeted_scalar_loop(const Stmt& s) {
    if(!s.init || s.init->kind!=Stmt::Kind::Variable || s.init->type!="int" || !s.init->expression ||
       !s.increment || !s.condition || !s.body) return false;
    const auto& first=*s.init->expression;
    const auto& cond=*s.condition;
    const auto& step=*s.increment;
    const auto counter=s.init->local_id;
    auto index=[&](const Expr& e){return e.kind==K::Name && e.binding==B::Local && e.local_id==counter;};
    if(first.kind!=K::Literal || first.type!="int" || cond.kind!=K::Binary || cond.binding!=B::None ||
       cond.text!="<" || cond.args.size()!=2 || !index(*cond.args[0]) || cond.args[1]->kind!=K::Literal ||
       cond.args[1]->type!="int" || (step.kind!=K::Postfix && step.kind!=K::Unary) ||
       step.text!="++" || step.binding!=B::None || step.args.size()!=1 || !index(*step.args[0])) return false;
    const auto begin=literal_value(first).as_int(), end=literal_value(*cond.args[1]).as_int();
    if(begin<0 || end-begin<8 || !scalar_loop_body(*s.body,counter)) return false;

    const auto slow=block(), available=block(), fast=block(), done=block();
    const auto flag=load(Type::I32,offset(context,offsetof(BrContext,flags)));
    const auto mask=integer(Type::I32,BR_PRIVATE_STEPS|1u);
    branch(cmp("eq",instruction(Op::And,Type::I32,{flag,mask}),mask),available,slow);
    current=available;
    const auto address=offset(context,offsetof(BrContext,steps));
    const auto remaining=load(Type::I64,address);
    const auto ticks=integer(Type::I64,end-begin+1);
    branch(cmp("uge",remaining,ticks),fast,slow);

    current=fast;
    if(auto narrow=NarrowLoop::match(*s.body)) {
        narrow->emit(*this,end-begin,slow);
        store(address,instruction(Op::Sub,Type::I64,{remaining,ticks}));
        jump(done);
    } else {
        const auto trapped=block();
        const auto outer=failure;
        failure=trapped;
        ordinary_for(s,1,false,true);
        failure=outer;
        store(address,instruction(Op::Sub,Type::I64,{remaining,ticks}));
        jump(done);
        current=trapped;
        const auto i=cast(load(Type::I32,locals.at(counter)),Type::I64,"zext");
        const auto elapsed=instruction(Op::Sub,Type::I64,{i,integer(Type::I64,begin)});
        const auto spent=instruction(Op::Add,Type::I64,{elapsed,integer(Type::I64,1)});
        store(address,instruction(Op::Sub,Type::I64,{remaining,spent}));
        jump(outer);
    }

    current=slow;
    ordinary_for(s,1);
    jump(done);
    current=done;
    return true;
}
static bool named_local(const Expr& expression, std::size_t id) {
    return expression.kind == K::Name && expression.binding == B::Local && expression.local_id == id;
}

// Sum a regularly spaced sequence of samples, wrapping at the array length.
// If the whole run spans at most one revolution, its offsets are independent
// after the first division. Each needs at most one conditional subtraction.
bool Builder::modular_sequence_sum(const Stmt& s, const Expr& limit) {
    using S = Stmt::Kind;
    const auto literal_int = [](const Expr& e) { return e.kind == K::Literal && e.type == "int"; };
    if (!s.init->expression || !literal_int(*s.init->expression) || literal_value(*s.init->expression).as_int()!=0 ||
        !literal_int(limit) || s.body->kind!=S::Block || s.body->statements.size()!=2) return false;
    const auto trips = literal_value(limit).as_int();
    if (trips < 4 || trips > INT32_MAX) return false;
    const auto& sample = *s.body->statements[0];
    const auto& update = *s.body->statements[1];
    if (sample.kind!=S::Variable || sample.type!="int" || !sample.expression ||
        update.kind!=S::Expression || !update.expression) return false;
    const auto& rem = *sample.expression;
    const auto& assignment = *update.expression;
    if (rem.kind!=K::Binary || rem.binding!=B::None || rem.text!="%" || rem.type!="int" || rem.args.size()!=2 ||
        assignment.kind!=K::Assign || assignment.binding!=B::None || assignment.text!="+=" ||
        assignment.args.size()!=2) return false;
    const auto& sum = *assignment.args[0];
    const auto& cast_expr = *assignment.args[1];
    if (sum.kind!=K::Name || sum.binding!=B::Local || is_reference(sum.declared_type) || sum.type!="ulong" ||
        cast_expr.kind!=K::Call || cast_expr.binding!=B::Cast || cast_expr.type!="ulong" || cast_expr.args.size()!=2)
        return false;
    const auto& access = *cast_expr.args[1];
    const auto& size = *rem.args[1];
    if (size.kind!=K::Member || size.binding!=B::Property || size.text!="Length" || size.args.size()!=1 ||
        access.kind!=K::Index || access.binding!=B::None || access.args.size()!=2 ||
        !named_local(*access.args[1],sample.local_id)) return false;
    const auto& source = *size.args[0];
    if (source.kind!=K::Name || source.binding!=B::Local || is_reference(source.declared_type) ||
        indexed_arrays.contains(source.local_id) ||
        (source.type!="int[]" && source.type!="Bolt.Runtime.Buffer<int>") ||
        !named_local(*access.args[0],source.local_id)) return false;
    const auto& add = *rem.args[0];
    if (add.kind!=K::Binary || add.text!="+" || add.binding!=B::None || add.type!="int" || add.args.size()!=2)
        return false;
    const auto& base = *add.args[0];
    const auto& scaled = *add.args[1];
    if (base.type!="int" || !pure_scalar_value(base) || !independent_of(base,s.init->local_id) ||
        !independent_of(base,sum.local_id) || scaled.kind!=K::Binary || scaled.binding!=B::None ||
        scaled.text!="*" || scaled.type!="int" || scaled.args.size()!=2 ||
        !named_local(*scaled.args[0],s.init->local_id) || !literal_int(*scaled.args[1])) return false;
    const auto step = literal_value(*scaled.args[1]).as_int();
    if (step < 1 || step > INT32_MAX / (trips-1)) return false;

    const auto initial = expression(base).value;
    const auto length = expression(size).value;
    const auto slow = block(), init = block(), loop = block(), verify = block(), publish = block(), done = block();
    const auto positive = cmp("sge",length,integer(Type::I32,step*(trips-1)));
    const auto in_range = cmp("ule",initial,integer(Type::I32,INT32_MAX-step*(trips-1)));
    branch(instruction(Op::And,Type::I32,{positive,in_range}),init,slow);
    current = init;
    const auto data = load_sequence_data(load(Type::Ptr,locals.at(source.local_id)));
    const auto counter=alloc(4), subtotal=alloc(8), signs=alloc(4);
    const auto start=instruction(Op::URem,Type::I32,{initial,length});
    store(counter,integer(Type::I32,0)); store(subtotal,integer(Type::I64,0)); store(signs,integer(Type::I32,0));
    jump(loop);
    current = loop;
    const auto i = load(Type::I32,counter);
    const auto displacement=instruction(Op::Mul,Type::I32,{i,integer(Type::I32,step)});
    const auto raw=instruction(Op::Add,Type::I32,{start,displacement});
    const auto wrapped=instruction(Op::Sub,Type::I32,{raw,length});
    const auto pos=instruction(Op::Select,Type::I32,{cmp("uge",raw,length),wrapped,raw});
    const auto bytes = instruction(Op::Mul,Type::I64,{cast(pos,Type::I64,"zext"),integer(Type::I64,4)});
    const auto v = load(Type::I32,instruction(Op::Offset,Type::Ptr,{data,bytes}));
    store(subtotal,instruction(Op::Add,Type::I64,{load(Type::I64,subtotal),cast(v,Type::I64,"zext")}));
    store(signs,instruction(Op::Or,Type::I32,{load(Type::I32,signs),v}));
    const auto count=instruction(Op::Add,Type::I32,{i,integer(Type::I32,1)});
    store(counter,count);
    branch(cmp("ne",count,integer(Type::I32,trips)),loop,verify);
    current = verify;
    const auto dest=locals.at(sum.local_id), prior=load(Type::I64,dest);
    const auto result=instruction(Op::Add,Type::I64,{prior,load(Type::I64,subtotal)});
    const auto valid=instruction(Op::And,Type::I32,{cmp("uge",result,prior),cmp("sge",load(Type::I32,signs),integer(Type::I32,0))});
    branch(valid,publish,slow);
    current=publish; store(dest,result); jump(done);
    // Replay checked arithmetic to preserve the accumulator at the first failing sample.
    current=slow; ordinary_for(s); jump(done);
    current=done; return true;
}

// Addition, subtraction and XOR with an invariant are permutations modulo
// every power of two. A full period therefore sets each selected bit in
// exactly half the values, regardless of the invariant's actual value.
static bool permutes_low_bits(const Expr& e, std::size_t counter) {
    if (named_local(e,counter)) return true;
    if (e.kind != K::Binary || e.binding != B::None || e.type != "int" || e.args.size() != 2 ||
        (e.text != "+" && e.text != "-" && e.text != "^")) return false;
    for (unsigned side = 0; side < 2; ++side) {
        const auto& fixed = *e.args[1-side];
        if (fixed.type == "int" && independent_of(fixed,counter) && permutes_low_bits(*e.args[side],counter))
            return true;
    }
    return false;
}

static std::optional<std::uint32_t> full_period_mask(const Expr& e, std::size_t counter, std::uint32_t count) {
    if (e.kind != K::Binary || e.binding != B::None || e.type != "int" || e.text != "&" || e.args.size() != 2)
        return {};
    for (unsigned side = 0; side < 2; ++side) {
        const auto& literal = *e.args[side];
        if (literal.kind != K::Literal || literal.type != "int") continue;
        const auto mask = static_cast<std::uint32_t>(literal_value(literal).as_int());
        if (mask > INT32_MAX || !permutes_low_bits(*e.args[1-side],counter)) continue;
        const auto period = std::uint32_t{1} << std::bit_width(mask);
        if (count % period == 0) return mask;
    }
    return {};
}

// Reduce two int fields of a dense record sequence. Nonnegative terms make
// every prefix monotone. Compute the subtotal in 64 bits and replay the
// checked source loop if a field, pair sum or accumulator may be out of range.
static bool matches_record_sum(Lower& lower, const Stmt& loop, const Expr& limit, const Expr& body) {
    const auto* initial = loop.init->expression.get();
    if (!initial || initial->kind != K::Literal || initial->type != "int" || literal_value(*initial).as_int() != 0 ||
        limit.kind != K::Member || limit.binding != B::Property || limit.arrow || limit.args.size() != 1 ||
        (limit.text != "Length" && limit.text != "Count") ||
        body.kind != K::Assign || body.binding != B::None || body.text != "+=" || body.args.size() != 2) return false;
    const auto& source = *limit.args[0];
    const auto& total = *body.args[0];
    const auto& add = *body.args[1];
    if (source.kind != K::Name || source.binding != B::Local || is_reference(source.declared_type) ||
        total.kind != K::Name || total.binding != B::Local || is_reference(total.declared_type) ||
        total.type != "int" || total.local_id == loop.init->local_id || add.kind != K::Binary ||
        add.binding != B::None || add.text != "+" || add.type != "int" || add.args.size() != 2) return false;
    const bool array = is_array(source.type) && array_rank(source.type) == 1;
    if (!array && generic_base(source.type) != "Bolt.Runtime.Buffer") return false;
    const auto record = array ? element_type(source.type) : generic_args(source.type).at(0);
    if (!lower.layout(record).plain_struct) return false;
    for (const auto& field : add.args) {
        if (field->kind != K::Member || field->binding != B::Field || field->arrow || field->is_static ||
            field->type != "int" || field->args.size() != 1 || field->owner != record) return false;
        const auto& access = *field->args[0];
        if (access.kind != K::Index || access.binding != B::None || access.type != record || access.args.size() != 2 ||
            !named_local(*access.args[0],source.local_id) || !named_local(*access.args[1],loop.init->local_id)) return false;
    }

    return true;
}

#include "../PrivateRecordRefill.inc"

bool Builder::record_field_sum(const Stmt& loop, const Expr& limit, const Expr& body) {
    if (!matches_record_sum(lower,loop,limit,body)) return false;
    const auto& source = *limit.args[0];
    const auto& total = *body.args[0];
    const auto& add = *body.args[1];
    const auto& record = add.args[0]->owner;
    const auto length = cast(expression(limit).value,Type::I64,"zext");
    const auto setup = block(), scan = block(), validate = block(), publish = block(), slow = block(), done = block();
    const auto dest = locals.at(total.local_id);
    const auto fill = record_fills.find(&loop);
    if (fill != record_fills.end() && fill->second.source == source.local_id) {
        const auto& known = fill->second;
        const auto a = known.masks.find(add.args[0]->text), b = known.masks.find(add.args[1]->text);
        if (a != known.masks.end() && b != known.masks.end()) {
            const auto max_pair = std::uint64_t{a->second} + b->second;
            if (max_pair <= INT32_MAX) {
                // Count equality excludes pre-existing elements. The producer
                // still executes all its checked stores before this reduction.
                const auto folded = block(), write_result = block(), general = block();
                branch(cmp("eq",length,integer(Type::I64,known.count)),folded,general);
                current = folded;
                const auto gain = integer(Type::I64,known.count * max_pair / 2);
                const auto final_sum = instruction(Op::Add,Type::I64,{cast(load(Type::I32,dest),Type::I64,"sext"),gain});
                branch(cmp("sle",final_sum,integer(Type::I64,INT32_MAX)),write_result,slow);
                current = write_result;
                store(dest,cast(final_sum,Type::I32,"trunc")); jump(done);
                current = general;
            }
        }
    }
    branch(cmp("ne",length,integer(Type::I64,0)),setup,done);
    current = setup;
    const auto data = load_sequence_data(load(Type::Ptr,locals.at(source.local_id)));
    const auto cursor = alloc(8), subtotal = alloc(8), signs = alloc(4);
    store(cursor,integer(Type::I64,0));
    store(subtotal,integer(Type::I64,0));
    store(signs,integer(Type::I32,0));
    jump(scan);
    current = scan;
    const auto i = load(Type::I64,cursor);
    const auto bytes = instruction(Op::Mul,Type::I64,{i,integer(Type::I64,lower.layout(record).size)});
    const auto item = instruction(Op::Offset,Type::Ptr,{data,bytes});
    const auto x = load(Type::I32,lower.field(*this,record,add.args[0]->text,item));
    const auto y = load(Type::I32,lower.field(*this,record,add.args[1]->text,item));
    const auto pair = instruction(Op::Add,Type::I32,{x,y});
    const auto sign_bits = instruction(Op::Or,Type::I32,{instruction(Op::Or,Type::I32,{x,y}),pair});
    store(signs,instruction(Op::Or,Type::I32,{load(Type::I32,signs),sign_bits}));
    store(subtotal,instruction(Op::Add,Type::I64,{load(Type::I64,subtotal),cast(pair,Type::I64,"zext")}));
    const auto next = instruction(Op::Add,Type::I64,{i,integer(Type::I64,1)});
    store(cursor,next);
    branch(cmp("ne",next,length),scan,validate,4);

    current = validate;
    const auto result = instruction(Op::Add,Type::I64,{cast(load(Type::I32,dest),Type::I64,"sext"),load(Type::I64,subtotal)});
    const auto safe = cmp("sge",load(Type::I32,signs),integer(Type::I32,0));
    branch(instruction(Op::And,Type::I32,{safe,cmp("sle",result,integer(Type::I64,INT32_MAX))}),publish,slow);
    current = publish;
    store(dest,cast(result,Type::I32,"trunc")); jump(done);
    current = slow;
    ordinary_for(loop); jump(done);
    current = done;
    return true;
}

bool Builder::optimize_counted_loop(const Stmt& s) {
    if(lower.options.optimize_loops && lower.module.instrument_budget) return budgeted_scalar_loop(s);
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget ||
        !s.init || s.init->kind != S::Variable || s.init->type != "int" ||
        !s.condition || !s.increment || !s.body) return false;

    if (auto it = projected_fills.find(&s); it != projected_fills.end()) {
        fill_projection(projected_lists.at(it->second));
        return true;
    }
    if (reverse_append_loop(s)) return true;
    const auto induction = s.init->local_id;
    auto counter = [induction](const Expr& e) {
        return e.kind == K::Name && e.binding == B::Local && e.local_id == induction;
    };
    const auto& cond = *s.condition;
    const auto& step = *s.increment;
    if (cond.kind != K::Binary || cond.binding != B::None || cond.text != "<" ||
        cond.args.size() != 2 || !counter(*cond.args[0]) || step.binding != B::None ||
        step.args.empty() || !counter(*step.args[0])) return false;
    if (step.kind == K::Assign && step.text == "+=" && step.args.size() == 2 &&
        step.args[1]->kind == K::Literal && step.args[1]->type == "int") {
        const auto stride = literal_value(*step.args[1]).as_int();
        return stride > 0 && version_array_loop(s,static_cast<std::int32_t>(stride));
    }
    if ((step.kind != K::Postfix && step.kind != K::Unary) || step.text != "++" ||
        step.args.size() != 1) return false;
    const Stmt* body = s.body.get();
    if (body->kind == S::Block && body->statements.size() == 1) body = body->statements[0].get();
    if (body->kind == S::If && optimize_search(s, *body)) return true;
    if (body->kind == S::Block && queue_work_loop(s,*cond.args[1])) return true;
    if (modular_sequence_sum(s,*cond.args[1]) || widening_sequence_sum(s,*cond.args[1])) return true;
    if ((body->kind == S::Block || body->kind == S::If) && version_array_loop(s,1)) return true;
    if (body->kind == S::Block && body->statements.size() >= 3) {
        const auto& last = *body->statements.back();
        if (last.kind == S::Expression && last.expression &&
            scalar_append_loop(s,*cond.args[1],*last.expression,body)) return true;
    }
    if (body->kind != S::Expression || !body->expression) return false;
    const auto& limit = *cond.args[1];
    return record_field_sum(s,limit,*body->expression) || projected_sum(s,limit,*body->expression) ||
           scalar_append_loop(s, limit, *body->expression) || checked_array_sum(s, limit, *body->expression);
}

// Chunk a counted append loop at its actual capacity boundaries. The inner loop
// owns only the currently free slots. Growth, allocation failure and iterator
// invalidation occur at the same Add calls as in ordinary scalar lowering.
bool Builder::scalar_append_loop(const Stmt& s, const Expr& limit, const Expr& add, const Stmt* record_body) {
    const auto induction = s.init->local_id;
    if (limit.type != "int" || (limit.kind != K::Literal &&
        (limit.kind != K::Name || limit.binding != B::Local ||
         limit.local_id == induction || is_reference(limit.declared_type)))) return false;
    if (add.kind != K::Call || add.args.size()!=2) return false;
    const auto& target = *add.args[0];
    if (target.kind != K::Member || target.arrow || target.args.size() != 1) return false;
    const bool buffer = add.binding==B::Collection && add.text=="Add" && collection_kind(add.owner)=="List";
    if (!buffer) {
        if (add.binding!=B::Method || target.text!="Add" || add.is_extension ||
            generic_base(add.owner)!="System.Collections.Generic.List") return false;
        const auto* cls=lower.program.find_class(add.owner);
        if (!cls || cls->token.at.file!="<bolt-stdlib>/System/Collections/Generic/List.bolt") return false;
    }
    const auto& object = *target.args[0];
    const auto& argument = *add.args[1];
    if (object.kind != K::Name || object.binding != B::Local || is_reference(object.declared_type)) return false;
    const auto item_type = generic_args(add.owner).at(0);
    const auto& layout = lower.layout(item_type);
    if (item_type != argument.type) return false;
    std::vector<std::pair<const Expr*,const Expr*>> affine_adds;
    if (record_body) {
        using S = Stmt::Kind;
        const auto& declaration = *record_body->statements.front();
        if (!layout.plain_struct || !layout.zero_default || layout.size > 64 ||
            declaration.kind != S::Variable || declaration.type != item_type || declaration.expression ||
            argument.kind != K::Name || argument.binding != B::Local ||
            argument.local_id != declaration.local_id) return false;
        // Building a local record can throw during scalar arithmetic, but it
        // cannot observe the deferred Count or expose a partly appended value.
        for (std::size_t n=1; n+1<record_body->statements.size(); ++n) {
            const auto& field = *record_body->statements[n];
            if (field.kind != S::Expression || !field.expression) return false;
            const auto& e = *field.expression;
            if (e.kind != K::Assign || e.text != "=" || e.binding != B::None || e.args.size() != 2 ||
                !pure_scalar_value(*e.args[1])) return false;
            const auto& lhs = *e.args[0];
            if (lhs.kind != K::Member || lhs.binding != B::Field || lhs.arrow || lhs.is_static ||
                lhs.args.size() != 1 || lhs.args[0]->binding != B::Local ||
                lhs.args[0]->local_id != declaration.local_id || !is_numeric(lhs.type)) return false;
            auto collect = [&](auto&& visit, const Expr& rhs) -> void {
                if (rhs.kind != K::Binary) return;
                if (rhs.type == "int" && rhs.text == "+" && rhs.args[0]->type == "int" && rhs.args[1]->type == "int") {
                    for (unsigned side=0; side<2; ++side) {
                        const auto& i = *rhs.args[side];
                        const auto& fixed = *rhs.args[1-side];
                        if (i.kind==K::Name && i.local_id==induction &&
                            fixed.kind==K::Name && fixed.local_id!=induction) affine_adds.emplace_back(&rhs,&fixed);
                    }
                }
                for (const auto& child : rhs.args) visit(visit,*child);
            };
            collect(collect,*e.args[1]);
        }
        const auto next = following_for.find(&s);
        if (buffer && next != following_for.end() && s.init->expression && s.init->expression->kind == K::Literal &&
            s.init->expression->type == "int" && literal_value(*s.init->expression).as_int() == 0 &&
            limit.kind == K::Literal && literal_value(limit).as_int() > 0) {
            RecordFill fill{object.local_id,static_cast<std::uint32_t>(literal_value(limit).as_int()),{}};
            for (std::size_t n = 1; n+1 < record_body->statements.size(); ++n) {
                const auto& field = *record_body->statements[n]->expression;
                const auto& lhs = *field.args[0];
                const auto mask = full_period_mask(*field.args[1],induction,fill.count);
                if (lhs.type == "int" && mask) fill.masks[lhs.text] = *mask;
                else fill.masks.erase(lhs.text);
            }
            if (!fill.masks.empty()) record_fills.insert_or_assign(next->second,std::move(fill));
        }
    } else if (!pure_scalar_value(argument) || !native_abi::scalar_storage_kind(layout.kind)) return false;

    const auto first = cleanup.size();
    statement(*s.init);
    const auto index_slot = locals.at(induction);
    const auto receiver_slot = locals.at(object.local_id);
    const auto slot = buffer ? receiver_slot : lower.field(*this,add.owner,"storage",receiver_slot);
    const auto revision = buffer ? Reg{} : lower.field(*this,add.owner,"revision",receiver_slot);
    const auto limit_value = expression(limit).value;
    const auto outer = block(), inspect = block(), capacity_check = block(), choose = block();
    const auto clamp = block(), start_chunk = block(), inner = block(), commit = block();
    const auto scalar = block(), done = block(), failed = block();
    const auto chunk_size = alloc(8);
    jump(outer);
    current = outer;
    const auto index = load(Type::I32, index_slot);
    branch(cmp("slt", index, limit_value), inspect, done);
    current = inspect;
    const auto header = load(Type::Ptr, slot);
    const auto observed = load(Type::I64, offset(context, offsetof(BrContext, active_ranges)),"execution context");
    const auto slow = instruction(Op::Or, Type::I32, {
        cmp("eq", header, null()), cmp("ne", observed, integer(Type::I64, 0))});
    branch(slow, scalar, capacity_check);
    current = scalar;
    // A scalar iteration also handles the very first Add and an active foreach.
    if (record_body) statement(*record_body);
    else (void)expression(add);
    (void)expression(*s.increment);
    jump(outer);

    current = capacity_check;
    const auto count_address = offset(header, offsetof(BrSequenceHeader, count));
    const auto count = load_buffer_size(count_address);
    const auto capacity = load_buffer_size(offset(header, offsetof(BrSequenceHeader, capacity)));
    branch(cmp("ult", count, capacity), choose, scalar);

    current = choose;
    const auto start = cast(index, Type::I64, "sext");
    const auto remaining = instruction(Op::Sub, Type::I64, {cast(limit_value, Type::I64, "sext"), start});
    const auto spare = instruction(Op::Sub, Type::I64, {capacity, count});
    store(chunk_size, remaining);
    branch(cmp("ult", spare, remaining), clamp, start_chunk);
    current = clamp;
    store(chunk_size, spare);
    jump(start_chunk);
    current = start_chunk;
    const auto length = load(Type::I64, chunk_size);
    const auto initial_revision = revision ? load(Type::I64,revision) : Reg{};
    const auto data = load_sequence_data(header);
    const auto stop = cast(instruction(Op::Add, Type::I64, {start, length}), Type::I32, "trunc");
    if (affine_adds.empty()) jump(inner);
    else {
        // A single wide check covers each i + invariant throughout the chunk.
        // If an endpoint is unsafe, run one ordinary checked Add and retry.
        const auto last = instruction(Op::Sub,Type::I64,{cast(stop,Type::I64,"sext"),integer(Type::I64,1)});
        auto safe = integer(Type::I32,1);
        for (const auto& [operation,fixed] : affine_adds) {
            const auto bias = cast(load(Type::I32,locals.at(fixed->local_id)),Type::I64,"sext");
            const auto lo = instruction(Op::Add,Type::I64,{start,bias});
            const auto hi = instruction(Op::Add,Type::I64,{last,bias});
            safe = instruction(Op::And,Type::I32,{safe,cmp("sge",lo,integer(Type::I64,INT32_MIN))});
            safe = instruction(Op::And,Type::I32,{safe,cmp("sle",hi,integer(Type::I64,INT32_MAX))});
        }
        branch(safe,inner,scalar);
    }

    current = inner;
    const auto previous_failure = failure;
    failure = failed;
    const auto i = load(Type::I32, index_slot);
    const auto completed = instruction(Op::Sub, Type::I64, {cast(i, Type::I64, "sext"), start});
    const auto position = instruction(Op::Add, Type::I64, {count, completed});
    const auto bytes = instruction(Op::Mul, Type::I64, {position, integer(Type::I64, lower.layout(item_type).size)});
    const auto destination = instruction(Op::Offset, Type::Ptr, {data, bytes});
    if (record_body) {
        const auto scope = cleanup.size();
        std::vector<const Expr*> enabled;
        for (const auto& [operation,fixed] : affine_adds)
            if (affine_ops.insert(operation).second) enabled.push_back(operation);
        for (std::size_t n=0; n+1<record_body->statements.size(); ++n)
            statement(*record_body->statements[n]);
        for (const auto* operation : enabled) affine_ops.erase(operation);
        copy_value(item_type,destination,locals.at(argument.local_id));
        drop_since(scope);
    } else store(destination,expression(argument).value);
    // i < stop <= limit <= INT_MAX proves this increment is representable.
    const auto next = instruction(Op::Add, Type::I32, {i, integer(Type::I32, 1)});
    store(index_slot, next);
    branch(cmp("ne", next, stop), inner, commit);

    current = commit;
    store(count_address, instruction(Op::Add, Type::I64, {count, length}));
    if (revision) store(revision,instruction(Op::Add,Type::I64,{initial_revision,length}));
    jump(outer);
    current = failed;
    // A checked calculation can fail in the middle of a chunk. Previously
    // appended elements must be visible to handlers and cleanup.
    store(count_address, position);
    if (revision) store(revision,instruction(Op::Add,Type::I64,{initial_revision,completed}));
    jump(previous_failure);
    failure = previous_failure;
    current = done;
    drop_since(first);
    return true;
}

// A checked unsigned sum can be reassociated for nonnegative inputs. Validate
// the complete fast result before publishing it. If any check may fail, replay
// the ordinary scalar loop so handlers see the exact source location and prefix.
bool Builder::checked_array_sum(const Stmt& s, const Expr& limit, const Expr& body) {
    if (limit.kind != K::Member || limit.binding != B::Property || limit.arrow ||
        (limit.text != "Length" && limit.text != "Count") ||
        !is_array(limit.owner) || array_rank(limit.owner) != 1 || limit.args.size() != 1)
        return false;
    const auto& array = *limit.args[0];
    if (array.kind != K::Name || array.binding != B::Local || is_reference(array.declared_type) ||
        body.kind != K::Assign || body.text != "+=" || body.args.size() != 2 || body.binding != B::None)
        return false;
    const auto& accumulator = *body.args[0];
    if (accumulator.kind != K::Name || accumulator.binding != B::Local ||
        accumulator.type != "ulong" || is_reference(accumulator.declared_type)) return false;
    const auto& rhs = *body.args[1];
    const Expr* access = &rhs;
    if (rhs.kind == K::Call && rhs.binding == B::Cast && rhs.type == "ulong" && rhs.args.size() == 2)
        access = rhs.args[1].get();
    if (access->kind != K::Index || access->args.size() != 2 ||
        access->args[0]->binding != B::Local || access->args[0]->local_id != array.local_id ||
        access->args[1]->binding != B::Local || access->args[1]->local_id != s.init->local_id)
        return false;
    if (!is_integer(access->type)) return false;
    const auto& element = lower.layout(access->type);
    if ((element.kind != native_abi::Signed && element.kind != native_abi::Unsigned) ||
        element.size > 4 || element_type(array.type) != access->type) return false;

    const auto first = cleanup.size();
    statement(*s.init);
    const auto index_slot = locals.at(s.init->local_id);
    const auto total_slot = locals.at(accumulator.local_id);
    const auto length = expression(limit).value; // Includes null/default and Length's int overflow check.
    const auto index = load(Type::I32, index_slot);
    const auto inspect = block(), start = block(), scan = block(), validate = block();
    const auto publish = block(), fallback = block(), replay = block(), done = block();
    branch(cmp("slt", index, length), inspect, done);
    current = inspect;
    branch(cmp("slt", index, integer(Type::I32, 0)), fallback, start);

    current = start;
    const auto indexed = indexed_arrays.find(array.local_id);
    const auto data = indexed == indexed_arrays.end()
        ? load(Type::Ptr, offset(load(Type::Ptr, locals.at(array.local_id)), offsetof(BrSequenceHeader, data)))
        : Reg{};
    const auto cursor = alloc(8), sum = alloc(8), signs = alloc(element.size);
    const auto unsigned_length = cast(length, Type::I64, "zext");
    store(cursor, cast(index, Type::I64, "zext"));
    store(sum, integer(Type::I64, 0));
    store(signs, integer(element.ir, 0));
    if (indexed != indexed_arrays.end() && indexed->second.complementary_sum) {
        // The producer proof excludes negative elements and checked-add overflow.
        // N <= INT_MAX, so N * (first + last) fits in uint64.
        const auto last = instruction(Op::Sub, Type::I32, {length,integer(Type::I32,1)});
        const auto low = instruction(Op::And, Type::I32, {length,last});
        const auto zero = integer(Type::I32,0);
        const auto closed = block();
        branch(instruction(Op::And, Type::I32, {cmp("eq",index,zero),cmp("eq",low,zero)}),closed,scan);
        current = closed;
        const auto first_value = indexed_element(indexed->second,zero).value;
        const auto last_value = indexed_element(indexed->second,last).value;
        const auto pair = instruction(Op::Add,Type::I64,{cast(first_value,Type::I64,"zext"),
                                                        cast(last_value,Type::I64,"zext")});
        const auto twice = instruction(Op::Mul,Type::I64,{unsigned_length,pair});
        store(sum,instruction(Op::LShr,Type::I64,{twice,integer(Type::I64,1)}));
        jump(validate);
    } else jump(scan);

    current = scan;
    const auto i = load(Type::I64, cursor);
    Reg value;
    if (indexed != indexed_arrays.end()) {
        value = indexed_element(indexed->second, cast(i, Type::I32, "trunc")).value;
    } else {
        const auto bytes = instruction(Op::Mul, Type::I64, {i, integer(Type::I64, element.size)});
        value = load(element.ir, instruction(Op::Offset, Type::Ptr, {data, bytes}));
    }
    if (element.kind == native_abi::Signed)
        store(signs, instruction(Op::Or, element.ir, {load(element.ir, signs), value}));
    // Length <= INT_MAX and zero-extended element <= UINT32_MAX. This
    // independent sum fits uint64 even if one of the original inputs is negative.
    store(sum, instruction(Op::Add, Type::I64, {load(Type::I64, sum), cast(value, Type::I64, "zext")}));
    const auto next = instruction(Op::Add, Type::I64, {i, integer(Type::I64, 1)});
    store(cursor, next);
    branch(cmp("ne", next, unsigned_length), scan, validate, element.size);
    current = validate;
    const auto initial = load(Type::I64, total_slot);
    const auto result = instruction(Op::Add, Type::I64, {initial, load(Type::I64, sum)});
    auto invalid = cmp("ult", result, initial);
    if (element.kind == native_abi::Signed)
        invalid = instruction(Op::Or, Type::I32, {invalid, cmp("slt", load(element.ir, signs), integer(element.ir, 0))});
    branch(invalid, fallback, publish);

    current = publish;
    store(total_slot, result);
    store(index_slot, length);
    jump(done);
    current = fallback;
    branch(expression(*s.condition).value, replay, done);
    current = replay;
    (void)expression(body);
    (void)expression(*s.increment);
    jump(fallback);
    current = done;
    drop_since(first);
    return true;
}

} // namespace bolt::ir::lowering
