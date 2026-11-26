// Sum signed int elements into independent long locals. Once length and
// accumulator headroom are proven, the loop has no exceptional exits and
// LLVM can vectorize the widening reductions.
bool Builder::widening_sequence_sum(const Stmt& loop, const Expr& limit) {
    using S = Stmt::Kind;
    const auto* first = loop.init->expression.get();
    if (!first || first->kind != K::Literal || first->type != "int" || literal_value(*first).as_int() != 0 ||
        limit.type != "int" || (limit.kind != K::Literal && (limit.kind != K::Name ||
        limit.binding != B::Local || is_reference(limit.declared_type) || limit.local_id == loop.init->local_id)))
        return false;

    struct Sum { Reg result; const Expr* source; Reg data = 0; };
    std::vector<Sum> sums;
    auto inspect = [&](const Stmt& st) {
        const auto* op = st.expression.get();
        if (st.kind != S::Expression || !op || op->kind != K::Assign || op->binding != B::None ||
            op->text != "+=" || op->args.size() != 2) return false;
        const auto& dest = *op->args[0];
        if (dest.kind != K::Name || dest.binding != B::Local || dest.type != "long" ||
            is_reference(dest.declared_type)) return false;
        const auto& value = *op->args[1];
        const auto* access = &value;
        if (value.kind == K::Call && value.binding == B::Cast && value.type == "long" && value.args.size() == 2)
            access = value.args[1].get();
        if (access->type != "int" || access->args.size() != 2) return false;
        const auto& index = *access->args[1];
        if (index.kind != K::Name || index.binding != B::Local || index.local_id != loop.init->local_id) return false;
        const Expr* source = access->args[0].get();
        if (access->kind == K::Call) {
            if (access->binding != B::Method || access->is_extension || source->kind != K::Member ||
                source->arrow || source->text != "@get.@indexer" || source->args.size() != 1 ||
                access->owner != "System.Collections.Generic.List<int>") return false;
            const auto* cls = lower.program.find_class(access->owner);
            if (!cls || cls->token.at.file != "<bolt-stdlib>/System/Collections/Generic/List.bolt") return false;
            source = source->args[0].get();
        } else if (access->kind != K::Index || access->binding != B::None ||
                   (source->type != "int[]" && source->type != "Bolt.Runtime.Buffer<int>")) return false;
        if (source->kind != K::Name || source->binding != B::Local || is_reference(source->declared_type) ||
            projected_lists.contains(source->local_id)) return false;
        const auto slot = locals.at(dest.local_id);
        if (std::any_of(sums.begin(),sums.end(),[&](const Sum& s){return s.result == slot;})) return false;
        sums.push_back({slot,source});
        return true;
    };
    if (loop.body->kind == S::Block) {
        for (const auto& st : loop.body->statements) if (!inspect(*st)) return false;
    } else if (!inspect(*loop.body)) return false;
    if (sums.empty()) return false;

    const auto fallback = block(), done = block(), positive = block();
    auto require = [&](Reg condition) {
        const auto next = block();
        branch(condition,next,fallback);
        current = next;
    };
    const auto stop = expression(limit).value;
    branch(cmp("sgt",stop,integer(Type::I32,0)),positive,done);
    current = positive;
    const auto length = cast(stop,Type::I64,"zext");
    // Every possible prefix lies between initial - n*2^31 and
    // initial + n*(2^31-1). These headroom thresholds themselves fit i64.
    const auto low = instruction(Op::Add,Type::I64,{integer(Type::I64,UINT64_C(1)<<63),
        instruction(Op::Shl,Type::I64,{length,integer(Type::I64,31)})});
    const auto high = instruction(Op::Sub,Type::I64,{integer(Type::I64,INT64_MAX),
        instruction(Op::Mul,Type::I64,{length,integer(Type::I64,INT32_MAX)})});
    for (auto& sum : sums) {
        const auto& source = *sum.source;
        auto slot = locals.at(source.local_id);
        if (source.type == "System.Collections.Generic.List<int>") slot = lower.field(*this,source.type,"storage",slot);
        const auto header = load(Type::Ptr,slot);
        require(cmp("ne",header,null()));
        require(cmp("ule",length,load(Type::I64,offset(header,offsetof(BrSequenceHeader,count)))));
        sum.data = load_sequence_data(header);
        const auto initial = load(Type::I64,sum.result);
        require(cmp("sge",initial,low));
        require(cmp("sle",initial,high));
    }
    const auto cursor = alloc(8), scan = block();
    store(cursor,integer(Type::I64,0));
    jump(scan);
    current = scan;
    const auto i = load(Type::I64,cursor);
    const auto bytes = instruction(Op::Mul,Type::I64,{i,integer(Type::I64,4)});
    for (const auto& sum : sums) {
        const auto value = load(Type::I32,instruction(Op::Offset,Type::Ptr,{sum.data,bytes}));
        store(sum.result,instruction(Op::Add,Type::I64,
            {load(Type::I64,sum.result),cast(value,Type::I64,"sext")},0,"nsw"));
    }
    const auto next = instruction(Op::Add,Type::I64,{i,integer(Type::I64,1)});
    store(cursor,next);
    branch(cmp("ne",next,length),scan,done,4);
    current = fallback;
    ordinary_for(loop);
    jump(done);
    current = done;
    return true;
}
