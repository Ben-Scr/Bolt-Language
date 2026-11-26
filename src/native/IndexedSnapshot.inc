// Scalar replacement of an immutable indexed snapshot. The producer must be
// total, independent of mutable state, and initialized by its zero-based index.
// An address, a reference, a second writer or an unknown use forces materialization.
static bool local_name(const Expr& e, std::size_t id) {
    return e.kind == K::Name && e.binding == B::Local && e.local_id == id;
}

// Return a mask of all bits that could be set. A checked addition may be
// deferred to a read only if it is total, even for elements never read.
static std::optional<std::uint32_t> index_formula(const Expr& e, std::size_t index,
                                                  std::map<std::size_t,Reg>& captured) {
    if (e.type != "int") return {};
    if (e.kind == K::Literal) return static_cast<std::uint32_t>(literal_value(e).as_int());
    if (e.kind == K::Name && e.binding == B::Local && !is_reference(e.declared_type)) {
        if (e.local_id == index) return INT32_MAX;
        captured.emplace(e.local_id, 0);
        return UINT32_MAX;
    }
    if (e.kind != K::Binary || e.binding != B::None || e.args.size() != 2) return {};
    const auto a = index_formula(*e.args[0], index, captured);
    const auto b = index_formula(*e.args[1], index, captured);
    if (!a || !b) return {};
    if (e.text == "&") return *a & *b;
    if (e.text == "|" || e.text == "^") return *a | *b;
    if (e.text == "+" && std::uint64_t{*a} + *b <= INT32_MAX)
        return (std::uint32_t{1} << std::bit_width(*a + *b)) - 1;
    return {};
}

// In a bitwise expression each output bit depends only on the corresponding
// input bit. Over [0, 2^k), complementary indices therefore have a constant
// pair sum. Addition preserves this property; masking a sum with carries need not.
static bool complementary_sum(const Expr& e, bool allow_add = true) {
    if (e.kind == K::Literal || e.kind == K::Name) return true;
    const bool add = e.text == "+";
    return (!add || allow_add) && complementary_sum(*e.args[0], add) &&
           complementary_sum(*e.args[1], add);
}

static bool safe_array_read(const Expr& e, std::size_t id, bool by_value) {
    if (local_name(e, id)) return false;
    if (e.arena && !safe_array_read(*e.arena, id, false)) return false;
    if (by_value && !e.args.empty() && local_name(*e.args[0], id)) {
        if (e.kind == K::Index && e.args.size() == 2 && e.args[1]->type == "int")
            return safe_array_read(*e.args[1], id, true);
        if (e.kind == K::Member && e.binding == B::Property && !e.arrow &&
            (e.text == "Length" || e.text == "Count" || e.text == "Rank")) return true;
    }
    for (std::size_t n = 0; n < e.args.size(); ++n) {
        // A value-consuming builtin operator or explicit scalar cast cannot
        // expose an element address. All other contexts require ordinary storage.
        const bool value = (by_value && e.kind == K::Binary && e.binding == B::None) ||
            (e.kind == K::Assign && e.binding == B::None && n == 1) ||
            (e.kind == K::Call && e.binding == B::Cast && n == 1 && is_numeric(e.type));
        if (!safe_array_read(*e.args[n], id, value)) return false;
    }
    return true;
}

static bool safe_array_uses(const Stmt& s, std::size_t id, bool reference_return) {
    using S = Stmt::Kind;
    const bool value = (s.kind == S::Variable && !is_reference(s.type)) || s.kind == S::Expression ||
                       (s.kind == S::Return && !reference_return);
    if (s.expression && !safe_array_read(*s.expression, id, value)) return false;
    if (s.condition && !safe_array_read(*s.condition, id, true)) return false;
    if (s.increment && !safe_array_read(*s.increment, id, true)) return false;
    for (const auto* child : {s.init.get(), s.body.get(), s.alternative.get()})
        if (child && !safe_array_uses(*child, id, reference_return)) return false;
    return std::all_of(s.statements.begin(), s.statements.end(), [&](const auto& child) {
        return safe_array_uses(*child, id, reference_return);
    });
}

bool Builder::indexed_snapshot(const Stmt& block, std::size_t position) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget || failure != error ||
        block.statements.size() - position < 3) return false;
    const auto& source = *block.statements[position];
    const auto& loop = *block.statements[position + 1];
    const auto& array = *block.statements[position + 2];
    if (source.kind != S::Variable || source.expression || array.kind != S::Variable ||
        !array.expression || !consumed_snapshots.contains(array.expression.get()) || array.type != "int[]" ||
        !local_name(*array.expression->args[0]->args[0], source.local_id)) return false;
    if (loop.kind != S::For || !loop.init || loop.init->kind != S::Variable || loop.init->type != "int" ||
        !loop.init->expression || loop.init->expression->kind != K::Literal || loop.init->expression->text != "0" ||
        !loop.condition || !loop.increment || !loop.body) return false;
    const auto index = loop.init->local_id;
    const auto& condition = *loop.condition;
    const auto& step = *loop.increment;
    if (condition.kind != K::Binary || condition.binding != B::None || condition.text != "<" ||
        condition.args.size() != 2 || !local_name(*condition.args[0], index) ||
        (step.kind != K::Postfix && step.kind != K::Unary) || step.binding != B::None ||
        step.text != "++" || step.args.size() != 1 || !local_name(*step.args[0], index)) return false;
    const auto& limit = *condition.args[1];
    if (limit.type != "int" || (limit.kind != K::Literal &&
        (limit.kind != K::Name || limit.binding != B::Local || limit.local_id == index ||
         is_reference(limit.declared_type)))) return false;
    const Stmt* body = loop.body.get();
    if (body->kind == S::Block && body->statements.size() == 1) body = body->statements[0].get();
    if (body->kind != S::Expression || !body->expression) return false;
    const auto& add = *body->expression;
    std::map<std::size_t,Reg> captured;
    if (add.kind != K::Call || add.binding != B::Collection || add.text != "Add" ||
        add.args.size() != 2 || add.args[0]->kind != K::Member || add.args[0]->arrow ||
        add.args[0]->args.size() != 1 || !local_name(*add.args[0]->args[0], source.local_id) ||
        add.args[1]->type != "int") return false;
    const auto bits = index_formula(*add.args[1], index, captured);
    if (!bits) return false;
    for (auto n = position + 3; n < block.statements.size(); ++n)
        if (!safe_array_uses(*block.statements[n], array.local_id, is_reference(return_type))) return false;
    // Freeze external operands and the bound at the producer. Later reads can
    // occur after the original locals have changed, including through aliases.
    for (auto& [id, value] : captured) value = load(Type::I32, locals.at(id));
    auto bound = expression(limit).value;
    auto length = alloc(4);
    auto zero = integer(Type::I32, 0);
    auto positive = this->block(), done = this->block();
    store(length, zero);
    branch(cmp("sgt", bound, zero), positive, done);
    current = positive;
    store(length, bound);
    jump(done);
    current = done;
    indexed_arrays[array.local_id] = {index, add.args[1].get(), load(Type::I32, length), std::move(captured),
        *bits <= INT32_MAX && complementary_sum(*add.args[1])};
    return true;
}

Value Builder::indexed_element(const IndexedArray& array, Reg index) {
    auto emit_value = [&](auto&& visit, const Expr& e) -> Reg {
        if (e.kind == K::Literal) return integer(Type::I32, literal_value(e).as_int());
        if (e.kind == K::Name) return e.local_id == array.index ? index : array.captured.at(e.local_id);
        const auto a = visit(visit, *e.args[0]);
        const auto b = visit(visit, *e.args[1]);
        if (e.text == "+") return instruction(Op::Add, Type::I32, {a,b}, 0, "nsw");
        const auto op = e.text == "&" ? Op::And : e.text == "|" ? Op::Or : Op::Xor;
        return instruction(op, Type::I32, {a,b});
    };
    return {"int", emit_value(emit_value, *array.formula)};
}
