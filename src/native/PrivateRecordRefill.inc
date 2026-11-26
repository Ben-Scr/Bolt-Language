// A private Buffer can keep its real Reserve (and any allocation failure) while
// forwarding a refill directly into its sole reduction. The use proof covers
// the entire remaining lifetime of the local, including enclosing loop backedges.
namespace {
using RecordStmt = Stmt::Kind;

bool record_buffer_call(const Expr& call, std::size_t id, std::string_view name, std::size_t arity) {
    if (call.kind != K::Call || call.binding != B::Collection || call.text != name ||
        call.args.size() != arity+1 || generic_base(call.owner) != "Bolt.Runtime.Buffer") return false;
    const auto& member = *call.args[0];
    return member.kind == K::Member && !member.arrow && member.args.size() == 1 && named_local(*member.args[0],id);
}

struct RecordBufferUses {
    std::size_t id;
    const Stmt* clear = nullptr;

    bool children(const Stmt& s, std::size_t begin) {
        for (auto i=begin; i<s.statements.size(); ++i) {
            const auto& child = *s.statements[i];
            if (s.kind == RecordStmt::Block && child.kind == RecordStmt::Expression && child.expression &&
                record_buffer_call(*child.expression,id,"Clear",0) && i+2 < s.statements.size() &&
                s.statements[i+1]->kind == RecordStmt::For && s.statements[i+2]->kind == RecordStmt::For) {
                if (clear) return false;
                clear = &child;
                i += 2; // Both loop bodies must subsequently pass the complete refill/reduction matcher.
            } else if (!statement(child)) return false;
        }
        return true;
    }

    bool statement(const Stmt& s) {
        for (const auto* e : {s.expression.get(),s.condition.get(),s.increment.get()})
            if (e && !independent_of(*e,id)) return false;
        for (const auto* child : {s.init.get(),s.body.get(),s.alternative.get()})
            if (child && !statement(*child)) return false;
        return children(s,0);
    }
};

// Only a complete zero-based traversal with the ordinary built-in increment
// can use the same induction value for constructing and consuming each record.
const Expr* record_loop_bound(const Stmt& s) {
    if (!s.init || s.init->kind != RecordStmt::Variable || s.init->type != "int" || !s.init->expression ||
        s.init->expression->kind != K::Literal || s.init->expression->type != "int" ||
        literal_value(*s.init->expression).as_int() != 0 || !s.condition || !s.increment || !s.body) return nullptr;
    const auto& test = *s.condition;
    const auto& step = *s.increment;
    if (test.kind != K::Binary || test.binding != B::None || test.text != "<" || test.args.size() != 2 ||
        !named_local(*test.args[0],s.init->local_id) || test.args[1]->type != "int" ||
        (step.kind != K::Unary && step.kind != K::Postfix) || step.binding != B::None || step.text != "++" ||
        step.args.size() != 1 || !named_local(*step.args[0],s.init->local_id)) return nullptr;
    return test.args[1].get();
}

// Masked and nonnegative literal fields admit a monotone wide reduction even
// when the input does not cover a complete period.
std::uint32_t record_field_bound(const Expr& value) {
    if (value.kind == K::Literal) return static_cast<std::uint32_t>(literal_value(value).as_int());
    if (value.kind == K::Binary && value.text == "&") {
        for (const auto& operand : value.args)
            if (operand->kind == K::Literal && literal_value(*operand).as_int() >= 0)
                return static_cast<std::uint32_t>(literal_value(*operand).as_int());
    }
    return UINT32_MAX;
}

// Every checked subexpression must be affine in the induction variable.
// Testing its endpoints then proves the entire interval safe. Bitwise nodes
// above the checked operations cannot fail.
bool record_formula(const Expr& e, std::vector<const Expr*>& checked) {
    const auto leaf = [](const Expr& value) {
        return value.type == "int" && (value.kind == K::Literal ||
            (value.kind == K::Name && value.binding == B::Local && !is_reference(value.declared_type)));
    };
    if (leaf(e)) return true;
    if (e.type != "int" || e.kind != K::Binary || e.binding != B::None || e.args.size() != 2) return false;
    if (e.text == "+" || e.text == "-") {
        if (!leaf(*e.args[0]) || !leaf(*e.args[1])) return false;
        checked.push_back(&e);
        return true;
    }
    return (e.text == "&" || e.text == "|" || e.text == "^") &&
        record_formula(*e.args[0],checked) && record_formula(*e.args[1],checked);
}
}

void Builder::find_private_record_buffers(const Stmt& scope) {
    if (!lower.options.optimize_loops || lower.module.instrument_budget) return;
    for (std::size_t i=0; i+1<scope.statements.size(); ++i) {
        const auto& var = *scope.statements[i];
        if (var.kind != RecordStmt::Variable || var.expression || generic_base(var.type) != "Bolt.Runtime.Buffer") continue;
        const auto& reserve = *scope.statements[i+1];
        if (reserve.kind != RecordStmt::Expression || !reserve.expression ||
            !record_buffer_call(*reserve.expression,var.local_id,"Reserve",1)) continue;
        const auto& capacity = *reserve.expression->args[1];
        if (capacity.kind != K::Literal || capacity.type != "int") continue;
        RecordBufferUses uses{var.local_id};
        if (uses.children(scope,i+2) && uses.clear)
            private_record_clears.emplace(uses.clear,PrivateRecordBuffer{var.local_id,static_cast<std::int32_t>(literal_value(capacity).as_int())});
    }
}

bool Builder::elide_record_refill(const Stmt& scope, std::size_t position) {
    const auto candidate = private_record_clears.find(scope.statements[position].get());
    if (candidate == private_record_clears.end()) return false;
    const auto& buffer = candidate->second;
    const auto& fill = *scope.statements[position+1];
    const auto& scan = *scope.statements[position+2];
    const auto* bound = record_loop_bound(fill);
    const auto* length = record_loop_bound(scan);
    if (!bound || bound->kind != K::Literal || !length || fill.body->kind != RecordStmt::Block) return false;
    const auto n = static_cast<std::int32_t>(literal_value(*bound).as_int());
    if (n <= 0 || n > buffer.capacity) return false;
    const auto* use = scan.body.get();
    if (use->kind == RecordStmt::Block && use->statements.size() == 1) use = use->statements.front().get();
    if (use->kind != RecordStmt::Expression || !use->expression ||
        !matches_record_sum(lower,scan,*length,*use->expression) || !named_local(*length->args[0],buffer.id)) return false;

    const auto& update = *use->expression;
    const auto& add = *update.args[1];
    const auto& fields = fill.body->statements;
    if (fields.size() < 3) return false;
    const auto& record = *fields.front();
    if (record.kind != RecordStmt::Variable || record.expression || record.type != add.args[0]->owner ||
        !lower.layout(record.type).zero_default) return false;
    const auto& append = *fields.back();
    if (append.kind != RecordStmt::Expression || !append.expression ||
        !record_buffer_call(*append.expression,buffer.id,"Add",1) ||
        !named_local(*append.expression->args[1],record.local_id)) return false;

    std::vector<const Expr*> writes, checked;
    const Expr* terms[2]{};
    for (std::size_t i=1; i+1<fields.size(); ++i) {
        const auto& st = *fields[i];
        if (st.kind != RecordStmt::Expression || !st.expression) return false;
        const auto& assignment = *st.expression;
        if (assignment.kind != K::Assign || assignment.binding != B::None || assignment.text != "=" ||
            assignment.args.size() != 2) return false;
        const auto& field = *assignment.args[0];
        const auto& rhs = *assignment.args[1];
        if (field.kind != K::Member || field.binding != B::Field || field.is_static || field.arrow ||
            field.owner != record.type || field.type != "int" || field.args.size() != 1 ||
            !named_local(*field.args[0],record.local_id) || !record_formula(rhs,checked)) return false;
        writes.push_back(&rhs);
        for (int side=0; side<2; ++side)
            if (field.text == add.args[side]->text) terms[side] = &rhs;
    }
    if (!terms[0] || !terms[1]) return false;
    const auto counter = fill.init->local_id;
    if (std::uint64_t{record_field_bound(*terms[0])}+record_field_bound(*terms[1]) > INT32_MAX) return false;
    const auto index = alloc(4);
    locals[counter] = index;
    const auto dest = locals.at(update.args[0]->local_id);
    const auto prior = cast(load(Type::I32,dest),Type::I64,"sext");
    const auto done = block();
    const auto a = full_period_mask(*terms[0],counter,n), b = full_period_mask(*terms[1],counter,n);
    if (a && b) {
        const auto slow = block(), publish = block();
        const auto outer_fallback = guard_fallback;
        guard_fallback = slow;
        for (const auto endpoint : {0,n-1}) {
            store(index,integer(Type::I32,endpoint));
            for (const auto* operation : checked) (void)expression(*operation);
        }
        guard_fallback = outer_fallback;
        const auto gain = static_cast<std::uint64_t>(n) * (std::uint64_t{*a}+*b) / 2;
        const auto result = instruction(Op::Add,Type::I64,{prior,integer(Type::I64,gain)});
        branch(cmp("sle",result,integer(Type::I64,INT32_MAX)),publish,slow);
        current = publish;
        store(dest,cast(result,Type::I32,"trunc")); jump(done);
        current = slow;
    }

    // For other lengths, or when a check may fail, forward values in source order. Defer
    // publishing the accumulator until construction succeeds, since the source
    // completes the entire fill before beginning the reduction. Terms are
    // nonnegative, so the last representable prefix is exactly what a handler
    // must observe when the original checked addition overflows.
    const auto wide = alloc(8), prefix = alloc(4);
    store(index,integer(Type::I32,0)); store(wide,prior); store(prefix,cast(prior,Type::I32,"trunc"));
    const auto replay = block(), finish = block();
    jump(replay);
    current = replay;
    Reg values[2]{};
    for (const auto* rhs : writes) {
        const auto value = expression(*rhs).value;
        for (int side=0; side<2; ++side) if (rhs == terms[side]) values[side] = value;
    }
    const auto pair = instruction(Op::Add,Type::I32,{values[0],values[1]});
    const auto accumulated = instruction(Op::Add,Type::I64,{load(Type::I64,wide),cast(pair,Type::I64,"zext")});
    const auto fits = cmp("sle",accumulated,integer(Type::I64,INT32_MAX));
    store(wide,accumulated);
    store(prefix,instruction(Op::Select,Type::I32,{fits,cast(accumulated,Type::I32,"trunc"),load(Type::I32,prefix)}));
    const auto next = instruction(Op::Add,Type::I32,{load(Type::I32,index),integer(Type::I32,1)});
    store(index,next);
    branch(cmp("ne",next,integer(Type::I32,n)),replay,finish);
    current = finish;
    store(dest,load(Type::I32,prefix));
    at = update.token.at;
    guard(cmp("sgt",load(Type::I64,wide),integer(Type::I64,INT32_MAX)),"integer overflow in +");
    jump(done);
    current = done;
    return true;
}

