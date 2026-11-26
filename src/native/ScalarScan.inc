// Peel a short, provably representable prefix of a scalar byte scan. A single
// preheader proves the cursor and all byte loads in the prefix. Interval
// arithmetic proves every intermediate operation of the accumulator update.
// Long fields and the end of the buffer resume the original checked loop.
bool Builder::peel_scalar_scan(const Stmt& seed, const Stmt& loop) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget || loop.kind != S::While ||
        seed.kind != S::Variable || seed.type != "int" || !seed.expression ||
        seed.expression->kind != K::Literal || seed.expression->type != "int" ||
        !loop.condition || !loop.body || loop.body->kind != S::Block || loop.body->statements.size() != 2)
        return false;
    const auto& condition = *loop.condition;
    if (condition.kind != K::Binary || condition.binding != B::None || condition.text != "!=" ||
        condition.args.size() != 2 || condition.args[1]->kind != K::Literal || condition.args[1]->type != "int")
        return false;
    const auto& input = *condition.args[0];
    const auto* view = private_array(input);
    if (!view || !view->data || input.owner != "byte[]" || input.binding != B::None ||
        input.args[1]->kind != K::Name || input.args[1]->binding != B::Local ||
        !stack_ints.contains(input.args[1]->local_id)) return false;
    const auto cursor = input.args[1]->local_id;
    auto local = [](const Expr& e, std::size_t id) {
        return e.kind == K::Name && e.binding == B::Local && e.local_id == id;
    };
    const auto& statements = loop.body->statements;
    const auto* update = statements[0]->expression.get();
    const auto* step = statements[1]->expression.get();
    if (cursor == seed.local_id || statements[0]->kind != S::Expression || !update ||
        update->kind != K::Assign || update->binding != B::None || update->text != "=" ||
        update->args.size() != 2 || !local(*update->args[0],seed.local_id) ||
        statements[1]->kind != S::Expression || !step || step->kind != K::Postfix ||
        step->binding != B::None || step->text != "++" || step->args.size() != 1 ||
        !local(*step->args[0],cursor)) return false;

    using Interval = std::pair<std::int64_t,std::int64_t>;
    auto range = [&](auto&& self,const Expr& e,Interval value) -> std::optional<Interval> {
        if (local(e,seed.local_id)) return value;
        if (e.kind == K::Literal && e.type == "int") {
            const auto n = literal_value(e).as_int();
            return Interval{n,n};
        }
        if (e.kind == K::Index && e.binding == B::None && e.owner == input.owner && e.args.size() == 2 &&
            local(*e.args[0],input.args[0]->local_id) && local(*e.args[1],cursor)) return Interval{0,255};
        if (e.kind != K::Binary || e.binding != B::None || e.type != "int" || e.args.size() != 2) return {};
        const auto a = self(self,*e.args[0],value), b = self(self,*e.args[1],value);
        if (!a || !b) return {};
        Interval result;
        if (e.text == "+") result = {a->first+b->first,a->second+b->second};
        else if (e.text == "-") result = {a->first-b->second,a->second-b->first};
        else if (e.text == "*") {
            const auto ends = std::minmax({a->first*b->first,a->first*b->second,a->second*b->first,a->second*b->second});
            result = {ends.first,ends.second};
        } else return {};
        return result.first >= INT32_MIN && result.second <= INT32_MAX ? std::optional{result} : std::nullopt;
    };
    const auto start = literal_value(*seed.expression).as_int();
    Interval value{start,start};
    unsigned prefix = 0;
    for (; prefix < 6; ++prefix) {
        auto next = range(range,*update->args[1],value);
        if (!next) break;
        value = *next;
    }
    if (prefix < 2) return false;

    // A negative int, zero-extended, is already above INT_MAX. One unsigned
    // comparison covers both array bounds and representability of cursor++.
    const auto position = cast(load(Type::I32,locals.at(cursor)),Type::I64,"zext");
    const auto count = integer(Type::I64,prefix);
    const auto end = instruction(Op::Add,Type::I64,{position,count});
    const auto maximum = integer(Type::I64,INT32_MAX);
    const auto limit = instruction(Op::Select,Type::I64,{cmp("ult",view->length,maximum),view->length,maximum});
    const auto fast = block(), slow = block(), done = block();
    branch(cmp("ule",end,limit),fast,slow);
    current = fast;
    const auto data = view->data;
    const auto delimiter = integer(Type::I32,literal_value(*condition.args[1]).as_int());
    Reg accumulator = integer(Type::I32,start);
    for (unsigned i = 0; i < prefix; ++i) {
        const auto offset = instruction(Op::Add,Type::I64,{position,integer(Type::I64,i)});
        const auto byte = cast(load(Type::I8,instruction(Op::Offset,Type::Ptr,{data,offset})),Type::I32,"zext");
        const auto body = block();
        branch(cmp("eq",byte,delimiter),done,body);
        current = body;
        auto evaluate = [&](auto&& self,const Expr& e) -> Reg {
            if (e.kind == K::Name) return accumulator;
            if (e.kind == K::Index) return byte;
            if (e.kind == K::Literal) return integer(Type::I32,literal_value(e).as_int());
            const auto a = self(self,*e.args[0]), b = self(self,*e.args[1]);
            return instruction(e.text == "+" ? Op::Add : e.text == "-" ? Op::Sub : Op::Mul,Type::I32,{a,b},0,"nsw");
        };
        accumulator = evaluate(evaluate,*update->args[1]);
        store(locals.at(seed.local_id),accumulator);
        const auto next = instruction(Op::Add,Type::I64,{offset,integer(Type::I64,1)});
        store(locals.at(cursor),cast(next,Type::I32,"trunc"));
    }
    jump(slow);
    current = slow;
    statement(loop);
    jump(done);
    current = done;
    return true;
}
