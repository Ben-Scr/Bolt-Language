#include "Lowering.hpp"
#include <bolt/Value.hpp>
#include <algorithm>
#include <bit>
#include <optional>

namespace bolt::ir::lowering {

// Record a plain conditional exchange. Its original branch stays intact
// unless the complete function proves that both storage slots are private.
bool Builder::lower_pointer_swap(const Stmt& s) {
    using S=Stmt::Kind;
    if (!lower.options.optimize_loops || loops.empty() || s.alternative || !s.body ||
        s.body->kind!=S::Block || s.body->statements.size()!=3 || s.condition->type!="bool") return false;
    const auto& stmts=s.body->statements;
    const auto& temp=*stmts[0];
    auto plain_local=[](const Expr& e) {
        return e.kind==K::Name && e.binding==B::Local && !is_reference(e.declared_type);
    };
    if (temp.kind!=S::Variable || !is_pointer(temp.type) || !temp.expression || !plain_local(*temp.expression) ||
        temp.expression->type!=temp.type) return false;
    auto assignment=[&](const Stmt& st)->const Expr* {
        if (st.kind!=S::Expression || !st.expression) return nullptr;
        const auto& e=*st.expression;
        if (e.kind!=K::Assign || e.binding!=B::None || e.text!="=" || e.args.size()!=2 ||
            !plain_local(*e.args[0]) || !plain_local(*e.args[1]) ||
            e.args[0]->type!=temp.type || e.args[1]->type!=temp.type) return nullptr;
        return &e;
    };
    const auto* first=assignment(*stmts[1]); const auto* second=assignment(*stmts[2]);
    if (!first || !second) return false;
    const auto left=temp.expression->local_id,right=first->args[1]->local_id;
    if (left==right || right==temp.local_id || first->args[0]->local_id!=left ||
        second->args[0]->local_id!=right || second->args[1]->local_id!=temp.local_id) return false;
    const Reg a=locals.at(left),b=locals.at(right),cond=expression(*s.condition).value;
    const Label header=current,yes=block(),done=block();
    branch(cond,yes,done);current=yes;
    const Reg x=load(Type::Ptr,a),y=load(Type::Ptr,b);
    store(a,y);store(b,x);jump(done);current=done;
    conditional_swaps.push_back({header,done,cond,a,b});
    return true;
}
void Builder::promote_private_swaps() {
    if (conditional_swaps.empty()) return;
    std::set<Reg> private_slots;
    for (const auto& s:conditional_swaps) {private_slots.insert(s.first);private_slots.insert(s.second);}
    for (const auto& block:fn.blocks) for (const auto& i:block.code) {
        for (std::size_t n=0;n<i.args.size();++n)
            if (n!=0 || (i.op!=Op::Load && i.op!=Op::Store && i.op!=Op::Zero)) private_slots.erase(i.args[n]);
    }
    const auto saved=current;
    for (const auto& s:conditional_swaps) {
        if (!private_slots.contains(s.first) || !private_slots.contains(s.second)) continue;
        current=s.header;
        fn.blocks[current].code.pop_back(); // replaces the provisional conditional terminator
        const Reg x=load(Type::Ptr,s.first),y=load(Type::Ptr,s.second);
        store(s.first,instruction(Op::Select,Type::Ptr,{s.condition,y,x}));
        store(s.second,instruction(Op::Select,Type::Ptr,{s.condition,x,y}));
        jump(s.done);
    }
    current=saved;
}
// A new array has stable storage and length while its owning array local
// cannot be replaced or borrowed. Track an optional upper bound for every
// element as well: fresh integers start at zero and a bounded store can only
// enlarge that bound. Unknown element uses discard this latter proof.
void Builder::find_private_arrays(const Stmt& scope) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget) return;
    std::unordered_map<std::size_t, PrivateArray> candidates;
    auto literal = [](const Expr& e) -> std::optional<std::uint32_t> {
        if (e.kind != K::Literal || (e.type != "int" && e.type != "uint")) return {};
        const auto v = literal_value(e).as_int();
        if (v < 0 || v > INT32_MAX) return {};
        return static_cast<std::uint32_t>(v);
    };
    auto consider = [&](const Stmt& st) {
        const auto* e = st.expression.get();
        if (st.kind != S::Variable || !is_array(st.type) || array_rank(st.type) != 1 || !e) return;
        const bool allocation = e->kind == K::New && e->arrow && e->args.size() == 1;
        const auto element = element_type(st.type);
        // A fresh fixed-length array keeps its data address even when a method
        // changes one of its elements. The scan below disqualifies any use of
        // the owning handle that could replace it. Element construction, copy
        // and destruction still follow the ordinary value-type paths.
        if (!allocation && (!is_numeric(element) || e->kind!=K::Call || e->type!=st.type || !fresh_handle(*e))) return;
        auto& candidate = candidates[st.local_id];
        candidate.nullable = !allocation;
        candidate.confined_elements = allocation;
        if (!allocation || (st.type != "int[]" && st.type != "uint[]")) candidate.maximum.reset();
    };
    for (const auto& st : scope.statements) {
        if (st->kind == S::Declarations) {
            for (const auto& variable : st->statements) consider(*variable);
        } else consider(*st);
    }
    if (candidates.empty()) return;
    auto bound = [&](const Expr& e) -> std::optional<std::uint32_t> {
        if (e.kind == K::Binary && e.binding == B::None && e.text == "&" && e.args.size() == 2 &&
            (e.type == "int" || e.type == "uint")) {
            if (auto mask = literal(*e.args[0])) return mask;
            return literal(*e.args[1]);
        }
        return literal(e);
    };
    enum class Use { Unknown, Value, Store };
    auto inspect = [&](auto&& visit, const Expr& e, Use use, const Expr* rhs, bool by_value) -> void {
        if (e.kind == K::Member && e.binding == B::Property && e.text == "Length" &&
            e.args.size() == 1 && e.args[0]->kind == K::Name && e.args[0]->binding == B::Local &&
            candidates.contains(e.args[0]->local_id)) return;
        if (e.kind == K::Name && e.binding == B::Local) {
            candidates.erase(e.local_id);
            return;
        }
        if (e.kind == K::Index && e.args.size() == 2) {
            const auto& base = *e.args[0];
            if (base.kind == K::Name && base.binding == B::Local) {
                auto it = candidates.find(base.local_id);
                if (it != candidates.end()) {
                    auto& maximum = it->second.maximum;
                    if (!by_value) it->second.confined_elements = false;
                    if (use == Use::Unknown) maximum.reset();
                    if (use == Use::Store && maximum) {
                        auto value = bound(*rhs);
                        if (value) maximum = std::max(*maximum, *value);
                        else maximum.reset();
                    }
                    visit(visit, *e.args[1], Use::Unknown, nullptr, true);
                    return;
                }
            }
        }
        if (e.kind == K::Assign && e.binding == B::None && e.text == "=" && e.args.size() == 2) {
            visit(visit, *e.args[0], Use::Store, e.args[1].get(), by_value);
            visit(visit, *e.args[1], Use::Value, nullptr, is_numeric(e.type));
            return;
        }
        if (e.arena) visit(visit, *e.arena, Use::Unknown, nullptr, false);
        const bool scalar = e.binding == B::None && (is_numeric(e.type) || e.type == "bool");
        const bool consumes = scalar && ((e.kind == K::Binary && e.text != "," && e.text != "??") ||
            (e.kind == K::Postfix && by_value) || (e.kind == K::Unary && (e.text == "-" || e.text == "+" ||
            e.text == "~" || e.text == "!")));
        for (const auto& a : e.args) visit(visit, *a, Use::Unknown, nullptr, consumes);
    };
    auto scan = [&](auto&& visit, const Stmt& st) -> void {
        if (st.expression) inspect(inspect,*st.expression,Use::Unknown,nullptr,
            st.kind == S::Expression || (st.kind == S::Variable && is_numeric(st.type)));
        for (const auto* e : {st.condition.get(), st.increment.get()})
            if (e) inspect(inspect,*e,Use::Unknown,nullptr,true);
        for (const auto* child : {st.init.get(), st.body.get(), st.alternative.get()})
            if (child) visit(visit, *child);
        for (const auto& child : st.statements) visit(visit, *child);
    };
    scan(scan, scope);
    private_arrays.insert(candidates.begin(), candidates.end());
    if (!candidates.empty()) find_frozen_tables(scope);
}

// Give LLVM a non-aliasing, stack-sized view of small tables initialized by a
// straight-line series of stores. The owning array remains available to other
// lowering paths. Only primitive value reads may follow the initialization.
void Builder::find_frozen_tables(const Stmt& scope) {
    using S = Stmt::Kind;
    for (std::size_t i = 0; i + 1 < scope.statements.size(); ++i) {
        const auto& decl = *scope.statements[i];
        const auto* init = decl.expression.get();
        if (decl.kind != S::Variable || !private_arrays.contains(decl.local_id) || !is_numeric(element_type(decl.type)) || !init ||
            init->kind != K::New || !init->arrow || init->args.size() != 1 ||
            init->args[0]->kind != K::Literal || init->args[0]->type != "int") continue;
        const auto count = literal_value(*init->args[0]).as_int();
        if (count <= 0 || count > 8) continue;
        const auto bytes = static_cast<std::size_t>(count) * lower.layout(element_type(decl.type)).size;
        if (table_bytes + bytes > 1024) continue;
        auto index_of_table = [&](const Expr& e) {
            return e.kind == K::Index && e.args.size() == 2 && e.args[0]->binding == B::Local &&
                   e.args[0]->local_id == decl.local_id;
        };
        auto safe = [&](auto&& self, const Expr& e, bool value) -> bool {
            if (index_of_table(e)) return value && self(self,*e.args[1],true);
            if (e.binding == B::Local && e.local_id == decl.local_id) return false;
            if (e.arena && !self(self,*e.arena,false)) return false;
            for (std::size_t n = 0; n < e.args.size(); ++n) {
                const bool scalar = e.binding == B::None &&
                    ((e.kind == K::Binary && e.text != "," && e.text != "??") ||
                     (e.kind == K::Index && n == 1) || (e.kind == K::Assign && n == 1) ||
                     (e.kind == K::Conditional && (value || n == 0)));
                if (!self(self,*e.args[n],scalar)) return false;
            }
            return true;
        };
        std::size_t end = i + 1;
        while (end < scope.statements.size()) {
            const auto& st = *scope.statements[end];
            const auto* e = st.expression.get();
            if (st.kind != S::Expression || !e || e->kind != K::Assign || e->binding != B::None ||
                e->text != "=" || e->args.size() != 2 || !index_of_table(*e->args[0]) ||
                e->args[0]->args[1]->kind != K::Literal || !safe(safe,*e->args[1],true)) break;
            ++end;
        }
        if (end == i + 1) continue;
        auto read_only = [&](auto&& self,const Stmt& st) -> bool {
            if (st.expression && !safe(safe,*st.expression,st.kind == S::Variable && !is_reference(st.type))) return false;
            for (auto* e : {st.condition.get(),st.increment.get()}) if (e && !safe(safe,*e,true)) return false;
            for (auto* child : {st.init.get(),st.body.get(),st.alternative.get()})
                if (child && !self(self,*child)) return false;
            return std::all_of(st.statements.begin(),st.statements.end(),[&](const auto& s){return self(self,*s);});
        };
        if (std::all_of(scope.statements.begin()+end,scope.statements.end(),[&](const auto& s){return read_only(read_only,*s);})) {
            frozen_tables.emplace(scope.statements[end-1].get(),FrozenTable{decl.local_id,bytes});
            table_bytes += bytes;
        }
    }
}

// Only direct local declarations in the same lexical block qualify. A local
// that appears anywhere except a safe receiver (including address-taking or
// references in another branch, loop, handler or finalizer) disqualifies it.
void Builder::find_consumed_snapshots(const Stmt& block) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget) return;
    const auto possible = [](const auto& st) {
        const auto& e = st->expression;
        return st->kind == S::Variable && e && e->kind == K::Call &&
               e->binding == B::Collection && e->text == "ToArray" && is_array(st->type);
    };
    if (std::none_of(block.statements.begin(), block.statements.end(), possible)) return;

    std::map<std::size_t, const Expr*> buffers;
    auto inspect = [&](auto&& visit, const Expr& e) -> void {
        if (e.kind == K::Name && e.binding == B::Local) {
            buffers.erase(e.local_id);
            return;
        }
        if (e.arena) visit(visit, *e.arena);
        // Add and Reserve neither expose the local nor return an element. Once
        // a snapshot has been seen, even these uses must invalidate the move.
        if (e.kind == K::Call && e.binding == B::Collection &&
            (e.text == "Add" || e.text == "Reserve") && !e.args.empty()) {
            const auto& target = *e.args[0];
            if (target.kind == K::Member && !target.arrow && target.args.size() == 1) {
                const auto& receiver = *target.args.front();
                auto it = buffers.find(receiver.local_id);
                if (receiver.kind == K::Name && receiver.binding == B::Local &&
                    it != buffers.end() && !it->second) {
                    for (std::size_t i = 1; i < e.args.size(); ++i) visit(visit, *e.args[i]);
                    return;
                }
            }
        }
        for (const auto& arg : e.args) visit(visit, *arg);
    };
    auto scan = [&](auto&& visit, const Stmt& s) -> void {
        for (const auto* expr : {s.expression.get(), s.condition.get(), s.increment.get()})
            if (expr) inspect(inspect, *expr);
        for (const auto* child : {s.init.get(), s.body.get(), s.alternative.get()})
            if (child) visit(visit, *child);
        for (const auto& child : s.statements) visit(visit, *child);
    };
    for (const auto& st : block.statements) {
        if (st->kind == S::Variable && !st->expression && is_collection(st->type) &&
            collection_kind(st->type) == "List" &&
            native_abi::scalar_storage_kind(lower.layout(generic_args(st->type).front()).kind)) {
            buffers.emplace(st->local_id, nullptr);
        } else if (possible(st)) {
            const auto& e = *st->expression;
            const auto& target = *e.args.front();
            if (target.kind == K::Member && !target.arrow && target.args.size() == 1) {
                const auto& receiver = *target.args.front();
                auto it = buffers.find(receiver.local_id);
                if (receiver.kind == K::Name && receiver.binding == B::Local &&
                    it != buffers.end() && !it->second && e.type == st->type) {
                    it->second = &e;
                    continue;
                }
            }
            scan(scan, *st);
        } else scan(scan, *st);
    }
    for (const auto& [id, snapshot] : buffers)
        if (snapshot) consumed_snapshots.insert(snapshot);
}

Value Builder::take_snapshot(const Expr& e, Reg object) {
    // Both native representations are an owning Sequence. Rank-one arrays use
    // its count as their shape. Keep the ordinary copy for a null or partially
    // filled buffer so the result retains exactly the same allocation capacity.
    auto output = temp(e.type);
    auto header = load(Type::Ptr, object);
    auto inspect = block(), move = block(), copy = block(), done = block();
    branch(cmp("eq", header, null()), copy, inspect);
    current = inspect;
    auto count = load(Type::I64, offset(header, offsetof(BrSequenceHeader, count)));
    auto capacity = load_buffer_size(offset(header, offsetof(BrSequenceHeader, capacity)));
    branch(cmp("eq", count, capacity), move, copy);
    current = move;
    store(output, header);
    store(object, null());
    jump(done);
    current = copy;
    call("br_collection", Type::Void, {context, site(), type(e.owner), object,
                                       string("ToArray"), output, null(), null()});
    jump(done);
    current = done;
    return {e.type, output};
}

#include "../IndexedSnapshot.inc"
#include "../ListProjection.inc"
#include "../PairedListEdit.inc"

static bool integer_local(const Expr& e) {
    return e.kind == K::Name && e.binding == B::Local && is_integer(e.type) && !is_reference(e.declared_type);
}

static bool utf8_store_index(const Expr& e) {
    return reads_integer_value(e) || ((e.kind == K::Postfix || e.kind == K::Unary) &&
        e.binding == B::None && (e.text == "++" || e.text == "--") &&
        e.args.size() == 1 && integer_local(*e.args[0]));
}

// Scalar-replace a non-escaping byte snapshot when its sole write is read
// back only at the same unchanged index. Everything after conversion up to
// the end of this lexical scope must be a side-effect-free scalar operand
// or an update to a separate, ordinary numeric local.
bool Builder::single_utf8_edit(const Stmt& scope,std::size_t start,std::size_t finish,const Expr& source) {
    using S = Stmt::Kind;
    const auto& statements=scope.statements;
    if (finish < start+2 || statements[start]->expression->arena ||
        statements[finish]->expression->args[1]->arena) return false;
    const auto id=statements[start]->local_id;
    const auto& store_stmt=*statements[finish-1];
    if (store_stmt.kind!=S::Expression || !store_stmt.expression) return false;
    const auto& write=*store_stmt.expression;
    if (write.kind!=K::Assign || write.text!="=" || write.args.size()!=2) return false;
    const auto& dest=*write.args[0];
    if (dest.kind!=K::Index || dest.args.size()!=2 || !local_name(*dest.args[0],id)) return false;
    const auto& pos=*dest.args[1];
    if (pos.type!="int" || (!integer_local(pos) && pos.kind!=K::Literal)) return false;
    std::vector<const Expr*> forwarded;
    auto scalar = [&](auto&& visit,const Expr& e,bool allow_reads) -> bool {
        if (integer_local(e) || (e.kind==K::Literal && is_integer(e.type))) return true;
        if (allow_reads && e.kind==K::Member && e.binding==B::Property && e.text=="Length" &&
            e.args.size()==1 && local_name(*e.args[0],source.local_id)) return true;
        if (allow_reads && e.kind==K::Index && e.binding==B::None && e.args.size()==2 && local_name(*e.args[0],id)) {
            const auto& key=*e.args[1];
            if (!(pos.kind==K::Literal ? key.kind==K::Literal && key.type==pos.type && key.text==pos.text :
                  local_name(key,pos.local_id))) return false;
            forwarded.push_back(&e);
            return true;
        }
        if (e.kind==K::Call && e.binding==B::Cast && is_integer(e.type) && e.args.size()==2)
            return visit(visit,*e.args[1],allow_reads);
        return e.kind==K::Binary && e.binding==B::None && is_integer(e.type) && e.args.size()==2 &&
               visit(visit,*e.args[0],allow_reads) && visit(visit,*e.args[1],allow_reads);
    };
    for (auto i=start+1;i+1<finish;++i) {
        const auto& st=*statements[i];
        if (st.kind!=S::Variable || !is_integer(st.type) || !st.expression ||
            !scalar(scalar,*st.expression,false)) return false;
    }
    if (!scalar(scalar,*write.args[1],false)) return false;
    for (auto i=finish+1;i<statements.size();++i) {
        const auto& st=*statements[i];
        if (st.kind!=S::Expression || !st.expression) return false;
        const auto& update=*st.expression;
        if (update.kind!=K::Assign || update.binding!=B::None || update.args.size()!=2 ||
            !integer_local(*update.args[0]) || (pos.kind==K::Name && local_name(*update.args[0],pos.local_id)) ||
            !scalar(scalar,*update.args[1],true)) return false;
    }
    const auto text_slot=locals.at(source.local_id);
    const auto length=byte_length(text_slot).value;
    for (auto i=start+1;i+1<finish;++i) statement(*statements[i]);
    const auto index=expression(pos).value;
    at=dest.token.at;
    guard(cmp("uge",index,length),"index out of range");
    const auto value=converted(expression(*write.args[1]),"byte").value;
    at=statements[finish]->expression->args[1]->token.at;
    call("br_edit_utf8_byte",Type::Void,{context,site(),text_slot,cast(index,Type::I64,"zext"),value});
    for (const auto* read : forwarded) forwarded_utf8_reads.emplace(read,value);
    for (auto i=finish+1;i<statements.size();++i) statement(*statements[i]);
    for (const auto* read : forwarded) forwarded_utf8_reads.erase(read);
    return true;
}

// From a fresh local ToUtf8 snapshot to assignment back to its source, track
// the actual checked indices. An escape, call, other writer or control-flow
// join ends the proof. No metadata or write barrier is needed on other arrays.
std::size_t Builder::lower_utf8_edits(const Stmt& scope, std::size_t start) {
    using S = Stmt::Kind;
    const auto& statements = scope.statements;
    const auto& snapshot = *statements[start];
    if (!lower.options.optimize_loops || lower.core() || lower.module.instrument_budget ||
        snapshot.kind != S::Variable || snapshot.type != "byte[]" || !snapshot.expression)
        return 0;
    const auto& encode = *snapshot.expression;
    if (encode.kind != K::Call || encode.binding != B::Builtin || encode.owner != "String" ||
        encode.text != "ToUtf8" || encode.args.size() != 1) return 0;
    const auto& member = *encode.args.front();
    if (member.kind != K::Member || member.arrow || member.args.size() != 1) return 0;
    const auto& source = *member.args.front();
    if (source.kind != K::Name || source.binding != B::Local || source.type != "string" ||
        is_reference(source.declared_type)) return 0;

    std::size_t finish = start + 1;
    for (; finish < statements.size(); ++finish) {
        const auto& st = *statements[finish];
        const auto* e = st.expression.get();
        if (st.kind == S::Variable && is_integer(st.type) && e && reads_integer_value(*e)) continue;
        if (st.kind != S::Expression || !e || e->kind != K::Assign ||
            e->binding != B::None || e->args.size() != 2) return 0;
        const auto& lhs = *e->args[0];
        const auto& rhs = *e->args[1];
        if (integer_local(lhs) && reads_integer_value(rhs)) continue;
        if (local_name(lhs, source.local_id) && e->text == "=" &&
            rhs.kind == K::Call && rhs.binding == B::Builtin && rhs.owner == "String" &&
            rhs.text == "FromUtf8" && rhs.args.size() == 2 &&
            local_name(*rhs.args[1], snapshot.local_id)) break;
        if (lhs.kind != K::Index || lhs.binding != B::None || lhs.owner != "byte[]" ||
            lhs.args.size() != 2 || !local_name(*lhs.args[0], snapshot.local_id) ||
            !utf8_store_index(*lhs.args[1]) || !reads_integer_value(rhs)) return 0;
    }
    if (finish == statements.size()) return 0;
    if (single_utf8_edit(scope,start,finish,source)) return statements.size()-start;

    statement(snapshot);
    Reg first = 0, last = 0;
    for (auto i = start + 1; i < finish; ++i) {
        const auto& st = *statements[i];
        if (st.kind == S::Variable || st.expression->args[0]->kind != K::Index) {
            statement(st);
            continue;
        }
        utf8_write_index = {st.expression->args[0].get(), 0};
        statement(st);
        const Reg pos = utf8_write_index.second;
        utf8_write_index = {};
        if (!first) first = last = pos;
        else {
            first = instruction(Op::Select, Type::I64, {cmp("ult", pos, first), pos, first});
            last = instruction(Op::Select, Type::I64, {cmp("ugt", pos, last), pos, last});
        }
    }
    at = statements[finish]->expression->args[1]->token.at;
    const auto end = last ? instruction(Op::Add, Type::I64, {last, integer(Type::I64, 1)}) : integer(Type::I64, 0);
    call("br_replace_utf8_edits", Type::Void, {context, site(), locals.at(source.local_id),
         locals.at(snapshot.local_id), first ? first : end, end});
    return finish - start + 1;
}

// Reading these scalar expressions cannot mutate either memory range. In
// particular, a user-defined operator, conversion or getter ends the proof.
static bool memory_scalar_read(const Expr& e) {
    if (!(is_integer(e.type) || is_pointer(e.type) || e.type == "null")) return false;
    if (e.kind == K::Literal || (e.kind == K::Name && e.binding == B::Local)) return true;
    if (e.kind == K::Call)
        return e.binding == B::Cast && e.args.size() == 2 && memory_scalar_read(*e.args[1]);
    if (e.binding != B::None) return false;
    if (e.kind == K::Binary && e.args.size() == 2)
        return memory_scalar_read(*e.args[0]) && memory_scalar_read(*e.args[1]);
    if (e.kind == K::Unary && e.args.size() == 1 && e.text != "++" && e.text != "--")
        return memory_scalar_read(*e.args[0]);
    if (e.kind == K::Index && e.args.size() == 2 && is_pointer(e.args[0]->type))
        return memory_scalar_read(*e.args[0]) && memory_scalar_read(*e.args[1]);
    return false;
}

std::size_t Builder::lower_memory_transfers(const Stmt& block, std::size_t index) {
    using S = Stmt::Kind;
    const auto& children = block.statements;
    const auto& first = *children[index];
    if (index + 1 >= children.size() || first.kind != S::Expression || !first.expression ||
        first.expression->kind != K::Call || !lower.options.optimize_loops || lower.module.instrument_budget)
        return 0;
    auto stdlib_operation = [&](const Expr* e, const char* name) {
        if (!e || e->kind != K::Call || e->binding != B::Method || e->args.size() != 4 ||
            (e->owner != "System.Memory.Memory" && e->owner != "Bolt.Runtime.MemoryOps")) return false;
        const auto* c = lower.program.find_class(e->owner);
        if (!c || !c->token.at.file.starts_with("<bolt-stdlib>")) return false;
        const auto* method = method_by_id(*c, e->resolved);
        return method && method->is_static && method->name == name &&
            std::all_of(e->args.begin() + 1, e->args.end(), [](const auto& a) { return memory_scalar_read(*a); });
    };
    MemoryRegion region;
    std::size_t next = index;
    auto operation = [&](const char* name) -> const Expr* {
        if (next >= children.size() || children[next]->kind != S::Expression) return nullptr;
        const auto* e = children[next]->expression.get();
        if (!stdlib_operation(e,name)) return nullptr;
        ++next;
        return e;
    };
    if (!lower.core()) region.fill = operation("Set");
    region.copy = operation("Copy");
    if (!region.fill && !region.copy) return 0;

    const auto* write = next < children.size() ? children[next]->expression.get() : nullptr;
    if (write && children[next]->kind == S::Expression && write->kind == K::Assign &&
        write->binding == B::None && write->text == "=" && write->type == "byte" &&
        write->args.size() == 2 && memory_scalar_read(*write->args[0]) && memory_scalar_read(*write->args[1])) {
        region.write = write;
        ++next;
    }
    bool can_move = region.fill != nullptr;
    if (region.copy && next < children.size()) {
        const Stmt* consumer = children[next].get();
        if (consumer->kind == S::Declarations && consumer->statements.size() == 1)
            consumer = consumer->statements.front().get();
        if ((consumer->kind == S::Variable || consumer->kind == S::Expression || consumer->kind == S::Return) &&
            stdlib_operation(consumer->expression.get(),"Compare")) {
            region.compare = consumer->expression.get();
            ++next;
            // An implicit conversion or return may run user code after Compare.
            can_move = can_move && (consumer->kind == S::Expression ||
                                    (consumer->kind == S::Variable && consumer->type == "int"));
        }
    }
    if (can_move) region.move = operation("Move");
    if (!region.move && !region.compare && !(region.fill && region.copy)) return 0;
    auto* previous = memory_region;
    memory_region = &region;
    for (std::size_t i = index; i < next; ++i) statement(*children[i]);
    memory_region = previous;
    return next - index;
}

// A successful Set leaves a uniform source. A Copy from that source can fill
// its destination independently. A subsequent Move only has to initialize the
// part outside the source and relocate a possible intervening byte write.
void Builder::transfer_filled(const Expr& e, const Class& c, const Method& m,
                              std::vector<Reg>& args, bool move) {
    auto& region=*memory_region;
    if (move && region.write && (!region.written_address || !region.written_byte)) {
        invoke(c,m,0,e.args,1,false,&args);
        return;
    }
    const auto& known=move && region.copy ? region.copied : region.filled;
    const auto dest=args[0], src=args[1], n=args[2], value=region.filled[1];
    const auto d=cast(dest,Type::I64,"ptrtoint"), s=cast(src,Type::I64,"ptrtoint");
    const auto zero=integer(Type::I64,0);
    auto both=[&](Reg a,Reg b){return instruction(Op::And,Type::I32,{a,b});};
    const auto above=cmp("uge",d,s);
    const auto forward=instruction(Op::Sub,Type::I64,{d,s}), backward=instruction(Op::Sub,Type::I64,{s,d});
    const auto distance=instruction(Op::Select,Type::I64,{above,forward,backward});
    auto valid=both(cmp("eq",src,known[0]),cmp("ule",n,known[2]));
    if (move && region.copy) valid=both(valid,region.uniform);
    valid=both(valid,both(cmp("ne",dest,null()),cmp("ule",d,instruction(Op::Sub,Type::I64,{zero,n}))));
    if (!move) {
        valid=both(valid,cmp("uge",distance,n));
        region.uniform=valid;
    }
    const auto fast=block(), original=block(), done=block();
    branch(valid,fast,original);
    current=original;
    invoke(c,m,0,e.args,1,false,&args);
    jump(done);
    current=fast;
    if (!move) {
        call("br_memory_set",Type::Void,{context,site(),dest,value,n});
    } else {
        const auto edge=instruction(Op::Select,Type::I64,{cmp("ult",distance,n),distance,n});
        const auto suffix=instruction(Op::Sub,Type::I64,{n,edge});
        const auto start=instruction(Op::Select,Type::I64,{above,suffix,zero});
        const auto boundary=instruction(Op::Offset,Type::Ptr,{dest,start});
        const auto single=block(), bulk=block(), patch=block();
        branch(cmp("eq",edge,integer(Type::I64,1)),single,bulk);
        current=single; store(boundary,value); jump(patch);
        current=bulk; call("br_memory_set",Type::Void,{context,site(),boundary,value,edge}); jump(patch);
        current=patch;
        if (region.write) {
            const auto w=cast(region.written_address,Type::I64,"ptrtoint");
            const auto relative=instruction(Op::Sub,Type::I64,{w,s});
            const auto relocate=block(), reset=block(), copy_byte=block();
            branch(cmp("ult",relative,n),relocate,done);
            current=relocate;
            branch(cmp("ult",instruction(Op::Sub,Type::I64,{w,d}),n),reset,copy_byte);
            current=reset; store(region.written_address,value); jump(copy_byte);
            current=copy_byte; store(instruction(Op::Offset,Type::Ptr,{dest,relative}),region.written_byte);
        }
    }
    jump(done);
    current=done;
}

void Builder::narrow_comparison(std::vector<Reg>& values) {
    // Both disjoint regions matched byte for byte after a successful Copy.
    // The compared prefix can differ only where an intervening byte store fell.
    // Compare's ordinary call still supplies its normal frame and diagnostics.
    if (memory_region->write && !memory_region->written_address) return;
    const auto& copy = memory_region->copied;
    const auto a = values[0], b = values[1], n = values[2];
    auto same_pair = [&](Reg x, Reg y) {
        return instruction(Op::And, Type::I32, {cmp("eq", a, x), cmp("eq", b, y)});
    };
    const auto pair = instruction(Op::Or, Type::I32, {same_pair(copy[0], copy[1]), same_pair(copy[1], copy[0])});
    const auto proven = instruction(Op::And, Type::I32, {pair, cmp("ule", n, copy[2])});
    const auto skip = alloc(8), bytes = alloc(8);
    store(skip, integer(Type::I64, 0));
    store(bytes, n);
    const auto inspect = block(), done = block();
    branch(proven, inspect, done);
    current = inspect;
    store(bytes, integer(Type::I64, 0));
    if (memory_region->written_address) {
        const auto w = cast(memory_region->written_address, Type::I64, "ptrtoint");
        const auto from_a = instruction(Op::Sub, Type::I64, {w, cast(a, Type::I64, "ptrtoint")});
        const auto from_b = instruction(Op::Sub, Type::I64, {w, cast(b, Type::I64, "ptrtoint")});
        const auto in_a = block(), try_b = block(), in_b = block();
        branch(cmp("ult", from_a, n), in_a, try_b);
        current = in_a;
        store(skip, from_a);
        store(bytes, integer(Type::I64, 1));
        jump(done);
        current = try_b;
        branch(cmp("ult", from_b, n), in_b, done);
        current = in_b;
        store(skip, from_b);
        store(bytes, integer(Type::I64, 1));
    }
    jump(done);
    current = done;
    const auto offset = load(Type::I64, skip);
    values = {instruction(Op::Offset, Type::Ptr, {a, offset}),
              instruction(Op::Offset, Type::Ptr, {b, offset}), load(Type::I64, bytes)};
}

std::vector<std::size_t> Builder::prepare_text_views(const Stmt& loop) {
    if(lower.core())return {}; // This representation decodes UTF-8 directly.
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget || !exit_actions.empty() || loop.kind != S::For ||
        !loop.init || loop.init->kind != S::Variable || loop.init->type != "int" || !loop.init->expression ||
        loop.init->expression->kind != K::Literal || literal_value(*loop.init->expression).as_int() != 0 ||
        !loop.condition || loop.condition->kind != K::Binary || loop.condition->binding != B::None ||
        loop.condition->text != "<" || loop.condition->args.size() != 2) return {};
    const auto& variable = *loop.condition->args[0];
    const auto& limit = *loop.condition->args[1];
    if (variable.kind != K::Name || variable.binding != B::Local || variable.local_id != loop.init->local_id ||
        limit.type != "int" || (limit.kind != K::Literal && (limit.kind != K::Name || limit.binding != B::Local ||
        is_reference(limit.declared_type) || limit.local_id == variable.local_id))) return {};
    if (limit.kind == K::Literal && literal_value(limit).as_int() < 32) return {};

    std::map<std::size_t, const Expr*> inputs;
    auto inspect_expression = [&](auto&& visit, const Expr& e) -> void {
        if (e.kind == K::Index && e.owner == "string" && !e.args.empty()) {
            const auto& input = *e.args[0];
            if (input.kind == K::Name && input.binding == B::Local && !is_reference(input.declared_type) &&
                !text_views.contains(input.local_id)) inputs.emplace(input.local_id, &input);
        }
        for (const auto& child : e.args) visit(visit, *child);
    };
    auto inspect_statement = [&](auto&& visit, const Stmt& s) -> void {
        for (auto* e : {s.expression.get(), s.condition.get(), s.increment.get()})
            if (e) inspect_expression(inspect_expression, *e);
        for (auto* child : {s.init.get(), s.body.get(), s.alternative.get()}) if (child) visit(visit, *child);
        for (const auto& child : s.statements) visit(visit, *child);
    };
    inspect_statement(inspect_statement, loop);
    if (inputs.empty() || !lower.read_only.reads_only(loop)) return {};

    // Resolve optional dense views once before a sizeable read-only loop.
    // A small trip count, exposed bytes, an arena or invalid UTF-8 retain the
    // normal checked indexing path. No pointer outlives this loop region.
    std::map<std::size_t, Reg> slots;
    std::vector<std::size_t> added;
    for (const auto& [id, input] : inputs) {
        (void)input;
        const auto slot = alloc(sizeof(BrTextView));
        instruction(Op::Zero, Type::Void, {slot}, sizeof(BrTextView));
        slots.emplace(id, slot);
        added.push_back(id);
    }
    const auto prepare = block(), ready = block();
    branch(cmp("sge", expression(limit).value, integer(Type::I32, 32)), prepare, ready);
    current = prepare;
    for (const auto& [id, input] : inputs) {
        const auto source = reference(*input);
        call("br_collection", Type::Void, {context, site(), type("string"), source,
            string("TryScalarView"), slots.at(id), null(), null()});
    }
    jump(ready); current = ready;
    for (const auto& [id, slot] : slots)
        text_views[id] = {load(Type::Ptr, offset(slot, offsetof(BrTextView, data))),
                          load(Type::I64, offset(slot, offsetof(BrTextView, count)))};
    return added;
}

} // namespace bolt::ir::lowering
