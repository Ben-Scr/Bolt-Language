// Version counted loops over scalar arrays. For affine indices test both endpoints of each intermediate
// arithmetic operation before using the unchecked loop. On failure the ordinary
// loop retains its original order of stores, side effects and exceptions.
bool Builder::version_array_loop(const Stmt& loop, std::int32_t stride) {
    using S = Stmt::Kind;
    const auto induction = loop.init->local_id;
    const auto* first = loop.init->expression.get();
    const auto* limit = loop.condition->args[1].get();
    if (!first || !affine_data.empty() || loop.body->kind != S::Block) return false;
    std::unordered_map<std::size_t, const Expr*> definitions;
    std::vector<const Expr*> accesses;
    std::unordered_set<const Expr*> arithmetic_ops;
    std::unordered_map<std::size_t, int> local_degrees;
    // An expression may depend on the counter or on one frozen table value.
    // Combining both axes would require checking more than two endpoints.
    enum { invariant, counter_axis, unknown, table_axis };
    std::optional<std::size_t> table;
    std::unordered_set<std::size_t> dependencies;
    std::unordered_map<std::size_t,unsigned> mutations;
    std::unordered_set<std::size_t> decreasing;
    std::vector<const Expr*> advancing;
    std::unordered_set<const Expr*> before_first_step;
    // A local initialized in the loop can receive a different value along a
    // branch. Its initializer then cannot serve as an affine substitution.
    std::unordered_set<std::size_t> assigned;
    std::unordered_set<std::size_t> moving;
    auto find_steps = [&](auto&& self,const Expr& e) -> void {
        if (e.kind == K::Postfix && e.binding == B::None && e.args.size() == 1 &&
            e.args[0]->binding == B::Local) moving.insert(e.args[0]->local_id);
        for (const auto& arg : e.args) self(self,*arg);
    };
    auto find_assignments = [&](auto&& self,const Stmt& s) -> void {
        for (const auto* expr : {s.expression.get(),s.condition.get()}) if (expr) find_steps(find_steps,*expr);
        const auto* e = s.expression.get();
        if (s.kind == S::Expression && e && e->kind == K::Assign && !e->args.empty() &&
            e->args[0]->binding == B::Local) assigned.insert(e->args[0]->local_id);
        for (const auto* st : {s.body.get(),s.alternative.get()}) if (st) self(self,*st);
        for (const auto& st : s.statements) self(self,*st);
    };
    find_assignments(find_assignments,*loop.body);
    auto degree = [&](auto&& self, const Expr& e) -> int {
        if (e.type != "int") return 2;
        if (e.kind == K::Literal) return 0;
        if (e.kind == K::Name && e.binding == B::Local) {
            if (assigned.contains(e.local_id) || moving.contains(e.local_id)) return unknown;
            if (e.local_id == induction) return 1;
            if (auto v = definitions.find(e.local_id); v != definitions.end()) {
                if (auto d = local_degrees.find(e.local_id); d != local_degrees.end()) return d->second;
                const int d = self(self,*v->second);
                local_degrees.emplace(e.local_id,d);
                return d;
            }
            dependencies.insert(e.local_id);
            return stack_ints.contains(e.local_id) ? invariant : unknown;
        }
        if (e.kind == K::Index && e.binding == B::None && e.args.size() == 2 &&
            e.args[0]->binding == B::Local && e.args[1]->binding == B::Local &&
            e.args[0]->kind == K::Name && e.args[1]->kind == K::Name && e.args[1]->local_id == induction) {
            const auto id = e.args[0]->local_id;
            if (auto it = private_arrays.find(id); it != private_arrays.end() && it->second.frozen_min &&
                (!table || *table == id)) {
                table = id;
                return table_axis;
            }
        }
        if (e.kind != K::Binary || e.binding != B::None ||
            (e.text != "+" && e.text != "-" && e.text != "*")) return 2;
        const int lhs = self(self,*e.args[0]), rhs = self(self,*e.args[1]);
        if (lhs == unknown || rhs == unknown ||
            (lhs && rhs && (lhs != rhs || e.text == "*"))) return unknown;
        return std::max(lhs,rhs);
    };
    const bool length_limit = limit->kind == K::Member && limit->binding == B::Property &&
        limit->text == "Length" && !limit->arrow && limit->args.size() == 1 &&
        limit->args[0]->kind == K::Name && limit->args[0]->binding == B::Local && is_array(limit->args[0]->type);
    if (degree(degree,*first) != 0 || (!length_limit && degree(degree,*limit) != 0)) return false;
    auto pure = [&](auto&& self, const Expr& e) -> bool {
        if (e.kind == K::Index) {
            if (e.args.size() != 2 || e.binding != B::None || array_rank(e.owner) != 1 ||
                !is_array(e.owner) || !is_numeric(e.type) || e.args[0]->kind != K::Name ||
                e.args[0]->binding != B::Local) return false;
            if (degree(degree,*e.args[1]) == unknown) {
                const auto& key = *e.args[1];
                if (!self(self,key)) return false;
                if ((key.kind == K::Postfix && key.text == "++") ||
                    (key.kind == K::Name && moving.contains(key.local_id))) {
                    const auto* array = private_array(e);
                    if (array && array->data) {
                        advancing.push_back(&e);
                        if (key.kind == K::Name && !mutations.contains(key.local_id)) before_first_step.insert(&e);
                    }
                }
                return true;
            }
            accesses.push_back(&e);
            return true;
        }
        if (e.type != "bool" && !is_numeric(e.type)) return false;
        if (e.kind == K::Name) return e.binding == B::Local;
        if (e.kind == K::Literal) return true;
        if (e.kind == K::Postfix && (e.text == "++" || e.text == "--") && e.binding == B::None &&
            e.args.size() == 1 && e.args[0]->kind == K::Name && e.args[0]->binding == B::Local &&
            stack_ints.contains(e.args[0]->local_id) && e.args[0]->local_id != induction) {
            ++mutations[e.args[0]->local_id];
            if (e.text == "--") decreasing.insert(e.args[0]->local_id);
            return true;
        }
        if (e.kind == K::Call)
            return e.binding == B::Cast && e.args.size() == 2 && self(self,*e.args[1]);
        if (e.binding != B::None || (e.kind != K::Binary && e.kind != K::Conditional &&
            !(e.kind == K::Unary && (e.text == "-" || e.text == "+" || e.text == "!" || e.text == "~")))) return false;
        return std::all_of(e.args.begin(),e.args.end(),[&](const auto& arg){return self(self,*arg);});
    };
    auto safe_body = [&](auto&& self, const Stmt& st) -> bool {
        if (st.kind == S::Block)
            return std::all_of(st.statements.begin(),st.statements.end(),[&](const auto& part){return self(self,*part);});
        if (st.kind == S::If)
            return pure(pure,*st.condition) && self(self,*st.body) && (!st.alternative || self(self,*st.alternative));
        const auto* e = st.expression.get();
        if (st.kind == S::Variable && (is_numeric(st.type) || st.type == "bool") &&
            e && pure(pure,*e)) {
            definitions.emplace(st.local_id,e);
            return true;
        }
        if (st.kind == S::Expression && e && e->kind == K::Assign && e->binding == B::None &&
            e->text == "=" && e->args.size() == 2 && e->args[0]->kind == K::Name &&
            e->args[0]->binding == B::Local && definitions.contains(e->args[0]->local_id))
            return pure(pure,*e->args[1]);
        if (st.kind != S::Expression || !e || e->binding != B::None ||
            e->args.empty() || e->args[0]->kind != K::Index || !pure(pure,*e->args[0])) return false;
        if (e->kind == K::Postfix || e->kind == K::Unary) return e->text == "++" || e->text == "--";
        return e->kind == K::Assign && e->args.size() == 2 && pure(pure,*e->args[1]);
    };
    if (!safe_body(safe_body,*loop.body) || (accesses.size() < 2 && advancing.size() < 2)) return false;
    for (auto [id,uses] : mutations) if (dependencies.contains(id) || definitions.contains(id)) return false;
    // A single increment guarded by a sentinel cannot cross it when the
    // cursor starts below it. Short-circuit evaluation also protects reads
    // in the rest of the predicate. The arm taken at equality must not use
    // the cursor at all.
    std::optional<std::pair<std::size_t,std::size_t>> sentinel;
    if (loop.body->statements.size() == 1 && loop.body->statements[0]->kind == S::If) {
        const auto& choice = *loop.body->statements[0];
        const auto& cond = *choice.condition;
        if (cond.kind == K::Binary && cond.binding == B::None && (cond.text == "||" || cond.text == "&&")) {
            const auto& stop = *cond.args[0];
            const auto* excluded = cond.text == "||" ? choice.body.get() : choice.alternative.get();
            if (stop.kind == K::Binary && stop.binding == B::None && stop.text == (cond.text == "||" ? "==" : "!=") &&
                stop.args[0]->kind == K::Name && stop.args[1]->kind == K::Name &&
                stop.args[0]->type == "int" && stop.args[1]->type == "int" &&
                stop.args[0]->binding == B::Local && stop.args[1]->binding == B::Local) {
                const auto id = stop.args[0]->local_id, bound = stop.args[1]->local_id;
                auto mentions = [&](auto&& self,const Expr& e) -> bool {
                    return (e.binding == B::Local && e.local_id == id) ||
                        std::any_of(e.args.begin(),e.args.end(),[&](const auto& arg){return self(self,*arg);});
                };
                auto uses = [&](auto&& self,const Stmt& s) -> bool {
                    for (const auto* e : {s.expression.get(),s.condition.get()}) if (e && mentions(mentions,*e)) return true;
                    for (const auto* st : {s.body.get(),s.alternative.get()}) if (st && self(self,*st)) return true;
                    return std::any_of(s.statements.begin(),s.statements.end(),[&](const auto& st){return self(self,*st);});
                };
                if (mutations.contains(id) && mutations.at(id) == 1 && stack_ints.contains(bound) &&
                    bound != induction && !moving.contains(bound) && !assigned.contains(bound) &&
                    !definitions.contains(bound) && (!excluded || !uses(uses,*excluded))) sentinel = {{id,bound}};
            }
        }
    }
    const auto slow = block(), done = block();
    auto require = [&](Reg condition) {
        const auto passed = block();
        branch(condition,passed,slow);
        current = passed;
    };
    // Only int operands reach here. A wide binary operation cannot overflow
    // int64 before its result is checked against the source int range.
    using Values = std::unordered_map<const Expr*, Reg>;
    Values low_values, high_values;
    auto address_value = [&](auto&& self, const Expr& e, Reg counter, Values& memo, bool index_root) -> Reg {
        if (auto it = memo.find(&e); it != memo.end()) return it->second;
        Reg value;
        if (e.kind == K::Literal) value = integer(Type::I64,literal_value(e).as_int());
        else if (e.kind == K::Name) {
            if (e.local_id == induction) value = counter;
            else if (auto it = definitions.find(e.local_id); it != definitions.end())
                value = self(self,*it->second,counter,memo,index_root);
            else value = cast(load(Type::I32,locals.at(e.local_id)),Type::I64,"sext");
        } else if (e.kind == K::Index) {
            const auto& fact = private_arrays.at(e.args[0]->local_id);
            value = &memo == &low_values ? fact.frozen_min : fact.frozen_max;
        } else {
            arithmetic_ops.insert(&e);
            const auto lhs = self(self,*e.args[0],counter,memo,false), rhs = self(self,*e.args[1],counter,memo,false);
            value = instruction(e.text=="+" ? Op::Add : e.text=="-" ? Op::Sub : Op::Mul,Type::I64,{lhs,rhs});
            if (!index_root) {
                require(cmp("sge",value,integer(Type::I64,static_cast<std::uint64_t>(INT32_MIN))));
                require(cmp("sle",value,integer(Type::I64,INT32_MAX)));
            }
        }
        memo.emplace(&e,value);
        return value;
    };
    const auto start = address_value(address_value,*first,0,low_values,false);
    Reg end;
    if (length_limit) {
        const auto header = load(Type::Ptr,locals.at(limit->args[0]->local_id));
        require(cmp("ne",header,null()));
        end = load(Type::I64,offset(header,offsetof(BrSequenceHeader,count)));
        require(cmp("ule",end,integer(Type::I64,INT32_MAX)));
    } else end = address_value(address_value,*limit,0,low_values,false);
    const auto count = instruction(Op::Sub,Type::I64,{end,start});
    require(cmp("sge",count,integer(Type::I64,advancing.size() >= 2 ? 1 : table ? 4 : 16)));
    auto last = instruction(Op::Sub,Type::I64,{end,integer(Type::I64,1)});
    if (stride > 1) {
        const auto distance = instruction(Op::Sub,Type::I64,{last,start});
        const auto remainder = instruction(Op::URem,Type::I64,{distance,integer(Type::I64,stride)});
        last = instruction(Op::Sub,Type::I64,{last,remainder});
        require(cmp("sle",last,integer(Type::I64,INT32_MAX-stride)));
    }
    std::unordered_map<std::size_t,std::pair<Reg,Reg>> arrays;
    std::vector<std::pair<const Expr*,Reg>> index_limits;
    std::unordered_map<std::size_t,std::size_t> local_limits;
    for (const auto* access : accesses) {
        const auto id = access->args[0]->local_id;
        if (!arrays.contains(id)) {
            if (auto fact = private_arrays.find(id); fact != private_arrays.end() && fact->second.data) {
                arrays[id] = {fact->second.data,fact->second.length};
            } else {
                const auto header = load(Type::Ptr,locals.at(id));
                require(cmp("ne",header,null()));
                arrays[id] = {load_sequence_data(header),load(Type::I64,offset(header,offsetof(BrSequenceHeader,count)))};
            }
        }
        const auto& index = *access->args[1];
        affine_data.emplace(access,arrays.at(id).first);
        const auto length = arrays.at(id).second;
        if (index.kind == K::Name && index.binding == B::Local) {
            auto [it,added] = local_limits.emplace(index.local_id,index_limits.size());
            if (!added) {
                auto& previous = index_limits[it->second].second;
                if (length != previous)
                    previous = instruction(Op::Select,Type::I64,{cmp("ult",length,previous),length,previous});
                continue;
            }
        }
        index_limits.emplace_back(&index,length);
    }
    for (const auto& [index,length] : index_limits) {
        // A single unsigned bound also proves the final int operation did
        // not overflow. Intermediate expressions still need their own checks.
        const auto int_limit = integer(Type::I64,static_cast<std::uint64_t>(INT32_MAX)+1);
        const auto bound = instruction(Op::Select,Type::I64,{cmp("ult",length,int_limit),length,int_limit});
        require(cmp("ult",address_value(address_value,*index,start,low_values,true),bound));
        require(cmp("ult",address_value(address_value,*index,last,high_values,true),bound));
    }
    affine_ops = std::move(arithmetic_ops);
    // Each syntactic increment can execute at most once per iteration.
    // Counting both branches gives a conservative bound on the last cursor.
    std::set<std::tuple<std::size_t,std::size_t,bool>> guarded;
    for (const auto* access : advancing) {
        const auto& key = *access->args[1];
        const bool increments = key.kind == K::Postfix;
        const auto id = increments ? key.args[0]->local_id : key.local_id;
        if (decreasing.contains(id) || mutations.at(id) > 32) continue;
        const auto& array = private_arrays.at(access->args[0]->local_id);
        const bool capped = sentinel && sentinel->first == id;
        const bool exclusive_end = increments || before_first_step.contains(access);
        if (guarded.emplace(id,access->args[0]->local_id,exclusive_end).second) {
            const auto cursor = cast(load(Type::I32,locals.at(id)),Type::I64,"sext");
            require(cmp("sge",cursor,integer(Type::I64,0)));
            if (capped) {
                const auto cap = cast(load(Type::I32,locals.at(sentinel->second)),Type::I64,"sext");
                require(cmp("sle",cursor,cap));
                require(cmp(exclusive_end ? "ule" : "ult",cap,array.length));
            } else {
                const auto distance = instruction(Op::Mul,Type::I64,{count,integer(Type::I64,mutations.at(id))});
                const auto after = instruction(Op::Add,Type::I64,{cursor,distance});
                require(cmp(exclusive_end ? "ule" : "ult",after,array.length));
                require(cmp("ule",after,integer(Type::I64,INT32_MAX)));
            }
        }
        if (increments) affine_ops.insert(&key);
        affine_data.emplace(access,array.data);
    }
    stable_loop_limit = limit;
    stable_loop_value = cast(end,Type::I32,"trunc");
    ordinary_for(loop,stride);
    stable_loop_limit = nullptr;
    affine_data.clear();
    affine_ops.clear();
    jump(done);
    current = slow;
    // The checked copy retains every exceptional edge. Replicating it for
    // a small frozen table competes with the unrolled fast path for I-cache.
    ordinary_for(loop,0,table.has_value());
    jump(done);
    current = done;
    return true;
}
