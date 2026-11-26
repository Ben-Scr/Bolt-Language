// A numeric Queue reads a consecutive batch from outgoing and appends to incoming.
// Between refills and allocations these operations have no observable side effects
// beyond the cursor, incoming length and enumerator revision.
static bool queue_scalar(const Expr& e) {
    if (!is_numeric(e.type) && e.type!="bool") return false;
    if (e.kind==K::Literal) return true;
    if (e.kind==K::Name) return e.binding==B::Local && !is_reference(e.declared_type);
    if (e.kind==K::Call && e.binding==B::Cast && e.args.size()==2) return queue_scalar(*e.args[1]);
    if (e.binding!=B::None) return false;
    if (e.kind==K::Unary && e.args.size()==1 && (e.text=="-" || e.text=="+" || e.text=="~" || e.text=="!"))
        return queue_scalar(*e.args[0]);
    return e.kind==K::Binary && e.args.size()==2 && queue_scalar(*e.args[0]) && queue_scalar(*e.args[1]);
}

static const Expr* queue_receiver(const Expr& call, const char* name, std::size_t nargs) {
    if (call.kind!=K::Call || call.binding!=B::Method || call.is_extension || call.is_static ||
        call.args.size()!=nargs+1 || generic_base(call.owner)!="System.Collections.Generic.Queue") return nullptr;
    const auto& member=*call.args[0];
    if (member.kind!=K::Member || member.arrow || member.text!=name || member.args.size()!=1) return nullptr;
    const auto& obj=*member.args[0];
    return obj.kind==K::Name && obj.binding==B::Local && !is_reference(obj.declared_type) ? &obj : nullptr;
}

bool Builder::queue_work_loop(const Stmt& loop, const Expr& limit) {
    using S=Stmt::Kind;
    const auto& body=loop.body->statements;
    if (lower.core() || body.size()<2 || limit.type!="int" ||
        (limit.kind!=K::Literal && !(limit.kind==K::Name && queue_scalar(limit)))) return false;
    const auto& pop=*body.front();
    if (pop.kind!=S::Variable || !is_numeric(pop.type) || !pop.expression || pop.expression->type!=pop.type) return false;
    const auto* queue=queue_receiver(*pop.expression,"Dequeue",0);
    if (!queue) return false;
    const auto& queue_type=pop.expression->owner;
    const auto* cls=lower.program.find_class(queue_type);
    if (!cls || cls->token.at.file!="<bolt-stdlib>/System/Collections/Generic/Queue.bolt") return false;

    // Only ordinary local scalar calculations may observe the interval between
    // the two calls. No callbacks, references, early exits or changes to the bound.
    const Stmt* push=nullptr;
    for (std::size_t i=1;i<body.size();++i) {
        const auto& st=*body[i];
        const auto* e=st.expression.get();
        if (st.kind==S::Variable && is_numeric(st.type) && e && queue_scalar(*e)) continue;
        if (st.kind!=S::Expression || !e) return false;
        if (auto* target=queue_receiver(*e,"Enqueue",1)) {
            if (push || target->local_id!=queue->local_id || e->owner!=queue_type ||
                e->args[1]->type!=pop.type || !queue_scalar(*e->args[1])) return false;
            push=&st;
            continue;
        }
        if (e->kind!=K::Assign || e->binding!=B::None || e->args.size()!=2 || !queue_scalar(*e->args[1])) return false;
        const auto& dest=*e->args[0];
        if (dest.kind!=K::Name || !queue_scalar(dest) || dest.local_id==loop.init->local_id ||
            (limit.kind==K::Name && dest.local_id==limit.local_id)) return false;
    }
    if (!push) return false;

    const auto cleanup_start=cleanup.size();
    statement(*loop.init);
    const auto index_slot=locals.at(loop.init->local_id), object=locals.at(queue->local_id);
    const auto bound=expression(limit).value;
    const auto in_slot=lower.field(*this,queue_type,"incoming",object), out_slot=lower.field(*this,queue_type,"outgoing",object);
    const auto cursor_slot=lower.field(*this,queue_type,"consumed",object), epoch_slot=lower.field(*this,queue_type,"revision",object);
    const auto outer=block(), inspect=block(), space=block(), begin=block(), inner=block();
    const auto scalar=block(), commit=block(), popped=block(), pushed=block(), done=block();
    const auto previous_failure=failure;
    jump(outer);
    current=outer;
    const auto start=load(Type::I32,index_slot);
    branch(cmp("slt",start,bound),inspect,done);
    current=inspect;
    const auto in=load(Type::Ptr,in_slot), out=load(Type::Ptr,out_slot);
    const auto observed=load(Type::I64,offset(context,offsetof(BrContext,active_ranges)),"execution context");
    const auto both=instruction(Op::And,Type::I32,{cmp("ne",in,null()),cmp("ne",out,null())});
    branch(instruction(Op::And,Type::I32,{both,cmp("eq",observed,integer(Type::I64,0))}),space,scalar);

    current=scalar;
    statement(*loop.body);
    (void)expression(*loop.increment);
    jump(outer);

    current=space;
    const auto count_slot=offset(in,offsetof(BrSequenceHeader,count));
    const auto count=load_buffer_size(count_slot), capacity=load_buffer_size(offset(in,offsetof(BrSequenceHeader,capacity)));
    const auto cursor=cast(load(Type::I32,cursor_slot),Type::I64,"zext");
    const auto end=load_buffer_size(offset(out,offsetof(BrSequenceHeader,count)));
    branch(instruction(Op::And,Type::I32,{cmp("ult",cursor,end),cmp("ult",count,capacity)}),begin,scalar);

    current=begin;
    const auto minimum=[&](Reg a,Reg b){return instruction(Op::Select,Type::I64,{cmp("ult",a,b),a,b});};
    const auto start64=cast(start,Type::I64,"sext");
    const auto trips=instruction(Op::Sub,Type::I64,{cast(bound,Type::I64,"sext"),start64});
    const auto chunk=minimum(trips,minimum(instruction(Op::Sub,Type::I64,{end,cursor}),
                                           instruction(Op::Sub,Type::I64,{capacity,count})));
    const auto stop=cast(instruction(Op::Add,Type::I64,{start64,chunk}),Type::I32,"trunc");
    const auto epoch=load(Type::I64,epoch_slot), input=load_sequence_data(out), output=load_sequence_data(in);
    const auto& element=lower.layout(pop.type);
    const auto stride=integer(Type::I64,element.size), one=integer(Type::I64,1);
    auto element_at=[&](Reg data,Reg base,Reg delta) {
        auto index=instruction(Op::Add,Type::I64,{base,delta});
        return instruction(Op::Offset,Type::Ptr,{data,instruction(Op::Mul,Type::I64,{index,stride})});
    };
    jump(inner);
    current=inner;
    // Preserve the source induction range across the extra outer chunk loop.
    // The loop body cannot assign the counter or take its address.
    const auto* initial=loop.init->expression.get();
    const auto upper=limit.kind==K::Literal && initial && initial->kind==K::Literal &&
        initial->type=="int" && literal_value(*initial).as_int()>=0 ? literal_value(limit).as_int() : 0;
    const auto i=load(Type::I32,index_slot);
    if (upper>0) {
        // LLVM can use an unsigned minimum after promoting the index slot to
        // SSA. Every executed iteration is already in [0, upper), so this
        // exposes the bound without changing checked arithmetic in the body.
        const auto last=integer(Type::I32,static_cast<std::uint64_t>(upper-1));
        const auto body_index=alloc(4,false);
        store(body_index,instruction(Op::Select,Type::I32,{cmp("ult",i,last),i,last}));
        locals[loop.init->local_id]=body_index;
    }
    const auto completed=instruction(Op::Sub,Type::I64,{cast(i,Type::I64,"sext"),start64});
    const auto item=temp(pop.type);
    locals[pop.local_id]=item;
    store(item,load(element.ir,element_at(input,cursor,completed),"sequence element"));
    failure=popped;
    for (std::size_t j=1;j<body.size();++j) {
        const auto& st=*body[j];
        if (&st==push) {
            const auto value=expression(*st.expression->args[1]);
            store(element_at(output,count,completed),value.value,"sequence element");
            failure=pushed;
        } else statement(st);
    }
    locals[loop.init->local_id]=index_slot;
    failure=previous_failure;
    const auto next=instruction(Op::Add,Type::I32,{i,integer(Type::I32,1)});
    store(index_slot,next);
    branch(cmp("ne",next,stop),inner,commit);

    // Publish exactly the successful operations, including on a checked
    // arithmetic failure between the dequeue and the enqueue.
    auto publish=[&](Reg reads,Reg writes) {
        store(cursor_slot,cast(instruction(Op::Add,Type::I64,{cursor,reads}),Type::I32,"trunc"));
        store(count_slot,instruction(Op::Add,Type::I64,{count,writes}),"sequence header");
        store(epoch_slot,instruction(Op::Add,Type::I64,{epoch,instruction(Op::Add,Type::I64,{reads,writes})}));
    };
    current=commit; publish(chunk,chunk); jump(outer);
    current=popped; publish(instruction(Op::Add,Type::I64,{completed,one}),completed); jump(previous_failure);
    current=pushed;
    const auto finished=instruction(Op::Add,Type::I64,{completed,one});
    publish(finished,finished); jump(previous_failure);
    current=done; drop_since(cleanup_start);
    return true;
}
