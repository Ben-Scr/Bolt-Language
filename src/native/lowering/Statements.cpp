#include "Lowering.hpp"
#include <algorithm>

namespace bolt::ir::lowering {

void Builder::drop_since(std::size_t first) {
    for (std::size_t n = cleanup.size(); n > first; --n) {
        const auto& slot = cleanup[n - 1];
        if (!slot.live) {
            clear_value(slot.type, slot.address, "br_drop");
            continue;
        }
        const auto release = block(), done = block();
        branch(load(Type::I8, slot.live), release, done);
        current = release;
        store(slot.live, integer(Type::I8, 0));
        clear_value(slot.type, slot.address, "br_drop");
        jump(done);
        current = done;
    }
}
void Builder::leave_actions(std::size_t first){
    const auto saved=exit_actions;const auto previous=failure;
    for(std::size_t i=saved.size();i>first&&!ended();--i){const auto action=saved[i-1];exit_actions.assign(saved.begin(),saved.begin()+static_cast<std::ptrdiff_t>(i-1));
        drop_since(action.cleanup_start);failure=action.outer;
        if(action.catch_end)call("br_catch_end",Type::Void,{context},false);
        if(action.finalizer)statement(*action.finalizer);
    }
    exit_actions=saved;failure=previous;
}
void Builder::try_statement(const Stmt& s){
    const auto outside=failure;const auto first=cleanup.size();const auto actions=exit_actions.size();
    auto dispatch=block(),propagate=block(),done=block();
    if(s.alternative)exit_actions.push_back({s.alternative.get(),false,first,outside});
    const auto try_actions=exit_actions;
    failure=dispatch;statement(*s.body);
    if(!ended()){drop_since(first);leave_actions(actions);if(!ended())jump(done);}
    current=dispatch;failure=outside;drop_since(first);
    for(const auto& handler:s.statements){
        auto body=block(),next=block(),catch_failure=block();
        auto match=call("br_exception_match",Type::I32,{context,handler->type.empty()?null():type(referred(handler->type))},false);branch(match,body,next);
        current=body;failure=propagate;auto pointer=call("br_catch_begin",Type::Ptr,{context,site()},false);check();
        if(!handler->type.empty())locals[handler->local_id]=pointer;
        const auto catch_first=cleanup.size();exit_actions=try_actions;exit_actions.push_back({nullptr,true,catch_first,outside});
        failure=catch_failure;statement(*handler->body);
        if(!ended()){leave_actions(actions);if(!ended())jump(done);}
        exit_actions=try_actions;current=catch_failure;failure=outside;drop_since(catch_first);call("br_catch_end",Type::Void,{context},false);jump(propagate);
        current=next;
    }
    jump(propagate);current=propagate;failure=outside;exit_actions.resize(actions);drop_since(first);
    if(s.alternative){auto saved=temp("#error-state");call("br_error_suspend",Type::Void,{context,saved},false);check();statement(*s.alternative);if(!ended())call("br_error_restore",Type::Void,{context,saved},false);}
    if(!ended())jump(outside);
    current=done;failure=outside;exit_actions.resize(actions);
}

static const Expr* indexed_loop_call(const Stmt& s) {
    if (!s.init || s.init->kind!=Stmt::Kind::Variable) return nullptr;
    const Stmt* work=s.body.get();
    if (work->kind==Stmt::Kind::Block && work->statements.size()==1) work=work->statements[0].get();
    const auto* call=work->expression.get();
    if (work->kind!=Stmt::Kind::Expression || !call || call->kind!=K::Call || call->args.empty()) return nullptr;
    const auto& member=*call->args[0];
    if (member.kind!=K::Member || member.args.size()!=1) return nullptr;
    const auto& receiver=*member.args[0];
    if (receiver.kind!=K::Index || !is_array(receiver.owner) || receiver.args.size()!=2 ||
        receiver.args[1]->binding!=B::Local || receiver.args[1]->local_id!=s.init->local_id) return nullptr;
    return call;
}

// Simple reads and the scalar List edit itself cannot open or close a native
// range. A range already active on entry still takes the checked edit path.
static bool stable_edit_ranges(Lower& lower, const Stmt& s, const Expr& call) {
    const auto& member=*call.args[0];
    if (call.binding!=B::Method || call.is_extension || member.arrow ||
        generic_base(call.owner)!="System.Collections.Generic.List" ||
        (member.text!="RemoveAt" && member.text!="Insert") ||
        !s.condition || !pure_scalar_value(*s.condition) || !s.increment) return false;
    const auto& step=*s.increment;
    if ((step.kind!=K::Postfix && step.kind!=K::Unary) || step.binding!=B::None ||
        step.text!="++" || step.args.size()!=1 || step.args[0]->binding!=B::Local ||
        step.args[0]->local_id!=s.init->local_id || s.init->type!="int") return false;
    auto local_array=[](const Expr& e) {
        return e.kind==K::Name && e.binding==B::Local && is_array(e.type) && !is_reference(e.declared_type);
    };
    const auto& receiver=*call.args[0]->args[0];
    if (receiver.binding!=B::None || !local_array(*receiver.args[0])) return false;
    const auto* cls=lower.program.find_class(call.owner);
    if (!cls || cls->token.at.file!="<bolt-stdlib>/System/Collections/Generic/List.bolt" ||
        !native_abi::scalar_storage_kind(lower.layout(generic_args(call.owner).at(0)).kind)) return false;
    return std::all_of(call.args.begin()+1,call.args.end(),[&](const auto& arg) {
        return pure_scalar_value(*arg) || (arg->kind==K::Index && arg->binding==B::None &&
            is_numeric(arg->type) && arg->args.size()==2 && local_array(*arg->args[0]) &&
            pure_scalar_value(*arg->args[1]));
    });
}

// A small integer recurrence exposes little instruction-level parallelism.
// Amortize its loop branch without duplicating calls or nested control flow.
static bool bitwise_recurrence(const Stmt& body) {
    using S = Stmt::Kind;
    if (body.kind != S::Block || body.statements.empty() || body.statements.size() > 8) return false;
    bool shift = false, mix = false;
    auto operand = [&](auto&& visit, const Expr& e) -> bool {
        if (!is_integer(e.type)) return false;
        if (e.kind == K::Name) return e.binding == B::Local && !is_reference(e.declared_type);
        if (e.kind == K::Literal) return true;
        if (e.kind == K::Call && e.binding == B::Cast && e.args.size() == 2)
            return visit(visit,*e.args[1]);
        if (e.kind != K::Binary || e.binding != B::None || e.args.size() != 2) return false;
        if (e.text == "<<" || e.text == ">>") shift = true;
        else if (e.text == "^" || e.text == "|" || e.text == "&") mix = true;
        else return false;
        return visit(visit,*e.args[0]) && visit(visit,*e.args[1]);
    };
    for (const auto& child : body.statements) {
        if (!child->expression) return false;
        const auto& e = *child->expression;
        if (child->kind == S::Variable) {
            if (!is_integer(child->type) || !operand(operand,e)) return false;
        } else if (child->kind != S::Expression || e.kind != K::Assign || e.binding != B::None ||
                   e.text != "=" || e.args.size() != 2 || e.args[0]->binding != B::Local ||
                   !operand(operand,*e.args[0]) || !operand(operand,*e.args[1])) return false;
    }
    return shift && mix;
}

void Builder::ordinary_for(const Stmt& s, std::int32_t proven_stride, bool keep_rolled, bool budget_reserved) {
    const auto first = cleanup.size();
    if (s.init) statement(*s.init);
    const auto body_first = cleanup.size();
    const auto* indexed=lower.options.optimize_loops && !keep_rolled ? indexed_loop_call(s) : nullptr;
    const auto enclosing_ranges=loop_edit_ranges;
    if (indexed && !lower.module.instrument_budget && stable_edit_ranges(lower,s,*indexed))
        loop_edit_ranges={indexed,load(Type::I64,offset(context,offsetof(BrContext,active_ranges)),"execution context")};
    const auto cond = block(), body = block(), inc = block(), done = block();
    jump(cond); current = cond;
    if (!budget_reserved) tick();
    if (s.condition) { const auto v = expression(*s.condition); branch(v.value,body,done); }
    else jump(body);
    current = body;
    loops.push_back({done,inc,body_first,exit_actions.size()});
    statement(*s.body); loops.pop_back();
    loop_edit_ranges=enclosing_ranges;
    if (!ended()) jump(inc);
    current = inc;
    if (proven_stride) {
        const auto slot = locals.at(s.init->local_id);
        const auto next = instruction(Op::Add,Type::I32,
            {load(Type::I32,slot),integer(Type::I32,proven_stride)},0,"nsw");
        store(slot,next);
    } else if (s.increment) (void)expression(*s.increment);
    drop_since(body_first);
    bool unroll_body = unroll_tallies.contains(&s);
    // Successive array elements have separate list headers. Unrolling a short
    // edit loop lets the processor overlap their loads and bounds checks.
    if (indexed && !unroll_body && fn.blocks.size()-cond<=16)
        unroll_body=std::any_of(fn.blocks.begin()+body,fn.blocks.end(),[](const Block& block) {
            return std::any_of(block.code.begin(),block.code.end(),[](const Instruction& i){return i.storage_move_stride!=0;});
        });
    unsigned unroll = unroll_body && fn.blocks.size()-cond<=16 ? 8 : 0;
    if (lower.options.optimize_loops && !lower.module.instrument_budget && !keep_rolled &&
        fn.blocks.size()-cond <= 24 && bitwise_recurrence(*s.body)) unroll = 16;
    jump(cond,keep_rolled ? 1 : unroll);
    current = done; drop_since(first);
}

void Builder::statement(const Stmt& s){
    if(ended())return;
    at=s.token.at;using S=Stmt::Kind;
    const auto added_views = s.kind == S::For ? prepare_text_views(s) : std::vector<std::size_t>{};
    const auto array_views = s.kind == S::For || s.kind == S::While ? loop_array_views(s) : std::vector<std::size_t>{};
    switch(s.kind){
    case S::Declarations:for(const auto& child:s.statements)statement(*child);break;
    case S::Block: {
        prove_finite_loops(s);
        find_consumed_snapshots(s);
        find_list_projections(s);
        find_private_record_buffers(s);
        find_private_arrays(s);
        prove_counted_additions(s);
        const auto first = cleanup.size();
        for (std::size_t i = 0; i < s.statements.size() && !ended();) {
            if (i && peel_scalar_scan(*s.statements[i-1],*s.statements[i])) { ++i; continue; }
            if (!private_record_clears.empty() && elide_record_refill(s,i)) { i += 3; continue; }
            if (indexed_snapshot(s, i)) { i += 3; continue; }
            if (const auto n = lower_utf8_edits(s, i)) { i += n; continue; }
            if (const auto edits = paired_list_edit(s,i)) { i += edits; continue; }
            const auto consumed = lower_memory_transfers(s, i);
            if (consumed) i += consumed;
            else {
                if (s.statements[i]->kind == S::For && i+1 < s.statements.size() && s.statements[i+1]->kind == S::For)
                    following_for.emplace(s.statements[i].get(),s.statements[i+1].get());
                statement(*s.statements[i++]);
            }
        }
        if (!ended()) drop_since(first);
        break;
    }
    case S::Variable: {
        if (is_reference(s.type)) {
            auto address=s.expression->lvalue ? reference(*s.expression) : memory(expression(*s.expression));
            locals[s.local_id]=borrow(s.expression->type,referred(s.type),address);
            break;
        }
        auto dest = temp(s.type);
        locals[s.local_id] = dest;
        if (auto it = projected_lists.find(s.local_id); it != projected_lists.end())
            store(it->second.count,integer(Type::I32,0));
        if (s.type == "int") stack_ints.insert(s.local_id);
        if (!s.expression) {
            initialize(s.type, dest);
            break;
        }
        const auto& init = *s.expression;
        const auto array = private_arrays.find(s.local_id);
        auto value = array != private_arrays.end() && !array->second.nullable
            ? new_vector(init,&array->second.length) : expression(init);
        at = s.token.at;
        // Move a freshly allocated value into this zero-initialized local.
        // Existing values and user-defined elements retain copy semantics.
        if (s.type == value.type && fresh_handle(init)) {
            store(dest, load(Type::Ptr, value.value));
            store(value.value, null());
        } else {
            assign(s.type, dest, value);
        }
        if (array != private_arrays.end()) {
            const auto header = load(Type::Ptr, dest);
            if (array->second.nullable) load_array_view(array->second,header);
            else array->second.data = load_sequence_data(header);
        }
        break;
    }
    case S::Expression: {
        const auto first = cleanup.size();
        if (s.expression) {
            const auto& e = *s.expression;
            if (lower.options.optimize_loops && e.kind == K::Assign && e.binding == B::None &&
                e.text == "=" && e.args.size() == 2 && e.args[0]->kind == K::Name &&
                e.args[0]->binding == B::Local && !is_reference(e.args[0]->declared_type) &&
                e.args[0]->type == e.args[1]->type && fresh_handle(*e.args[1])) {
                const auto dest = reference(*e.args[0]);
                const auto& value = *e.args[1];
                if (value.kind == K::Call && value.binding == B::Builtin && value.owner == "String" &&
                    value.text == "FromUtf8" && value.args.size() == 2) {
                    // This bridge commits only after validating its complete byte
                    // input. The owning local can receive the result directly.
                    auto bytes = expression(*value.args[1]);
                    at = value.token.at;
                    string_call(value.text,value.type,{bytes},dest);
                } else {
                    const auto source = expression(value);
                    at = e.token.at;
                    take_handle(dest,source);
                }
            } else (void)expression(e);
            if (auto frozen = frozen_tables.find(&s); frozen != frozen_tables.end()) {
                auto& fact = private_arrays.at(frozen->second.local);
                const auto scratch = alloc(frozen->second.bytes,false);
                // Matching the original scalar stores lets LLVM forward each
                // initialized value into the private table, including offsets.
                const auto word = lower.layout(e.args[0]->type).ir;
                for (std::size_t n=0; n<frozen->second.bytes; n+=width(word))
                    store(offset(scratch,n),load(word,offset(fact.data,n)));
                fact.data = scratch;
                if (e.args[0]->type == "int") {
                    Reg lo = load(Type::I32,scratch), hi = lo;
                    for (std::size_t n=4; n<frozen->second.bytes; n+=4) {
                        const auto v=load(Type::I32,offset(scratch,n));
                        lo=instruction(Op::Select,Type::I32,{cmp("slt",v,lo),v,lo});
                        hi=instruction(Op::Select,Type::I32,{cmp("sgt",v,hi),v,hi});
                    }
                    fact.frozen_min=cast(lo,Type::I64,"sext");
                    fact.frozen_max=cast(hi,Type::I64,"sext");
                }
            }
        }
        drop_since(first);
        break;
    }
    case S::If:{
        if(lower_pointer_swap(s)||select_array_update(s)||select_array_cursor(s)||conditional_increment(s))break;
        const auto cond=expression(*s.condition).value;
        const auto yes=block(),no=block(),done=block();
        branch(cond,yes,no);
        if (updates_tested_element(s)) fn.blocks[current].code.back().text="unlikely";
        current=yes;auto yes_first=cleanup.size();statement(*s.body);if(!ended()){drop_since(yes_first);jump(done);}
        current=no;auto no_first=cleanup.size();if(s.alternative)statement(*s.alternative);if(!ended()){drop_since(no_first);jump(done);}
        current=done;break;}
    case S::While:{auto first=cleanup.size();auto cond=block(),body=block(),done=block();jump(cond);current=cond;tick();auto v=expression(*s.condition);branch(v.value,body,done);current=body;loops.push_back({done,cond,first,exit_actions.size()});statement(*s.body);loops.pop_back();if(!ended()){drop_since(first);jump(cond);}current=done;drop_since(first);break;}
    case S::For: if (!optimize_counted_loop(s)) ordinary_for(s); break;
    case S::SwitchScope: {
        auto done=block();
        loops.push_back({done,0,cleanup.size(),exit_actions.size(),true});
        statement(*s.body);loops.pop_back();
        if(!ended())jump(done);
        current=done;break;
    }

    case S::ForEach:{
        const auto& container = s.expression->type;
        const auto element_type = referred(s.type);
        const bool contiguous = (is_array(container) && array_rank(container)==1) || collection_kind(container)=="List";
        const bool direct = lower.options.optimize_loops && !lower.module.instrument_budget && contiguous &&
            (is_numeric(element_type) || element_type=="char" || element_type=="bool") &&
            lower.read_only.reads_only(*s.body);
        const auto first = cleanup.size();
        const auto source = expression(*s.expression);
        const auto cond = block(), body = block(), done = block();
        Reg element;
        if (direct) {
            // A body that only reads managed values cannot replace the iterated
            // handle or change its epoch. A null sequence has no elements.
            const auto header = load(Type::Ptr,source.value);
            const auto present = block();
            branch(cmp("ne",header,null()),present,done);
            current = present;
            const auto count = load(Type::I64,offset(header,offsetof(BrSequenceHeader,count)));
            const auto data = load_sequence_data(header);
            const auto cursor = alloc(8,false);
            store(cursor,integer(Type::I64,0));
            jump(cond);
            current = cond;
            const auto index = load(Type::I64,cursor);
            branch(cmp("ult",index,count),body,done);
            current = body;
            const auto stride = integer(Type::I64,lower.layout(element_type).size);
            const auto bytes = instruction(Op::Mul,Type::I64,{index,stride});
            element = instruction(Op::Offset,Type::Ptr,{data,bytes});
            store(cursor,instruction(Op::Add,Type::I64,{index,integer(Type::I64,1)}));
        } else {
            const auto range=temp("#native-range");
            call("br_range",Type::Void,{context,site(),type(source.type),source.value,type(element_type),range});
            jump(cond);current=cond;tick();
            element=call("br_range_next",Type::Ptr,{context,site(),range});
            branch(cmp("ne",element,null()),body,done);current=body;
        }
        const auto each_first=cleanup.size();
        if(is_reference(s.type))locals[s.local_id]=element;
        else {auto value=temp(s.type);assign(s.type,value,from_memory(s.type,element));locals[s.local_id]=value;}
        loops.push_back({done,cond,each_first,exit_actions.size()});statement(*s.body);loops.pop_back();if(!ended()){drop_since(each_first);jump(cond);}
        current=done;drop_since(first);break;
    }
    case S::Try:try_statement(s);break;
    case S::Catch:throw Error(s.token.at,"internal catch outside a try","BLT4001");
    case S::Throw:{if(s.expression){auto value=expression(*s.expression);at=s.token.at;call("br_raise",Type::Void,{context,site(),type(value.type),memory(value)},false);}else call("br_rethrow",Type::Void,{context,site()},false);jump(failure);break;}
    case S::Return: {
        if (s.expression) {
            if (return_type == "void") expression(*s.expression);
            else if (is_reference(return_type))
                store(return_slot,borrow(s.expression->type,referred(return_type),reference(*s.expression)));
            else {
                auto value = expression(*s.expression);
                at = s.token.at;
                const auto dest = structure_return ? structure_return : return_slot;
                if (lower.options.optimize_loops && return_type == value.type && fresh_handle(*s.expression))
                    take_handle(dest,value);
                else assign(return_type,dest,value);
            }
        }
        leave_actions(0);
        if (!ended()) jump(exit);
        break;
    }
    case S::Break:leave_actions(loops.back().action_depth);if(!ended()){drop_since(loops.back().cleanup_start);jump(loops.back().done);}break;
    case S::Continue:{
        auto loop=*std::find_if(loops.rbegin(),loops.rend(),[](const Loop& item){return !item.switch_scope;});
        leave_actions(loop.action_depth);if(!ended()){drop_since(loop.cleanup_start);jump(loop.again);}break;
    }
    case S::Empty:break;
    }
    for (const auto id : added_views) text_views.erase(id);
    for (const auto id : array_views) private_arrays.erase(id);
}
void Builder::initialize_class(const Class& c,const Method* constructor){
    if(!c.base.empty()){
        const auto* base=lower.program.find_class(c.base);
        if(constructor&&constructor->explicit_base&&!constructor->base_constructor.empty())invoke(*base,*method_by_id(*base,constructor->base_constructor),self,constructor->base_arguments,0,false);
        else if(constructor&&!constructor->base_constructor.empty())invoke(*base,*method_by_id(*base,constructor->base_constructor),self,constructor->base_arguments,0,false);
        else call(lower.init_name(c.base),Type::Void,{context,self},true,false);
    }
    if(!c.is_struct)store(self,type(c.name));
    for(const auto& field:c.fields)if(!field.is_static){
        at=field.token.at;auto p=lower.field(*this,c.name,field.name,self);
        const Expr* member=nullptr;if(constructor)for(const auto& [name,expr]:constructor->field_initializers)if(name==field.name){member=expr.get();break;}
        const auto* init=member?member:field.initializer.get();
        if(init)assign(field.type,p,expression(*init));else initialize(field.type,p);
    }
}

} // namespace bolt::ir::lowering
