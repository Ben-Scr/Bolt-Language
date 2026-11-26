// After a successful store, the inequality is false for that element until
// another write or a change of key. Favor the skip path on repeated visits.
bool Builder::updates_tested_element(const Stmt& s) const {
    using S = Stmt::Kind;
    const auto& test = *s.condition;
    if (affine_data.empty() || s.alternative || test.kind != K::Binary || test.binding != B::None ||
        test.text != "&&" || test.args.size()!=2) return false;
    const auto* first = s.body.get();
    if (first->kind==S::Block && !first->statements.empty()) first=first->statements[0].get();
    const auto* write = first->expression.get();
    if (first->kind!=S::Expression || !write || write->kind!=K::Assign || write->binding!=B::None ||
        write->text!="=" || write->args.size()!=2) return false;
    const auto& right = *test.args[1];
    if (right.kind!=K::Binary || right.binding!=B::None || right.text!="!=" || right.args.size()!=2) return false;
    const auto& key = *right.args[0];
    const auto& dest = *write->args[0];
    const auto& value = *right.args[1];
    const auto& stored = *write->args[1];
    const auto same_local=[](const Expr& a,const Expr& b) {
        return a.kind==K::Name && b.kind==K::Name && a.binding==B::Local && b.binding==B::Local &&
               a.local_id==b.local_id;
    };
    return key.kind==K::Index && dest.kind==K::Index && affine_data.contains(&key) && affine_data.contains(&dest) &&
           key.args.size()==2 && dest.args.size()==2 && same_local(*dest.args[0],*key.args[0]) &&
           same_local(*dest.args[1],*key.args[1]) && value.type==stored.type && is_integer(value.type) &&
           (same_local(value,stored) || (value.kind==K::Literal && stored.kind==K::Literal && stored.text==value.text));
}

// Add the predicate as a zero-or-one delta to a private counter. Writing the
// original value on a false predicate is valid only when neither the array
// nor its elements have been exposed through references or raw pointers.
bool Builder::conditional_increment(const Stmt& s) {
    using S = Stmt::Kind;
    if (s.alternative || loops.empty() || !lower.options.optimize_loops || lower.module.instrument_budget)
        return false;
    const auto& body = s.body->kind == S::Block && s.body->statements.size() == 1 ? *s.body->statements[0] : *s.body;
    const auto* e = body.expression.get();
    if (body.kind != S::Expression || !e || e->kind != K::Postfix || e->binding != B::None ||
        e->text != "++" || e->type != "int" || e->args.size() != 1) return false;
    const auto& item = *e->args[0];
    const auto* view = private_array(item);
    if (!view || !view->data || !view->confined_elements) return false;
    const auto& index = *item.args[1];
    const auto* base = &index;
    std::int64_t delta = 0;
    if (index.kind == K::Binary && index.binding == B::None && index.type == "int" &&
        (index.text == "+" || index.text == "-") && index.args.size() == 2 &&
        index.args[1]->kind == K::Literal && index.args[1]->type == "int") {
        base = index.args[0].get();
        delta = literal_value(*index.args[1]).as_int();
        if (index.text == "-") delta = -delta;
    }
    if (base->kind != K::Name || base->binding != B::Local || !stack_ints.contains(base->local_id)) return false;
    const auto data = view->data, length = view->length;
    const auto choice = expression(*s.condition).value;
    const auto key = instruction(Op::Add,Type::I64,{cast(expression(*base).value,Type::I64,"sext"),integer(Type::I64,delta)});
    const auto maximum = integer(Type::I64,static_cast<std::uint64_t>(INT32_MAX)+1);
    const auto limit = instruction(Op::Select,Type::I64,{cmp("ult",length,maximum),length,maximum});
    const auto invalid = cmp("uge",key,limit);
    const auto active = cmp("ne",choice,integer(fn.registers[choice],0));
    const auto fast = block(), slow = block(), replay = block(), done = block();
    branch(invalid,slow,fast);
    current = slow;
    branch(choice,replay,done);
    current = fast;
    const auto address = instruction(Op::Offset,Type::Ptr,{data,instruction(Op::Mul,Type::I64,{key,integer(Type::I64,4)})});
    Instruction add;
    add.op = Op::CheckedAdd;
    add.text = "s";
    add.args = {load(Type::I32,address),active};
    add.result = reg(Type::I32);
    add.overflow = reg(Type::I32);
    emit(add);
    const auto commit = block();
    branch(add.overflow,replay,commit);
    current = commit;
    store(address,add.result);
    jump(done);
    current = replay;
    statement(body);
    jump(done);
    current = done;
    return true;
}

// Both arms copy a scalar element into the same destination and advance
// their own input cursor. Select the cursor before the common copy. Fall
// back to the selected original arm on any possible exception so its
// evaluation order, cursor side effects and source locations are preserved.
bool Builder::select_array_cursor(const Stmt& s) {
    using S = Stmt::Kind;
    if (!s.alternative || loops.empty() || !lower.options.optimize_loops || lower.module.instrument_budget)
        return false;
    auto assignment = [&](const Stmt& branch) -> const Expr* {
        const auto& st = branch.kind == S::Block && branch.statements.size() == 1 ? *branch.statements[0] : branch;
        const auto* e = st.expression.get();
        if (st.kind != S::Expression || !e || e->kind != K::Assign || e->binding != B::None ||
            e->text != "=" || e->args.size() != 2 || !is_numeric(e->type)) return nullptr;
        const auto& dest = *e->args[0];
        const auto& src = *e->args[1];
        const auto* out = private_array(dest);
        const auto* in = private_array(src);
        if (!out || !in || !out->data || !in->data || dest.type != src.type ||
            dest.args[1]->kind != K::Name || dest.args[1]->binding != B::Local ||
            !stack_ints.contains(dest.args[1]->local_id)) return nullptr;
        const auto& index = *src.args[1];
        if (index.kind != K::Postfix || index.text != "++" || index.binding != B::None || index.args.size() != 1 ||
            index.args[0]->kind != K::Name || index.args[0]->binding != B::Local ||
            !stack_ints.contains(index.args[0]->local_id)) return nullptr;
        return e;
    };
    const auto* yes = assignment(*s.body);
    const auto* no = assignment(*s.alternative);
    if (!yes || !no) return false;
    const auto& dest = *yes->args[0];
    const auto& src = *yes->args[1];
    if (dest.args[0]->local_id != no->args[0]->args[0]->local_id ||
        dest.args[1]->local_id != no->args[0]->args[1]->local_id ||
        src.args[0]->local_id != no->args[1]->args[0]->local_id) return false;
    const auto a = locals.at(src.args[1]->args[0]->local_id);
    const auto b = locals.at(no->args[1]->args[1]->args[0]->local_id);
    if (a == b) return false;
    const auto* terminal = s.condition.get();
    auto logical = [](const Expr& e) {
        return e.kind == K::Binary && e.binding == B::None && (e.text == "||" || e.text == "&&");
    };
    while (logical(*terminal)) terminal = terminal->args[1].get();
    // Only preload elements already read by this comparison. In particular,
    // copying one selected element should not introduce a read racing with
    // another worker's write to the unselected element.
    bool reads_left = false, reads_right = false;
    auto reads = [&](auto&& self, const Expr& e) -> bool {
        if (e.kind == K::Name) return e.binding == B::Local &&
            (is_numeric(e.type) || e.type == "bool" || (is_array(e.type) && is_numeric(element_type(e.type))));
        if (!is_numeric(e.type) && e.type != "bool") return false;
        if (e.kind == K::Literal) return true;
        if (e.binding != B::None || (e.kind != K::Index && e.kind != K::Binary) || logical(e) ||
            e.text == "??" || e.args.size() != 2) return false;
        if (e.kind == K::Index && e.args[0]->binding == B::Local && e.args[0]->local_id == src.args[0]->local_id &&
            e.args[1]->kind == K::Name && e.args[1]->binding == B::Local) {
            reads_left |= e.args[1]->local_id == src.args[1]->args[0]->local_id;
            reads_right |= e.args[1]->local_id == no->args[1]->args[1]->args[0]->local_id;
        }
        return self(self,*e.args[0]) && self(self,*e.args[1]);
    };
    if (!reads(reads,*terminal) || !reads_left || !reads_right) return false;
    const auto chosen = block(), other = block(), done = block();
    const auto* predicate = s.condition.get();
    while (logical(*predicate)) {
        const auto test = expression(*predicate->args[0]).value;
        const auto next = block();
        if (predicate->text == "||") branch(test,chosen,next);
        else branch(test,next,other);
        current = next;
        predicate = predicate->args[1].get();
    }
    const auto choice = expression(*predicate).value;
    const auto left = load(Type::I32,a), right = load(Type::I32,b);
    const auto extension = affine_data.contains(&src) ? "zext" : "sext";
    const auto i = cast(left,Type::I64,extension), k = cast(right,Type::I64,extension);
    const auto j = cast(expression(*dest.args[1]).value,Type::I64,"sext");
    const auto input = *private_array(src), output = *private_array(dest);
    if (!affine_data.contains(&dest) || !affine_data.contains(&src) ||
        !affine_data.contains(no->args[1].get())) {
        // Both input bounds were checked by the terminal comparison. Only
        // the increments and destination can still fail here.
        const auto maximum = integer(Type::I32,INT32_MAX);
        const auto valid = instruction(Op::And,Type::I32,{cmp("ne",left,maximum),cmp("ne",right,maximum)});
        const auto fast = block(), slow = block();
        branch(instruction(Op::And,Type::I32,{valid,cmp("ult",j,output.length)}),fast,slow);
        current = slow; branch(choice,chosen,other);
        current = fast;
    }
    const auto layout = lower.layout(src.type);
    const auto size = integer(Type::I64,layout.size);
    const auto from = [&](Reg index) {
        return load(layout.ir,instruction(Op::Offset,Type::Ptr,{input.data,instruction(Op::Mul,Type::I64,{index,size})}));
    };
    const auto to = instruction(Op::Offset,Type::Ptr,{output.data,instruction(Op::Mul,Type::I64,{j,size})});
    const auto one = integer(Type::I32,1);
    store(a,instruction(Op::Select,Type::I32,{choice,instruction(Op::Add,Type::I32,{left,one},0,"nsw"),left}));
    store(b,instruction(Op::Select,Type::I32,{choice,right,instruction(Op::Add,Type::I32,{right,one},0,"nsw")}));
    store(to,instruction(Op::Select,layout.ir,{choice,from(i),from(k)}));
    jump(done);
    current = chosen; statement(*s.body); jump(done);
    current = other; statement(*s.alternative); jump(done);
    current = done;
    return true;
}

// In a counted loop, a conditional update of scalar arrays can often select
// the target address and signed delta before doing a single checked store.
// Nested comparisons may only read array elements required on every path.
bool Builder::select_array_update(const Stmt& root) {
    using S = Stmt::Kind;
    if (affine_data.empty() || !root.alternative) return false;
    auto unbrace = [](const Stmt& st) -> const Stmt& {
        return st.kind == S::Block && st.statements.size() == 1 ? *st.statements[0] : st;
    };
    std::optional<std::size_t> index_local;
    std::map<std::size_t,Reg> addresses;
    std::set<std::size_t> speculative_reads;
    auto eligible_index = [&](const Expr& e) {
        auto* array = private_array(e);
        if (!array || array->nullable || !array->data || e.binding != B::None || e.type != "int" ||
            e.args[0]->type != "int[]") return false;
        const auto& i = *e.args[1];
        if (i.kind != K::Name || i.binding != B::Local || !stack_ints.contains(i.local_id) ||
            (index_local && *index_local != i.local_id)) return false;
        index_local = i.local_id;
        addresses.try_emplace(e.args[0]->local_id,0);
        return true;
    };
    auto scalar = [&](const Expr& e) {
        return e.type == "int" && (e.kind == K::Literal ||
            (e.kind == K::Name && e.binding == B::Local && stack_ints.contains(e.local_id)));
    };
    auto inspect = [&](auto&& visit, const Stmt& statement, int depth) -> bool {
        const auto& st = unbrace(statement);
        if (st.kind == S::If) {
            if (depth > 1 || !st.alternative) return false;
            if (depth) {
                const auto& c = *st.condition;
                if (c.kind != K::Binary || c.binding != B::None || c.args.size() != 2 ||
                    (c.text != "==" && c.text != "!=" && c.text != "<" && c.text != "<=" &&
                     c.text != ">" && c.text != ">=")) return false;
                for (const auto& operand : c.args) {
                    if (scalar(*operand)) continue;
                    if (!eligible_index(*operand)) return false;
                    speculative_reads.insert(operand->args[0]->local_id);
                }
            }
            return visit(visit,*st.body,depth+1) && visit(visit,*st.alternative,depth+1);
        }
        if (st.kind != S::Expression || !st.expression) return false;
        const auto& e = *st.expression;
        if (e.binding != B::None || e.args.empty() || !eligible_index(*e.args[0])) return false;
        if (e.kind == K::Assign)
            return e.args.size() == 2 && (e.text == "+=" || e.text == "-=") && scalar(*e.args[1]);
        return (e.kind == K::Unary || e.kind == K::Postfix) && (e.text == "++" || e.text == "--");
    };
    if (!inspect(inspect,root,0)) return false;
    auto always_reads = [&](auto&& visit,const Stmt& statement,std::size_t id) -> bool {
        const auto& st = unbrace(statement);
        if (st.kind != S::If) return st.expression->args[0]->args[0]->local_id == id;
        for (const auto& operand : st.condition->args)
            if (operand->kind == K::Index && operand->args[0]->local_id == id) return true;
        return visit(visit,*st.body,id) && visit(visit,*st.alternative,id);
    };
    for (auto id : speculative_reads)
        if (!always_reads(always_reads,*root.body,id) || !always_reads(always_reads,*root.alternative,id)) return false;

    const auto outer = expression(*root.condition).value;
    const auto index = cast(load(Type::I32,locals.at(*index_local)),Type::I64,"sext");
    const auto bytes = instruction(Op::Mul,Type::I64,{index,integer(Type::I64,4)});
    const auto slow = block(), done = block();
    auto require = [&](Reg condition) {
        auto next = block();
        branch(condition,next,slow);
        current = next;
    };
    for (auto& [id,address] : addresses) {
        const auto& array = private_arrays.at(id);
        require(cmp("ult",index,array.length));
        address = instruction(Op::Offset,Type::Ptr,{array.data,bytes});
    }
    struct Update { Reg address, delta; };
    auto select_update = [&](auto&& visit,const Stmt& statement) -> Update {
        const auto& st = unbrace(statement);
        if (st.kind == S::If) {
            auto condition = outer;
            if (&st != &root) {
                const auto& c = *st.condition;
                auto operand = [&](const Expr& e) {
                    return e.kind == K::Index ? load(Type::I32,addresses.at(e.args[0]->local_id)) : expression(e).value;
                };
                const auto lhs = operand(*c.args[0]), rhs = operand(*c.args[1]);
                const auto pred = c.text == "==" ? "eq" : c.text == "!=" ? "ne" : c.text == "<" ? "slt" :
                                  c.text == "<=" ? "sle" : c.text == ">" ? "sgt" : "sge";
                condition = cmp(pred,lhs,rhs);
            }
            const auto yes = visit(visit,*st.body), no = visit(visit,*st.alternative);
            return {instruction(Op::Select,Type::Ptr,{condition,yes.address,no.address}),
                    instruction(Op::Select,Type::I32,{condition,yes.delta,no.delta})};
        }
        const auto& e = *st.expression;
        auto delta = e.kind == K::Assign ? expression(*e.args[1]).value : integer(Type::I32,1);
        if (e.text == "-=" || e.text == "--") {
            if (e.kind == K::Assign) require(cmp("ne",delta,integer(Type::I32,0x80000000u)));
            delta = instruction(Op::Sub,Type::I32,{integer(Type::I32,0),delta});
        }
        return {addresses.at(e.args[0]->args[0]->local_id),delta};
    };
    const auto update = select_update(select_update,root);
    Instruction add;
    add.op = Op::CheckedAdd; add.text = "s";
    add.args = {load(Type::I32,update.address),update.delta};
    add.result = reg(Type::I32); add.overflow = reg(Type::I32);
    emit(add);
    require(cmp("eq",add.overflow,integer(Type::I32,0)));
    store(update.address,add.result);
    jump(done);
    // Replay only the selected source arm for precise bounds and overflow
    // exceptions. Its original condition has already been evaluated once.
    current = slow;
    const auto yes = block(), no = block();
    branch(outer,yes,no);
    current = yes; statement(*root.body); jump(done);
    current = no; statement(*root.alternative); jump(done);
    current = done;
    return true;
}
