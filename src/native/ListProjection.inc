// A private, pre-reserved int list can represent a counted refill by its
// index expression. Keep the real constructor and Reserve (including errors).
// Every use of the local must be accounted for before substituting any read.
namespace {
using StmtKind = Stmt::Kind;

bool list_member(const Expr& e, std::size_t id, std::string_view name, std::size_t arity,
                 std::string_view owner = "System.Collections.Generic.List<int>") {
    if (e.kind != K::Call || e.binding != B::Method || e.is_extension ||
        e.owner != owner || e.args.size() != arity+1) return false;
    const auto& member = *e.args[0];
    return member.kind == K::Member && !member.arrow && member.text == name &&
           member.args.size() == 1 && local_name(*member.args[0],id);
}

bool projected_formula(const Expr& e, std::size_t index, std::map<std::size_t,Reg>& inputs) {
    if (e.type != "int") return false;
    if (e.kind == K::Literal) return true;
    if (e.kind == K::Name && e.binding == B::Local && !is_reference(e.declared_type)) {
        if (e.local_id != index) inputs.emplace(e.local_id,0);
        return true;
    }
    return e.kind == K::Binary && e.binding == B::None && e.args.size() == 2 &&
           (e.text == "&" || e.text == "|" || e.text == "^") &&
           projected_formula(*e.args[0],index,inputs) && projected_formula(*e.args[1],index,inputs);
}

struct ProjectionUses {
    std::size_t id;
    std::int32_t capacity;
    Builder::ListProjection list;
    std::vector<const Expr*> calls;

    bool fill(const Stmt& s) {
        if (list.fill || s.kind != StmtKind::For || !s.init || s.init->kind != StmtKind::Variable ||
            s.init->type != "int" || !s.init->expression || s.init->expression->kind != K::Literal ||
            s.init->expression->type != "int" || literal_value(*s.init->expression).as_int() != 0 ||
            !s.condition || !s.increment || !s.body) return false;
        const auto id_index = s.init->local_id;
        const auto& test = *s.condition;
        const auto& step = *s.increment;
        if (test.kind != K::Binary || test.binding != B::None || test.text != "<" || test.args.size() != 2 ||
            !local_name(*test.args[0],id_index) || test.args[1]->kind != K::Literal || test.args[1]->type != "int" ||
            (step.kind != K::Postfix && step.kind != K::Unary) || step.binding != B::None || step.text != "++" ||
            step.args.size() != 1 || !local_name(*step.args[0],id_index)) return false;
        const auto length = literal_value(*test.args[1]).as_int();
        const auto* body = s.body.get();
        if (body->kind == StmtKind::Block && body->statements.size() == 1) body = body->statements.front().get();
        if (length < 0 || length > capacity || body->kind != StmtKind::Expression || !body->expression ||
            !list_member(*body->expression,id,"Add",1)) return false;
        const auto& value = *body->expression->args[1];
        if (!projected_formula(value,id_index,list.inputs)) return false;
        list.fill = &s;
        list.formula = &value;
        list.length = static_cast<std::int32_t>(length);
        return true;
    }

    bool expression(const Expr& e, bool value) {
        if (local_name(e,id) || e.lambda) return false;
        if (list_member(e,id,"@get.Count",0) || list_member(e,id,"@get.Length",0) ||
            (value && list_member(e,id,"@get.@indexer",1) &&
             (e.args[1]->kind == K::Literal || (e.args[1]->kind == K::Name &&
              e.args[1]->binding == B::Local && !is_reference(e.args[1]->declared_type))) &&
             e.args[1]->type == "int")) {
            calls.push_back(&e);
            return true;
        }
        if (e.arena && !expression(*e.arena,false)) return false;
        for (std::size_t i=0;i<e.args.size();++i) {
            const bool consumes = e.binding == B::None &&
                ((e.kind == K::Binary && e.text != "," && e.text != "??" &&
                  is_integer(e.args[i]->type)) ||
                 (e.kind == K::Assign && i == 1 && is_integer(e.args[0]->type)));
            if (!expression(*e.args[i],consumes)) return false;
        }
        return true;
    }

    bool statement(const Stmt& s) {
        if (s.expression && !expression(*s.expression,s.kind == StmtKind::Variable && is_integer(s.type) &&
            !is_reference(s.type))) return false;
        for (const auto* e : {s.condition.get(),s.increment.get()}) if (e && !expression(*e,false)) return false;
        for (const auto* sub : {s.init.get(),s.body.get(),s.alternative.get()}) if (sub && !statement(*sub)) return false;
        return children(s,0);
    }

    bool children(const Stmt& s,std::size_t begin) {
        for (std::size_t i=begin;i<s.statements.size();++i) {
            const auto& child = *s.statements[i];
            if (s.kind == StmtKind::Block && child.kind == StmtKind::Expression && child.expression &&
                list_member(*child.expression,id,"Clear",0)) {
                calls.push_back(child.expression.get());
                if (i+1 < s.statements.size() && fill(*s.statements[i+1])) ++i;
            } else if (!statement(child)) return false;
        }
        return true;
    }
};
}

void Builder::find_list_projections(const Stmt& scope) {
    if (!lower.options.optimize_loops || lower.module.instrument_budget) return;
    for (std::size_t i=0;i+1<scope.statements.size();++i) {
        const auto& var = *scope.statements[i];
        if (var.kind != StmtKind::Variable || var.type != "System.Collections.Generic.List<int>" ||
            var.expression || projected_lists.contains(var.local_id)) continue;
        const auto* cls = lower.program.find_class(var.type);
        if (!cls || cls->token.at.file != "<bolt-stdlib>/System/Collections/Generic/List.bolt") continue;
        const auto& reserve = *scope.statements[i+1];
        if (reserve.kind != StmtKind::Expression || !reserve.expression ||
            !list_member(*reserve.expression,var.local_id,"Reserve",1)) continue;
        const auto& bound = *reserve.expression->args[1];
        if (bound.kind != K::Literal || bound.type != "int") continue;
        ProjectionUses uses{var.local_id,static_cast<std::int32_t>(literal_value(bound).as_int())};
        if (!uses.children(scope,i+2) || !uses.list.fill) continue;
        for (auto& [id,slot] : uses.list.inputs) slot = alloc(4);
        uses.list.count = alloc(4);
        projected_fills.emplace(uses.list.fill,var.local_id);
        for (const auto* e : uses.calls) projected_calls.emplace(e,var.local_id);
        projected_lists.emplace(var.local_id,std::move(uses.list));
    }
}

void Builder::fill_projection(ListProjection& p) {
    for (auto [id,slot] : p.inputs) store(slot,load(Type::I32,locals.at(id)));
    store(p.count,integer(Type::I32,p.length));
}

static Reg projected_value(Builder& b,const Builder::ListProjection& p,const Expr& e,Reg index) {
    if (e.kind == K::Literal) return b.integer(Type::I32,literal_value(e).as_int());
    if (e.kind == K::Name)
        return e.local_id == p.fill->init->local_id ? index : b.load(Type::I32,p.inputs.at(e.local_id));
    const auto left = projected_value(b,p,*e.args[0],index);
    const auto right = projected_value(b,p,*e.args[1],index);
    return b.instruction(e.text=="&" ? Op::And : e.text=="|" ? Op::Or : Op::Xor,Type::I32,{left,right});
}

Value Builder::projected_call(const Expr& e,ListProjection& p) {
    const auto& name = e.args[0]->text;
    if (name == "Clear") {
        store(p.count,integer(Type::I32,0));
        return {"void",0};
    }
    const auto length = load(Type::I32,p.count);
    if (name != "@get.@indexer") return {"int",length};
    const auto index = expression(*e.args[1]).value;
    const auto valid = block(), invalid = block();
    branch(cmp("ult",index,length),valid,invalid);
    current = invalid;
    // The reserved backing list stays empty. An index outside the logical
    // length also fails there, with the original indexer and diagnostic frame.
    (void)call_expr(e);
    jump(failure);
    current = valid;
    return {"int",projected_value(*this,p,*p.formula,index)};
}

// AND, OR and XOR act independently on each bit. In an aligned power-of-two
// interval, each input bit is either constant or evenly split. Consequently
// the mean of the two endpoint values is the mean over the entire interval.
// All indices are nonnegative, so the projected sign bit is constant too:
// the final sum fits int exactly when every original addition fits.
bool Builder::projected_sum(const Stmt& loop,const Expr& limit,const Expr& update) {
    const auto found = projected_calls.find(&limit);
    if (found == projected_calls.end() || !loop.init->expression ||
        loop.init->expression->kind != K::Literal || loop.init->expression->type != "int" ||
        literal_value(*loop.init->expression).as_int() != 0 ||
        update.kind != K::Assign || update.text != "+=" || update.binding != B::None || update.args.size()!=2)
        return false;
    const auto id = found->second;
    const auto& acc = *update.args[0];
    const auto& read = *update.args[1];
    if ((!list_member(limit,id,"@get.Count",0) && !list_member(limit,id,"@get.Length",0)) ||
        !list_member(read,id,"@get.@indexer",1) || !local_name(*read.args[1],loop.init->local_id) ||
        acc.kind!=K::Name || acc.binding!=B::Local || acc.type!="int" || is_reference(acc.declared_type) ||
        acc.local_id==loop.init->local_id) return false;
    const auto& p = projected_lists.at(id);
    // Bound code expansion for irregular lengths. Ordinary indexing still
    // benefits from the projection when the sum is left as a loop.
    if (std::popcount(static_cast<unsigned>(p.length)) > 4) return false;
    const auto reduce=block(), publish=block(), replay=block(), done=block();
    branch(cmp("ne",load(Type::I32,p.count),integer(Type::I32,0)),reduce,done);
    current=reduce;
    const auto acc_slot = locals.at(acc.local_id);
    auto sum=cast(load(Type::I32,acc_slot),Type::I64,"sext");
    const auto value_at=[&](unsigned index) {
        return cast(projected_value(*this,p,*p.formula,integer(Type::I32,index)),Type::I64,"sext");
    };
    const auto length=static_cast<unsigned>(p.length);
    for (unsigned first=0;first<length;) {
        const auto span=std::bit_floor(length-first);
        auto part=value_at(first);
        if (span>1) {
            part=instruction(Op::Add,Type::I64,{part,value_at(first+span-1)});
            part=instruction(Op::Mul,Type::I64,{part,integer(Type::I64,span/2)});
        }
        sum=instruction(Op::Add,Type::I64,{sum,part});
        first+=span;
    }
    const auto narrowed=cast(sum,Type::I32,"trunc");
    branch(cmp("eq",sum,cast(narrowed,Type::I64,"sext")),publish,replay);
    current=publish;store(acc_slot,narrowed);jump(done);
    // Replay preserves the first failing source location and partial value.
    current=replay;ordinary_for(loop);jump(done);
    current=done;
    return true;
}
