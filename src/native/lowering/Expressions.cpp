#include "Lowering.hpp"
#include <bolt/Value.hpp>
#include <bit>

namespace bolt::ir::lowering {

// These tests return a canonical bool. Evaluating a second test eagerly
// cannot call user code, dereference an alias, or fail a checked conversion.
static bool simple_integer_test(const Expr& e) {
    if (e.kind!=K::Binary || e.binding!=B::None || e.args.size()!=2) return false;
    if (e.text!="==" && e.text!="!=" && e.text!="<" && e.text!="<=" && e.text!=">" && e.text!=">=") return false;
    const auto& left=*e.args[0];
    const auto& right=*e.args[1];
    const auto plain=[](const Expr& operand) {
        return operand.kind==K::Literal ||
               (operand.kind==K::Name && operand.binding==B::Local && !is_reference(operand.declared_type));
    };
    return is_integer(left.type) && left.type==right.type && plain(left) && plain(right);
}

Value Builder::expression(const Expr& e){
    if(lower.options.edit_data && e.kind==K::RuntimeOperand) {
        auto& slot=lower.options.edit_data->slots.at(&e);
        slot.emitted=true;
        return from_memory(e.type,address(slot.symbol));
    }
    if (e.kind == K::Call && !projected_calls.empty()) {
        if (auto it = projected_calls.find(&e); it != projected_calls.end())
            return projected_call(e,projected_lists.at(it->second));
    }
    if (e.kind == K::Index && !forwarded_utf8_reads.empty()) {
        if (auto it=forwarded_utf8_reads.find(&e); it!=forwarded_utf8_reads.end()) return {"byte",it->second};
    }
    if (&e == stable_loop_limit) return {"int",stable_loop_value};
    if (e.kind == K::Postfix && affine_ops.contains(&e)) {
        const auto slot = locals.at(e.args[0]->local_id);
        const auto old = load(Type::I32,slot);
        store(slot,instruction(Op::Add,Type::I32,{old,integer(Type::I32,1)},0,"nsw"));
        return {"int",old};
    }
    at=e.token.at;
    if ((e.kind == K::Index || e.kind == K::Member) && !e.args.empty() &&
        e.args[0]->kind == K::Name && e.args[0]->binding == B::Local) {
        if (auto found = indexed_arrays.find(e.args[0]->local_id); found != indexed_arrays.end()) {
            const auto& array = found->second;
            if (e.kind == K::Member)
                return {"int", e.text == "Rank" ? integer(Type::I32, 1) : array.length};
            auto index = expression(*e.args[1]).value;
            at = e.token.at;
            guard(cmp("uge", index, array.length), "index out of range");
            return indexed_element(array, index);
        }
    }
    if(e.kind==K::SizeOf)return {"int",integer(Type::I32,std::stoull(e.resolved))};
    if(e.kind==K::OptionalHas){auto p=expression(*e.args[0]).value;return {"bool",load(Type::I8,p)};}
    if(e.kind==K::OptionalValue)return from_memory(e.type,reference(e));
    if(e.kind==K::Let){
        // SnapshotReceiver on IStringable converts a copy. For a standalone
        // image, select the concrete copy size before calling its formatter.
        // In particular, a reference to a base class may point at a subclass.
        const auto* source=lower.program.find_class(referred(e.args[0]->type));
        if(lower.core() && e.text.starts_with("@conversion_receiver") && e.declared_type=="System.IStringable" &&
           source && !source->is_interface && !source->is_native && e.type=="string"){
            const auto* iface=lower.program.find_class(e.declared_type);
            const auto* conversion=find_method(*iface,"@convert.string");
            if(conversion && e.args[1]->resolved==conversion->id){
                const auto id=find_method(*iface,"ToString")->id;
                const auto value=expression(*e.args[0]);
                const auto output=temp("string"),done=block();
                auto format=[&](const Class& actual){
                    const auto* cls=&actual;
                    while(cls && !method_by_id(*cls,conversion->id))cls=lower.program.find_class(cls->base);
                    const auto* method=cls?method_by_id(*cls,conversion->id):nullptr;
                    if(!method || method->default_owner==iface->name){
                        cls=&actual;
                        while(cls && !method_by_id(*cls,id))cls=lower.program.find_class(cls->base);
                        method=cls?method_by_id(*cls,id):nullptr;
                    }
                    if(!method || method->is_abstract){
                        call("br_fail",Type::Void,{context,site(),string("missing IStringable formatter")});
                        jump(done);return;
                    }
                    const auto copy=temp(actual.name);
                    copy_value(actual.name,copy,value.value);
                    auto formatted=invoke(*cls,*method,copy,{},0,false);
                    assign("string",output,formatted);
                    jump(done);
                };
                if(!source->is_struct && !source->is_sealed){
                    const auto dynamic_type=load(Type::Ptr,value.value);
                    for(const auto& cls:lower.program.classes){
                        if(cls.name==source->name || cls.is_abstract || cls.is_interface || !emitted_class(cls) ||
                           !cls.generic_parameters.empty() || !derives_from(lower.program,cls.name,source->name))continue;
                        const auto match=block(),next=block();
                        branch(cmp("eq",dynamic_type,type(cls.name)),match,next);current=match;
                        format(cls);current=next;
                    }
                }
                format(*source);current=done;return {"string",output};
            }
        }
        if(is_reference(e.declared_type))
            locals[e.local_id]=borrow(e.args[0]->type,referred(e.declared_type),reference(*e.args[0]));
        else {auto slot=temp(e.declared_type);assign(e.declared_type,slot,expression(*e.args[0]));locals[e.local_id]=slot;}
        auto result=expression(*e.args[1]);locals.erase(e.local_id);return result;
    }
    if(e.kind==K::Conditional){
        auto condition=expression(*e.args[0]);auto result=temp(e.type);auto yes=block(),no=block(),done=block();branch(condition.value,yes,no);
        current=yes;assign(e.type,result,expression(*e.args[1]));jump(done);
        current=no;assign(e.type,result,expression(*e.args[2]));jump(done);current=done;return from_memory(e.type,result);
    }
    if(e.kind==K::Default){auto p=temp(e.type);initialize(e.type,p);return from_memory(e.type,p);}
    if(e.kind==K::Literal){if(e.type=="null")return {"null",null()};auto v=literal_value(e);if(e.type=="string"){auto s=v.as_string();auto p=temp("string");call("br_string",Type::Void,{context,site(),p,string(s),integer(Type::I64,s.size())});return {e.type,p};}auto l=lower.layout(e.type);std::uint64_t bits=0;if(e.type=="bool")bits=v.as_bool()?1:0;else if(e.type=="float")bits=std::bit_cast<std::uint32_t>(std::get<float>(v.data));else if(e.type=="double")bits=std::bit_cast<std::uint64_t>(std::get<double>(v.data));else bits=is_unsigned(e.type)?std::stoull(v.to_string()):static_cast<std::uint64_t>(v.as_int());return {e.type,integer(l.ir,bits)};}
    if(e.binding==B::EnumValue){const auto* en=lower.program.find_enum(e.type);for(const auto& m:en->members)if(m.name==e.text){auto bits=is_unsigned(en->underlying)?std::stoull(m.value):static_cast<std::uint64_t>(std::stoll(m.value));return {e.type,integer(lower.layout(e.type).ir,bits)};}throw std::runtime_error("unknown enum value during lowering");}
    if(e.binding==B::Field){
        if(const auto* c=lower.program.find_class(e.owner);c&&c->is_native&&!native_abi::library_pair(*c)){
            const auto* f=find_field(*c,e.text);if(!f)throw Error(e.token.at,"unknown native property","BLT4001");
            Reg object=f->is_static?0:(e.kind==K::Member?receiver(e):self);
            return library_call(native_abi::library_property_key(*c,*f),f->type,object,{});
        }
    }
    if(e.binding==B::Property){
        if (e.text == "Length" && e.args.size() == 1 && e.args[0]->binding == B::Local) {
            const auto found = private_arrays.find(e.args[0]->local_id);
            if (found != private_arrays.end() && found->second.length)
                return converted({"ulong",found->second.length},"int");
        }
        if(e.owner=="String"&&e.text=="Empty")return {"string",temp("string")};
        if(e.owner=="string" && e.text=="ByteLength") {
            const auto slot = receiver(e);
            return lower.options.optimize_loops ? byte_length(slot) : string_call("ByteLength","int",{{"string",slot}});
        }
        if(e.owner=="Arena"){auto p=receiver(e);at=e.token.at;auto property=e.text=="UsedBytes"?0:e.text=="CapacityBytes"?1:e.text=="PeakBytes"?2:3;return {"ulong",call("br_arena_stat",Type::I64,{context,site(),p,integer(Type::I32,property)})};}
        if(e.owner=="Math"){
            double value=e.text=="Pi"?3.141592653589793238462643383279502884:e.text=="E"?2.718281828459045235360287471352662498:6.283185307179586476925286766559005768;
            if(e.type=="float")return {e.type,integer(Type::F32,std::bit_cast<std::uint32_t>(static_cast<float>(value)))};
            return {e.type,integer(Type::F64,std::bit_cast<std::uint64_t>(value))};
        }
        if(is_numeric(e.owner)){
            auto value=numeric_limit_value(e.owner,e.text=="MaxValue");
            std::uint64_t bits=0;
            if(e.owner=="float")bits=std::bit_cast<std::uint32_t>(std::get<float>(value.data));
            else if(e.owner=="double")bits=std::bit_cast<std::uint64_t>(std::get<double>(value.data));
            else bits=is_unsigned(e.owner)?std::get<std::uint64_t>(value.data):static_cast<std::uint64_t>(value.as_int());
            return {e.type,integer(lower.layout(e.type).ir,bits)};
        }
        auto p=receiver(e);at=e.token.at;
        if(is_array(e.owner)&&e.text=="Rank")return {"int",integer(Type::I32,array_rank(e.owner))};
        if(is_array(e.owner)||collection_kind(e.owner)=="List") {
            const auto member=e.text=="Capacity"?offsetof(BrSequenceHeader,capacity):offsetof(BrSequenceHeader,count);
            return {"int",sequence_size(p,member,collection_kind(e.owner) == "List")};
        }
        return {"int",call(e.text=="Capacity"?"br_capacity":"br_count",Type::I32,{context,site(),type(e.owner),p})};
    }
    if(e.kind==K::Name||e.kind==K::Member)return from_memory(e.type,reference(e));
    if(e.kind==K::Call){auto v=call_expr(e);if(is_reference(e.declared_type))return from_memory(e.type,v.value);return v;}
    if(e.kind==K::Index){
        if (e.owner == "string") {
            auto value = expression(*e.args[0]);
            if (!cannot_change_string(*e.args[1])) value = snapshot(value);
            auto index = converted(expression(*e.args[1]), "int");
            at = e.token.at;
            auto ordinary = [&] {
                // Shared modules keep the stable bridge symbol, so an older
                // DSO can reach the ABI guard before any new call.
                if (lower.core() || !lower.options.standalone_executable)
                    return string_call("At", "char", {value, index});
                return Value{"char", call("br_string_at", Type::I32, {context, site(), value.value, index.value})};
            };
            const auto& input = *e.args[0];
            auto it = text_views.find(input.local_id);
            if (input.kind != K::Name || input.binding != B::Local || it == text_views.end()) return ordinary();
            const auto view = it->second;
            const auto i = cast(index.value, Type::I64, "sext");
            const auto fast = block(), slow = block(), done = block();
            const auto result = temp("char", false);
            // A missing view has length zero. An invalid index takes the same
            // path as ordinary indexing and preserves its exact exception.
            branch(cmp("ult", i, view.count), fast, slow);
            current = fast;
            const auto bytes = instruction(Op::Mul, Type::I64, {i, integer(Type::I64, sizeof(char32_t))});
            const auto addr = instruction(Op::Offset, Type::Ptr, {view.data, bytes});
            store(result, load(Type::I32, addr)); jump(done);
            current = slow; store(result, ordinary().value); jump(done);
            current = done; return {"char", load(Type::I32, result)};
        }
        if (auto* fact = private_array(e); fact && fact->maximum) {
            const auto pointer = reference(e);
            const auto limit = std::uint64_t(*fact->maximum) + 1;
            return {e.type, instruction(Op::Load, Type::I32, {pointer}, limit)};
        }
        return from_memory(e.type,reference(e));
    }
    if(e.kind==K::Pack){for(const auto& a:e.args)(void)expression(*a);return {"void",0};}
    if(e.kind==K::Array){auto p=temp(e.type);
        if(array_rank(e.type)>1){auto dimensions=alloc(e.dimensions.size()*8,false);for(std::size_t d=0;d<e.dimensions.size();++d)store(offset(dimensions,d*8),integer(Type::I64,e.dimensions[d]));call("br_array_shape",Type::Void,{context,site(),type(e.type),p,dimensions,integer(Type::I64,e.dimensions.size()),integer(Type::I32,0)});}
        else call("br_array",Type::Void,{context,site(),type(e.type),p,integer(Type::I64,e.args.size()),integer(Type::I32,0)});
        auto t=element_type(e.type);for(std::size_t n=0;n<e.args.size();++n){auto v=converted(expression(*e.args[n]),t);auto dest=call("br_index",Type::Ptr,{context,site(),type(e.type),p,integer(Type::I64,n)});assign(t,dest,v);}return {e.type,p};}
    if(e.kind==K::New){if(e.arrow){if(array_rank(e.type)<=1)return new_vector(e);auto p=temp(e.type);
        if(array_rank(e.type)>1){auto dimensions=alloc(e.args.size()*8,false);for(std::size_t d=0;d<e.args.size();++d){auto n=expression(*e.args[d]);at=e.token.at;if(!is_unsigned(n.type))guard(cmp("slt",n.value,integer(lower.layout(n.type).ir,0)),"array dimensions must be non-negative");n=converted(std::move(n),"ulong");store(offset(dimensions,d*8),n.value);}at=e.token.at;call("br_array_shape",Type::Void,{context,site(),type(e.type),p,dimensions,integer(Type::I64,e.args.size()),integer(Type::I32,1)});}
        initialize_array_elements(e.type,p);return {e.type,p};}
        auto arena=e.arena?reference(*e.arena):Reg(0);
        auto t=element_type(e.type);auto* c=lower.program.find_class(t);
        auto allocate=[&](){at=e.token.at;return arena?call("br_arena_new",Type::Ptr,{context,site(),arena,type(t)}):call("br_new",Type::Ptr,{context,site(),type(t)});};
        auto cleanup_failed=[&](Reg p){
            if(arena && lower.core())call("br_arena_failed",Type::Void,{context,arena,type(t),p},false);
            else call("br_new_failed",Type::Void,{context,type(t),p},false);
        };
        if(c){
            const auto* constructor=e.resolved.empty()?nullptr:method_by_id(*c,e.resolved);
            std::vector<Reg> prepared;
            if(arena&&constructor)for(std::size_t n=0;n<constructor->parameters.size();++n){const auto& parameter=constructor->parameters[n];const auto& argument=*e.args[n];if(is_reference(parameter.type))prepared.push_back(reference(argument));else prepared.push_back(snapshot(converted(expression(argument),parameter.type)).value);}
            auto p=allocate();const auto outer=failure,bad=block(),done=block();failure=bad;
            if(!constructor)initialize(t,p);else invoke(*c,*constructor,p,e.args,0,false,arena?&prepared:nullptr);
            failure=outer;jump(done);current=bad;cleanup_failed(p);jump(outer);current=done;return {e.type,p};
        }
        Value v;if(!e.args.empty())v=converted(expression(*e.args[0]),t);auto p=allocate();
        if(lower.core() && lower.layout(t).aggregate && !e.args.empty()){
            const auto outer=failure,bad=block(),done=block();failure=bad;
            assign(t,p,v);failure=outer;jump(done);
            current=bad;cleanup_failed(p);jump(outer);current=done;
        }else if(!e.args.empty())assign(t,p,v);else initialize(t,p);
        return {e.type,p};}
    if(e.kind==K::Delete){auto v=expression(*e.args[0]);at=e.token.at;call("br_delete",Type::Void,{context,site(),v.value});return {"void",0};}
    if(e.binding==B::Operator){
        auto ptr=reference(*e.args[0]);auto old=snapshot(from_memory(e.type,ptr));std::vector<Value> args{old};
        if(e.args.size()==2)args.push_back(snapshot(expression(*e.args[1])));
        at=e.token.at;const auto& c=*lower.program.find_class(e.owner);auto result=invoke_operator(c,*method_by_id(c,e.resolved),std::move(args));
        assign(e.type,ptr,result);return e.kind==K::Postfix?old:result;
    }
    if(e.kind==K::Assign){const auto& a=*e.args[0];const auto& b=*e.args[1];
        if(e.text=="?" "?="){auto p=reference(a);auto old=from_memory(a.type,p);auto missing=block(),done=block();branch(is_optional(a.type)?cmp("eq",load(Type::I8,old.value),integer(Type::I8,0)):cmp("eq",old.value,null()),missing,done);current=missing;assign(e.type,p,expression(b));jump(done);current=done;return from_memory(e.type,p);}
        if(e.text=="="&&a.kind==K::Index&&collection_kind(a.owner)=="Dictionary"){auto obj=expression(*a.args[0]);auto key=snapshot(converted(expression(*a.args[1]),generic_args(a.owner)[0]));auto rhs=snapshot(converted(expression(b),e.type));at=e.token.at;call("br_collection",Type::Void,{context,site(),type(a.owner),obj.value,string("Set"),null(),memory(key),memory(rhs)});return rhs;}
        auto ptr=reference(a);
        if (memory_region && memory_region->write == &e) memory_region->written_address = ptr;
        const bool append_string = e.text == "+=" && a.type == "string";
        Value old;
        if (e.text != "=") {
            old = from_memory(a.type,ptr);
            // A formatter on a non-string RHS can run user code even when the
            // argument itself is a simple name.
            if (!append_string || b.type != "string" || !cannot_change_string(b))
                old = snapshot(old);
        }
        auto rhs=expression(b);at=e.token.at;
        if (bounded_additions.contains(&e)) {
            const auto gain = converted(std::move(rhs), "ulong");
            rhs = {"ulong", instruction(Op::Add, Type::I64, {old.value, gain.value})};
        } else if(e.text!="="){const auto op=e.text.substr(0,e.text.size()-1);rhs=(op=="&"||op=="|"||op=="^"||op=="<<"||op==">>")?bitwise(op,old,rhs,e.type):arithmetic(op,old,rhs);}
        if (append_string && a.binding == B::Local && !is_reference(a.declared_type))
            take_handle(ptr,rhs);
        else assign(e.type,ptr,rhs);
        auto result=from_memory(e.type,ptr);
        if (memory_region && memory_region->write==&e) memory_region->written_byte=result.value;
        return result;}
    if(e.kind==K::Unary||e.kind==K::Postfix){const auto& a=*e.args[0];if(e.text=="&")return {e.type,reference(a)};if(e.text=="*")return from_memory(e.type,reference(e));
        if(e.text=="++"||e.text=="--"){
            auto ptr=reference(a);auto old=from_memory(a.type,ptr);
            auto one=converted({"int",integer(Type::I32,1)},promoted(a.type));
            const bool increment=e.text=="++";
            auto updated=bounded_additions.contains(&e)
                ? Value{"int",instruction(increment?Op::Add:Op::Sub,Type::I32,{old.value,one.value},0,"nsw")}
                : arithmetic(increment?"+":"-",old,one);
            at=e.token.at;updated=converted(updated,e.type);assign(e.type,ptr,updated);return e.kind==K::Postfix?old:updated;
        }
        auto v=expression(a);at=e.token.at;if(e.text=="!")return {"bool",boolean(cmp("eq",v.value,integer(fn.registers[v.value],0)))};v=converted(std::move(v),e.type);if(e.text=="+")return v;if(e.text=="~")return {e.type,instruction(Op::Xor,lower.layout(e.type).ir,{v.value,integer(lower.layout(e.type).ir,~std::uint64_t{0})})};
        if(is_floating(e.type))return {e.type,instruction(Op::FNeg,lower.layout(e.type).ir,{v.value})};
        return arithmetic("-",{e.type,integer(lower.layout(e.type).ir,0)},v);
    }
    if(e.kind==K::Binary){const auto& a=*e.args[0];const auto& b=*e.args[1];
        if(e.text=="??"&&is_optional(a.type)) {
            auto left=expression(a);auto result=temp(e.type);auto present=block(),missing=block(),done=block();
            branch(load(Type::I8,left.value),present,missing);current=present;
            assign(e.type,result,is_optional(e.type)?left:from_memory(element_type(a.type),offset(left.value,8)));jump(done);
            current=missing;assign(e.type,result,expression(b));jump(done);current=done;return from_memory(e.type,result);
        }
        if(e.text=="??"){
            auto left=expression(a);auto result=temp(e.type);assign(e.type,result,left);auto right=block(),done=block();branch(cmp("eq",left.value,null()),right,done);
            current=right;assign(e.type,result,expression(b));jump(done);current=done;return from_memory(e.type,result);
        }
        if(e.text=="&"||e.text=="|"||e.text=="^"||e.text=="<<"||e.text==">>"){auto left=snapshot(expression(a));auto right=expression(b);return bitwise(e.text,left,right,e.type);}
        if(e.text=="+"&&(a.type=="string"||b.type=="string")){
            auto text=[&](const Expr& expr){
                auto v=expression(expr);
                if (v.type == "string") return v;
                auto p=temp("string");
                call("br_text",Type::Void,{context,site(),p,type(v.type),memory(v)});
                return Value{"string",p};
            };
            auto left=text(a);
            if (a.type == "string" && !fresh_handle(a) && (b.type != "string" || !cannot_change_string(b)))
                left=snapshot(left);
            auto right=text(b);
            return arithmetic("+",std::move(left),std::move(right));
        }
        if(e.text==","){(void)expression(a);return expression(b);}
        if(e.text=="&&"||e.text=="||"){
            auto left=expression(a);
            if(lower.options.optimize_loops && simple_integer_test(a) && simple_integer_test(b)) {
                auto right=expression(b);
                return {"bool",instruction(e.text=="&&"?Op::And:Op::Or,Type::I8,{left.value,right.value})};
            }
            auto p=temp("bool",false);store(p,left.value);auto right=block(),done=block();
            if(e.text=="&&")branch(left.value,right,done);else branch(left.value,done,right);
            current=right;auto v=expression(b);store(p,v.value);jump(done);current=done;return from_memory("bool",p);
        }
        auto left = expression(a);
        if (left.type != "string" || !cannot_change_string(b)) left = snapshot(left);
        auto right = expression(b);
        if (right.type != "string") right = snapshot(right);
        at = e.token.at;
        bool compare=e.text=="=="||e.text=="!="||e.text=="<"||e.text=="<="||e.text==">"||e.text==">=";
        if(lower.core() && compare && is_pointer(left.type) && is_pointer(right.type) &&
           left.type!=right.type && is_interface(lower.program,element_type(left.type)))
            right=converted(std::move(right),left.type);
        if(compare&&((is_optional(left.type)&&right.type=="null")||(is_optional(right.type)&&left.type=="null"))) {
            const auto optional=is_optional(left.type)?left:right;
            return {"bool",boolean(cmp(e.text=="=="?"eq":"ne",load(Type::I8,optional.value),integer(Type::I8,0)))};
        }
        if (lower.options.optimize_loops && left.type == "string" && right.type == "string" &&
            (e.text == "==" || e.text == "!=")) {
            const auto same = string_equal(left.value,right.value);
            return {"bool", e.text == "==" ? same : instruction(Op::Xor,Type::I8,{same,integer(Type::I8,1)})};
        }
        if(compare){auto common=common_numeric(left.type,right.type);if(!common.empty()){left=converted(std::move(left),common);right=converted(std::move(right),common);}else if(is_pointer(left.type)&&right.type=="null")right=converted(std::move(right),left.type);else if(left.type=="null"&&is_pointer(right.type))left=converted(std::move(left),right.type);
            auto layout=lower.layout(left.type);Reg result=0;
            if(layout.aggregate){int op=e.text=="=="?0:e.text=="!="?1:e.text=="<"?2:e.text=="<="?3:e.text==">"?4:5;result=call("br_compare",Type::I32,{context,site(),type(left.type),left.value,right.value,integer(Type::I32,static_cast<std::uint64_t>(op))});}
            else {std::string pred=e.text=="=="?"eq":e.text=="!="?"ne":e.text=="<"?"lt":e.text=="<="?"le":e.text==">"?"gt":"ge";if(layout.kind==native_abi::Float)pred=e.text=="!="?"une":"o"+pred;else if(e.text!="=="&&e.text!="!=")pred=(layout.kind==native_abi::Signed?"s":"u")+pred;result=cmp(pred,left.value,right.value);}return {"bool",boolean(result)};
        }
        if (affine_ops.contains(&e))
            return {"int",instruction(e.text=="+"?Op::Add:e.text=="-"?Op::Sub:Op::Mul,
                                      Type::I32,{left.value,right.value})};
        return arithmetic(e.text,left,right,finite_operations.contains(&e));
    }
    throw Error(e.token.at,"native backend does not recognize this analyzed expression","BLT4001");
}

} // namespace bolt::ir::lowering
