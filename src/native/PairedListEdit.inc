// Insert followed by RemoveAt leaves the suffix beyond both indices in place.
// Shift only their intervening elements. Capacity growth, observed ranges and
// every possible checked failure execute the original statements in order.
std::size_t Builder::paired_list_edit(const Stmt& scope, std::size_t index) {
    using S = Stmt::Kind;
    if (!lower.options.optimize_loops || lower.module.instrument_budget ||
        index+1 >= scope.statements.size()) return 0;
    const auto call_at = [&](std::size_t pos,const char* name,std::size_t args) -> const Expr* {
        if (pos>=scope.statements.size()) return nullptr;
        const auto& st=*scope.statements[pos];
        const auto* e=st.expression.get();
        if (st.kind!=S::Expression || !e || e->kind!=K::Call || e->binding!=B::Method ||
            e->is_extension || e->args.size()!=args+1) return nullptr;
        const auto& target=*e->args[0];
        if (target.kind!=K::Member || target.arrow || target.text!=name || target.args.size()!=1) return nullptr;
        return e;
    };
    const auto atom=[](const Expr& e) {
        return e.kind==K::Literal || (e.kind==K::Name && e.binding==B::Local && !is_reference(e.declared_type));
    };
    const auto* insert=call_at(index,"Insert",2);
    if (!insert || generic_base(insert->owner)!="System.Collections.Generic.List") return 0;
    const auto& list=*insert->args[0]->args[0];
    const auto& pos=*insert->args[1];
    const auto& item=*insert->args[2];
    if (list.kind!=K::Name || list.binding!=B::Local || is_reference(list.declared_type) ||
        list.type!=insert->owner || pos.type!="int" || !atom(pos) || !atom(item)) return 0;
    const auto* cls=lower.program.find_class(list.type);
    if (!cls || cls->token.at.file!="<bolt-stdlib>/System/Collections/Generic/List.bolt") return 0;
    const auto element=generic_args(list.type).at(0);
    const auto layout=lower.layout(element);
    if (item.type!=element || !native_abi::scalar_storage_kind(layout.kind)) return 0;

    std::size_t last=index+1;
    const auto* remove=call_at(last,"RemoveAt",1);
    const Expr* observer=nullptr;
    const Expr* read=nullptr;
    if (!remove) {
        const auto& middle=*scope.statements[last];
        observer=middle.expression.get();
        if (middle.kind!=S::Expression || !observer || observer->kind!=K::Assign || observer->binding!=B::None ||
            observer->args.size()!=2 || (observer->text!="=" && observer->text!="+=" && observer->text!="-=")) return 0;
        const auto& dest=*observer->args[0];
        if (dest.kind!=K::Name || dest.binding!=B::Local || is_reference(dest.declared_type) ||
            !is_integer(dest.type) || !is_integer(element)) return 0;
        read=observer->args[1].get();
        if (read->kind==K::Call && read->binding==B::Cast && read->args.size()==2 && is_integer(read->type))
            read=read->args[1].get();
        if (!list_member(*read,list.local_id,"@get.@indexer",1,list.type)) return 0;
        if (read->args[1]->type!="int" || !atom(*read->args[1])) return 0;
        remove=call_at(++last,"RemoveAt",1);
    }
    if (!remove || remove->owner!=list.type || !local_name(*remove->args[0]->args[0],list.local_id)) return 0;
    const auto& erased=*remove->args[1];
    if (erased.type!="int" || !atom(erased) ||
        (observer && local_name(erased,observer->args[0]->local_id))) return 0;

    at=insert->token.at;
    const auto p=cast(expression(pos).value,Type::I64,"zext");
    const auto q=cast(expression(erased).value,Type::I64,"zext");
    const auto added=expression(item);
    const auto object=locals.at(list.local_id);
    const auto slot=lower.field(*this,list.type,"storage",object);
    const auto header=load(Type::Ptr,slot);
    const auto slow=block(),done=block();
    auto require=[&](Reg valid) {
        const auto next=block(); branch(valid,next,slow); current=next;
    };
    const auto zero=integer(Type::I64,0);
    const auto ranges=load(Type::I64,offset(context,offsetof(BrContext,active_ranges)),"execution context");
    require(instruction(Op::And,Type::I32,{cmp("eq",ranges,zero),cmp("ne",header,null())}));
    const auto count=load_buffer_size(offset(header,offsetof(BrSequenceHeader,count)));
    const auto capacity=load_buffer_size(offset(header,offsetof(BrSequenceHeader,capacity)));
    require(cmp("ult",count,capacity));
    require(cmp("ule",p,count));
    require(cmp("ule",q,count));
    const auto data=buffer_data(header);
    const auto width=integer(Type::I64,layout.size);
    auto address=[&](Reg i) {
        return instruction(Op::Offset,Type::Ptr,{data,instruction(Op::Mul,Type::I64,{i,width})});
    };
    if (observer) {
        const auto r=cast(expression(*read->args[1]).value,Type::I64,"zext");
        require(cmp("ule",r,count));
        const auto original=instruction(Op::Sub,Type::I64,{r,cast(cmp("ugt",r,p),Type::I64,"zext")});
        const auto element_ptr=instruction(Op::Select,Type::Ptr,{cmp("eq",r,p),memory(added),address(original)});
        const auto& dest=*observer->args[0];
        // Do not publish anything before the conversion and arithmetic succeed.
        // On failure the ordinary Insert runs first, so handlers see its effect.
        guard_fallback=slow;
        Value value=converted({element,load(layout.ir,element_ptr)},observer->args[1]->type);
        if (observer->text!="=") {
            auto old=from_memory(dest.type,locals.at(dest.local_id));
            value=arithmetic(observer->text=="+="?"+":"-",old,value);
        }
        value=converted(value,dest.type);
        guard_fallback.reset();
        store(locals.at(dest.local_id),value.value);
    }
    const auto different=block(),publish=block();
    branch(cmp("ne",p,q),different,publish);
    current=different;
    const auto right=cmp("ult",p,q);
    auto select=[&](Reg a,Reg b){return instruction(Op::Select,Type::I64,{right,a,b});};
    const auto first=select(p,q),end=select(q,p);
    const auto start=address(first),next=offset(start,layout.size);
    const auto dest=instruction(Op::Select,Type::Ptr,{right,next,start});
    const auto source=instruction(Op::Select,Type::Ptr,{right,start,next});
    const auto one=integer(Type::I64,1);
    const auto tail=instruction(Op::Sub,Type::I64,{instruction(Op::Sub,Type::I64,{end,first}),one});
    move_within_storage(dest,source,instruction(Op::Mul,Type::I64,{tail,width}),layout.size);
    store(address(select(p,instruction(Op::Sub,Type::I64,{p,one}))),added.value,"sequence element");
    jump(publish);
    current=publish;
    instruction(Op::Zero,Type::Void,{address(count)},layout.size);
    const auto revision=lower.field(*this,list.type,"revision",object);
    store(revision,instruction(Op::Add,Type::I64,{load(Type::I64,revision),integer(Type::I64,2)}));
    jump(done);
    current=slow;
    for (auto i=index;i<=last;++i) statement(*scope.statements[i]);
    jump(done); current=done;
    return last-index+1;
}
