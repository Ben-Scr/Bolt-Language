void Builder::load_array_view(PrivateArray& view, Reg header) {
    const auto data = alloc(sizeof(void*),false), count = alloc(8,false);
    store(data,null());
    store(count,integer(Type::I64,0));
    const auto present = block(), done = block();
    branch(cmp("ne",header,null()),present,done);
    current = present;
    store(data,load_sequence_data(header));
    store(count,load(Type::I64,offset(header,offsetof(BrSequenceHeader,count))));
    jump(done);
    current = done;
    view.data = load(Type::Ptr,data);
    view.length = load(Type::I64,count);
}

// Scalar array element stores cannot rebind an array. When these and ordinary
// local scalar operations are the only effects of the whole loop nest, a view
// loaded at entry remains valid even for aliased array reference parameters.
// Keep the null handle intact until an actual invalid access calls br_index.
std::vector<std::size_t> Builder::loop_array_views(const Stmt& loop) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget) return {};
    std::set<std::size_t> arrays;
    auto scalar = [](const std::string& t) { return is_numeric(t) || t == "bool"; };
    auto expression_ok = [&](auto&& visit, const Expr& e) -> bool {
        if (e.arena || !scalar(e.type)) return false;
        if (e.kind == K::Index && e.binding == B::None && e.args.size() == 2 &&
            is_array(e.owner) && array_rank(e.owner) == 1) {
            const auto& base = *e.args[0];
            if (base.kind != K::Name || base.binding != B::Local || !locals.contains(base.local_id)) return false;
            if (!private_arrays.contains(base.local_id) && !indexed_arrays.contains(base.local_id))
                arrays.insert(base.local_id);
            return visit(visit,*e.args[1]);
        }
        if (e.kind == K::Name) return e.binding == B::Local && !is_reference(e.declared_type);
        if (e.kind == K::Literal) return true;
        if (e.kind == K::Call) return e.binding == B::Cast && e.args.size() == 2 && visit(visit,*e.args[1]);
        if (e.binding != B::None) return false;
        if (e.kind == K::Unary && (e.text == "*" || e.text == "&")) return false;
        if (e.kind != K::Assign && e.kind != K::Unary && e.kind != K::Postfix &&
            e.kind != K::Binary && e.kind != K::Conditional) return false;
        return std::all_of(e.args.begin(),e.args.end(),[&](const auto& arg){return visit(visit,*arg);});
    };
    auto safe = [&](auto&& visit, const Stmt& s) -> bool {
        if (s.kind == S::Variable && !scalar(s.type)) return false;
        if (s.kind != S::Variable && s.kind != S::Block && s.kind != S::Expression && s.kind != S::If &&
            s.kind != S::While && s.kind != S::For && s.kind != S::Break && s.kind != S::Continue) return false;
        for (const auto* e : {s.init.get(),s.body.get(),s.alternative.get()})
            if (e && !visit(visit,*e)) return false;
        for (const auto* e : {s.expression.get(),s.condition.get(),s.increment.get()})
            if (e && !expression_ok(expression_ok,*e)) return false;
        return std::all_of(s.statements.begin(),s.statements.end(),[&](const auto& child){return visit(visit,*child);});
    };
    if (!safe(safe,loop)) return {};
    for (const auto id : arrays) {
        auto& view = private_arrays[id];
        view.maximum.reset();
        view.nullable = true;
        load_array_view(view,load(Type::Ptr,locals.at(id)));
    }
    return {arrays.begin(),arrays.end()};
}
