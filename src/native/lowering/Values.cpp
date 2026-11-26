#include "Lowering.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace bolt::ir::lowering {
void Builder::take_handle(Reg destination, Value source) {
    if(lower.core()) {
        lower.core_call(*this,"Take",Type::Void,{context,type(source.type),destination,source.value},true);
        return;
    }
#if defined(_M_X64)
    if (lower.options.standalone_executable) {
        call("br_static_take_value",Type::Void,{context,site(),type(source.type),destination,source.value});
        return;
    }
#endif
    call("br_collection",Type::Void,{context,site(),type(source.type),destination,
         string("TakeValue"),null(),source.value,null()});
}

bool pure_scalar_value(const Expr& e) {
    if (!is_numeric(e.type) && e.type != "bool") return false;
    if (e.kind == K::Literal) return true;
    if (e.kind == K::Name) return e.binding == B::Local && !is_reference(e.declared_type);
    if (e.kind != K::Binary || e.binding != B::None || e.args.size() != 2) return false;
    // User operators, calls, alias reads, assignments and increments cannot
    // observe the temporary deferral of the public count.
    return pure_scalar_value(*e.args[0]) && pure_scalar_value(*e.args[1]);
}

// Built-in field/sequence reads may throw, but cannot reassign an earlier
// string operand. User accessors, overloaded indexing, callbacks and static
// initializers still require the regular owning snapshot.
bool cannot_change_string(const Expr& e) {
    if (e.kind == K::Literal || (e.kind == K::Name && e.binding == B::Local) || pure_scalar_value(e)) return true;
    bool read = (e.kind == K::Name || e.kind == K::Member) && e.binding == B::Field && !e.is_static;
    if (e.kind == K::Index && e.binding == B::None)
        read = is_array(e.owner) || collection_kind(e.owner) == "List" || e.owner == "string";
    return read && std::all_of(e.args.begin(), e.args.end(), [](const auto& arg) { return cannot_change_string(*arg); });
}

Value Builder::from_memory(const std::string& t,Reg address){auto l=lower.layout(t);return {t,l.aggregate?address:load(l.ir,address)};}
Reg Builder::memory(Value v) {
    const auto& layout = lower.layout(v.type);
    if (layout.aggregate) return v.value;
    // The store defines the whole scalar before its address escapes. Keep the
    // temporary in the slow branch when the fast path does not use it.
    const auto slot = alloc(layout.size,false);
    store(slot,v.value);
    return slot;
}
void Builder::copy_value(const std::string& t,Reg destination,Reg source) {
    const auto& layout = lower.layout(t);
    const bool fieldwise = !layout.class_headers.empty() || layout.padded_field_bytes || layout.inline_optional;
    if (!layout.plain_struct && !fieldwise) {
        call("br_copy",Type::Void,{context,site(),type(t),destination,source});
        return;
    }
    if (layout.size > 64 && !layout.inline_optional) {
        const auto whole_element = [&](Reg address) {
            const auto it=element_tags.find(address);
            return it!=element_tags.end() && it->second.type==t;
        };
        if (layout.size <= 128 && layout.plain_struct && whole_element(destination) && whole_element(source)) {
            instruction(Op::CopyElement,Type::Void,{destination,source},layout.size);
            return;
        }
        // Arbitrary references can partially overlap.
        call("br_memory_move", Type::Void,
             {context, site(), destination, source, integer(Type::I64, layout.size)});
        return;
    }
    // Runtime self-assignment leaves even explicitly written padding intact.
    Label done = 0;
    if (fieldwise) {
        if (destination == source) return;
        done = block();
        const auto distinct = block();
        branch(cmp("eq",destination,source),done,distinct);
        current = distinct;
    }
    if (layout.inline_optional) {
        const auto present = block(), absent = block();
        branch(cmp("ne",load(Type::I8,source),integer(Type::I8,0)),present,absent);
        current = present;
        const auto& payload = lower.layout(element_type(t));
        // Snapshot the payload before changing the tag: references can partially overlap.
        copy_small_value(payload,offset(destination,8),offset(source,8));
        store(destination,integer(Type::I64,1));
        const auto end = 8 + payload.size;
        if (end < layout.size) instruction(Op::Zero,Type::Void,{offset(destination,end)},layout.size-end);
        jump(done);
        current = absent;
        instruction(Op::Zero,Type::Void,{destination},layout.size);
    } else {
        copy_small_value(layout,destination,source);
    }
    if (fieldwise) {
        jump(done);
        current = done;
    }
}
void Builder::copy_small_value(const Layout& layout,Reg destination,Reg source) {
    const bool small_class = !layout.class_headers.empty();
    const auto field_bytes = small_class ? layout.class_field_bytes : layout.padded_field_bytes;
    const bool fieldwise = small_class || layout.padded_field_bytes;
    // Load the whole source before writing. References to overlapping storage
    // must observe the same snapshot as a runtime value copy.
    std::vector<std::pair<std::size_t,Reg>> parts;
    for (std::size_t pos=0; pos<layout.size;) {
        const auto remaining = fieldwise
            ? static_cast<std::size_t>(std::countr_one(field_bytes >> pos))
            : layout.size-pos;
        if (!remaining) {
            ++pos;
            continue;
        }
        const Type word = remaining>=8 ? Type::I64 : remaining>=4 ? Type::I32 : remaining>=2 ? Type::I16 : Type::I8;
        parts.emplace_back(pos,load(word,offset(source,pos)));
        pos += width(word);
    }
    if (fieldwise && std::popcount(field_bytes) + layout.class_headers.size() * lower.target_layouts.get("null").size < layout.size)
        instruction(Op::Zero,Type::Void,{destination},layout.size);
    for (auto [pos,value] : parts) store(offset(destination,pos),value);
    for (const auto& [pos,name] : layout.class_headers) store(offset(destination,pos),type(name));
}
void Builder::clear_value(const std::string& t,Reg address,const char* operation) {
    const auto& layout = lower.layout(t);
    if (layout.plain_struct || layout.padded_field_bytes || !layout.class_headers.empty() || layout.inline_optional) {
        instruction(Op::Zero,Type::Void,{address},layout.size);
        return;
    }
    // These values have one nullable owning handle. Error-path temporaries
    // often stay empty throughout a successful call. Expose that fact to LLVM.
    const bool handle = t == "string" || is_array(t) || is_collection(t);
    if (lower.options.optimize_loops && handle) {
        const auto release = block(), done = block();
        branch(cmp("ne", load(Type::Ptr,address), null()),release,done);
        current = release;
        call(operation,Type::Void,{type(t),address},false);
        jump(done); current = done;
        // Both br_reset and br_drop leave the handle empty on return.
        store(address,null());
    } else call(operation,Type::Void,{type(t),address},false);
}
Reg Builder::text_header(Reg object) {
    return instruction(Op::Select, Type::Ptr, {cmp("ne", object, null()), object, address(lower.text_literal(""))});
}

Value Builder::byte_length(Reg slot) {
    const auto header = text_header(load(Type::Ptr, slot));
    const auto n = load(Type::I64, offset(header, offsetof(BrTextHeader, size)));
    const auto fast = block(), slow = block(), done = block();
    const auto result = temp("int", false);
    branch(cmp("ule", n, integer(Type::I64, std::numeric_limits<std::int32_t>::max())), fast, slow);
    current = fast; store(result, cast(n, Type::I32, "trunc")); jump(done);
    current = slow; store(result, string_call("ByteLength", "int", {{"string",slot}}).value); jump(done);
    current = done; return {"int", load(Type::I32, result)};
}

// Compare equal-length short strings in place. Overlapping first/last words
// cover every byte; all loads stay strictly inside the UTF-8 payload.
// For up to 16 literal bytes the first and last machine words cover the
// complete pattern. Both loads stay inside the checked string range.
Reg Builder::literal_affix(Reg slot,const std::string& pattern,bool suffix) {
    const auto length = pattern.size();
    if (!length) return integer(Type::I8,1);
    const auto header = text_header(load(Type::Ptr,slot));
    const auto size = load(Type::I64,offset(header,offsetof(BrTextHeader,size)));
    const auto required = integer(Type::I64,length);
    const auto result = alloc(1,false);
    store(result,integer(Type::I8,0));
    const auto compare = block(), done = block();
    branch(cmp("uge",size,required),compare,done);
    current = compare;
    auto data = load(Type::Ptr,offset(header,offsetof(BrTextHeader,data)));
    if (suffix) data = instruction(Op::Offset,Type::Ptr,{data,instruction(Op::Sub,Type::I64,{size,required})});
    const auto bytes = string(pattern);
    const auto word = length >= 8 ? Type::I64 : length >= 4 ? Type::I32 : length >= 2 ? Type::I16 : Type::I8;
    const auto matches = [&](std::size_t pos) {
        return cmp("eq",load(word,offset(data,pos)),load(word,offset(bytes,pos)));
    };
    auto equal = matches(0);
    if (length > width(word)) equal = instruction(Op::And,Type::I32,{equal,matches(length-width(word))});
    store(result,boolean(equal));
    jump(done);
    current = done;
    return load(Type::I8,result);
}

Reg Builder::string_equal(Reg lhs, Reg rhs) {
    const auto a = load(Type::Ptr, lhs), b = load(Type::Ptr, rhs);
    const auto distinct = block(), one_null = block();
    const auto sizes = block(), same_size = block(), classify = block(), words = block();
    const auto pair = block(), four = block(), ordinary = block(), yes = block(), no = block(), done = block();
    const auto result = temp("bool", false);
    branch(cmp("eq", a, b), yes, distinct);
    current = distinct;
    const auto a_null = cmp("eq", a, null());
    branch(instruction(Op::Or,Type::I32,{a_null,cmp("eq",b,null())}), one_null, sizes);
    current = one_null;
    const auto other = instruction(Op::Select,Type::Ptr,{a_null,b,a});
    branch(cmp("eq",load(Type::I64,offset(other,offsetof(BrTextHeader,size))),integer(Type::I64,0)),yes,no);
    current = sizes;
    const auto n = load(Type::I64, offset(a, offsetof(BrTextHeader, size)));
    const auto m = load(Type::I64, offset(b, offsetof(BrTextHeader, size)));
    branch(cmp("eq", n, m), same_size, no);
    current = same_size; branch(cmp("eq", n, integer(Type::I64, 0)), yes, classify);
    current = classify;
    const auto size_minus_8 = instruction(Op::Sub, Type::I64, {n, integer(Type::I64,8)});
    const auto other_width = block(), long_words = block();
    branch(cmp("ule", size_minus_8, integer(Type::I64,24)), words, other_width);
    current = words;
    const auto first_a = load(Type::Ptr, offset(a, offsetof(BrTextHeader, data)));
    const auto first_b = load(Type::Ptr, offset(b, offsetof(BrTextHeader, data)));
    const auto last_a = instruction(Op::Offset, Type::Ptr, {first_a, size_minus_8});
    const auto last_b = instruction(Op::Offset, Type::Ptr, {first_b, size_minus_8});
    auto word_diff = [&](Reg x, Reg y) {
        return instruction(Op::Xor, Type::I64, {load(Type::I64,x), load(Type::I64,y)});
    };
    const auto outer = instruction(Op::Or, Type::I64, {word_diff(first_a,first_b),word_diff(last_a,last_b)});
    branch(cmp("ule", n, integer(Type::I64,16)), pair, four);
    current = pair; store(result,boolean(cmp("eq",outer,integer(Type::I64,0)))); jump(done);
    current = four;
    const auto size_minus_16 = instruction(Op::Sub, Type::I64, {n, integer(Type::I64,16)});
    const auto inner_a = instruction(Op::Offset, Type::Ptr, {first_a,size_minus_16});
    const auto inner_b = instruction(Op::Offset, Type::Ptr, {first_b,size_minus_16});
    const auto inner = instruction(Op::Or, Type::I64, {word_diff(offset(first_a,8),offset(first_b,8)),word_diff(inner_a,inner_b)});
    const auto all = instruction(Op::Or, Type::I64, {outer,inner});
    store(result,boolean(cmp("eq",all,integer(Type::I64,0)))); jump(done);
    current = other_width;
    branch(cmp("ugt",n,integer(Type::I64,32)),long_words,ordinary);
    current = long_words;
    // Long strings with different ends need no call or scan of the middle.
    const auto x = load(Type::Ptr, offset(a, offsetof(BrTextHeader,data)));
    const auto y = load(Type::Ptr, offset(b, offsetof(BrTextHeader,data)));
    const auto tail_x = instruction(Op::Offset,Type::Ptr,{x,size_minus_8});
    const auto tail_y = instruction(Op::Offset,Type::Ptr,{y,size_minus_8});
    const auto ends = instruction(Op::Or,Type::I64,{word_diff(x,y),word_diff(tail_x,tail_y)});
    branch(cmp("eq",ends,integer(Type::I64,0)),ordinary,no);
    current = ordinary;
    const auto equal = call("br_compare",Type::I32,{context,site(),type("string"),lhs,rhs,integer(Type::I32,0)});
    store(result,boolean(equal)); jump(done);
    current = yes; store(result,integer(Type::I8,1)); jump(done);
    current = no; store(result,integer(Type::I8,0)); jump(done);
    current = done; return load(Type::I8,result);
}

bool Builder::fresh_handle(const Expr& e) {
    const bool array = is_array(e.type);
    if (array) {
        const auto element = element_type(e.type);
        const auto& entry = lower.layout(element);
        if ((!entry.aggregate || entry.plain_struct) &&
            (e.kind == K::Array || (e.kind == K::New && e.arrow) ||
             (e.kind == K::Call && e.binding == B::Collection && e.text == "ToArray"))) return true;
        if (element != "string" && lower.layout(element).aggregate) return false;
    } else if (e.type != "string") return false;
    if (!lower.options.optimize_loops || is_reference(e.declared_type)) return false;
    // A method returning an array/string by value writes into a caller-owned
    // result slot. Transfer that handle instead of copying the payload again.
    if (e.kind == K::Call && e.binding == B::Method && !e.lvalue) return true;
    // Each string intrinsic returns fresh owned storage. The compiler knows
    // exactly where it was allocated and no user callback can observe it.
    if (e.kind == K::Call && e.binding == B::Builtin && e.owner == "String") return true;
    return !array && (e.kind == K::Literal ||
        (e.kind == K::Binary && e.binding == B::None && e.text == "+") ||
        (e.kind == K::Call && e.binding == B::Cast));
}

void Builder::initialize_array_elements(const std::string& array,Reg slot) {
    if(!lower.core())return;
    const auto element=element_type(array);
    const auto& layout=lower.layout(element);
    if(layout.zero_default || (layout.kind!=native_abi::Class && layout.kind!=native_abi::Struct &&
       layout.kind!=native_abi::LibraryValue))return;
    const auto header=load(Type::Ptr,slot);
    const auto data=load_sequence_data(header), count=load(Type::I64,offset(header,offsetof(BrSequenceHeader,count)));
    const auto cursor=alloc(8,false);
    const auto condition=block(),body=block(),done=block();
    store(cursor,integer(Type::I64,0));jump(condition);current=condition;
    const auto index=load(Type::I64,cursor);
    branch(cmp("ult",index,count),body,done);current=body;
    const auto bytes=instruction(Op::Mul,Type::I64,{index,integer(Type::I64,layout.size)});
    const auto address=instruction(Op::Offset,Type::Ptr,{data,bytes});
    const auto outer=failure,bad=block();failure=bad;
    call(lower.init_name(element),Type::Void,{context,address},true,false);
    failure=outer;const auto success=current;
    current=bad;
    // Array cleanup owns only the completed prefix. The failed element can
    // still contain initialized fields and completed base subobjects.
    store(offset(header,offsetof(BrSequenceHeader,count)),index);
    lower.core_call(*this,"DropIncomplete",Type::Void,{type(element),address},false);
    jump(outer);current=success;
    store(cursor,instruction(Op::Add,Type::I64,{index,integer(Type::I64,1)}));jump(condition);current=done;
}

Value Builder::new_vector(const Expr& e, Reg* length) {
    const auto slot = temp(e.type);
    auto size = expression(*e.args[0]);
    at = e.token.at;
    if (!is_unsigned(size.type))
        guard(cmp("slt",size.value,integer(lower.layout(size.type).ir,0)),"array dimensions must be non-negative");
    size = converted(std::move(size),"ulong");
    at = e.token.at;
    call("br_array",Type::Void,{context,site(),type(e.type),slot,size.value,integer(Type::I32,1)});
    initialize_array_elements(e.type,slot);
    if (length) *length = size.value;
    return {e.type,slot};
}
Value Builder::snapshot(Value v){if(!lower.layout(v.type).aggregate)return v;auto p=temp(v.type);copy_value(v.type,p,v.value);return {v.type,p};}

Reg Builder::check_finite(Reg value) {
    const bool single = fn.registers[value] == Type::F32;
    const auto word = single ? Type::I32 : Type::I64;
    const auto exponent = integer(word, single ? 0x7f800000ull : 0x7ff0000000000000ull);
    const auto bits = cast(value, word, "bitcast");
    guard(cmp("eq", instruction(Op::And, word, {bits, exponent}), exponent), "floating-point overflow");
    return value;
}

Reg Builder::checked_float_to_integer(Reg value, const Layout& src, const Layout& dst) {
    const bool signed_target = dst.kind == native_abi::Signed;
    const int digits = static_cast<int>(dst.size * 8) - signed_target;
    const auto bound = std::ldexp(1.0, digits);
    auto fp_constant = [&](double number) {
        return integer(src.ir, src.ir == Type::F32 ? std::bit_cast<std::uint32_t>(static_cast<float>(number))
                                                   : std::bit_cast<std::uint64_t>(number));
    };
    // Truncation towards zero also admits the open interval (minimum-1, minimum).
    // If minimum-1 is unrepresentable, minimum itself is the first valid float.
    const int precision = src.ir == Type::F32 ? std::numeric_limits<float>::digits : std::numeric_limits<double>::digits;
    const bool inclusive = signed_target && digits >= precision;
    const auto lower_bound = signed_target ? (inclusive ? -bound : -bound - 1) : -1.0;
    const auto above = cmp(inclusive ? "oge" : "ogt", value, fp_constant(lower_bound));
    const auto below = cmp("olt", value, fp_constant(bound));
    const auto valid = instruction(Op::And, Type::I32, {above, below});
    guard(cmp("eq", valid, integer(Type::I32, 0)), "integer conversion out of range");
    return cast(value, dst.ir, signed_target ? "fptosi" : "fptoui");
}

Reg Builder::borrow(const std::string& source,const std::string& target,Reg address){
    if(!lower.core() || source==target || !is_interface(lower.program,target))return address;
    const auto* c=lower.program.find_class(source);
    if(!c || c->is_interface)return address;
    if(stringable_type(lower.program,source))lower.boxed_formatters.insert(source);
    // Classes already carry their descriptor. Struct storage starts at the
    // first field and needs a typed view when borrowed through an interface.
    return c->is_struct ? lower.core_call(*this,"BorrowStruct",Type::Ptr,{context,site(),type(source),address},true) : address;
}

Value Builder::converted(Value v,const std::string& t){
    if(v.type==t)return v;
    auto src=lower.layout(v.type),dst=lower.layout(t);
    if(is_optional(t)) {
        auto p=temp(t);const auto payload=element_type(t);
        if(v.type=="null")return {t,p};
        if(is_optional(v.type)) {
            auto present=block(),done=block();branch(load(Type::I8,v.value),present,done);current=present;
            assign(payload,offset(p,8),from_memory(element_type(v.type),offset(v.value,8)));
            store(p,integer(Type::I8,1));jump(done);current=done;
        } else {assign(payload,offset(p,8),v);store(p,integer(Type::I8,1));}
        return {t,p};
    }
    if(dst.kind==native_abi::Pointer&&(src.kind==native_abi::Pointer||v.type=="null")){
        if(lower.core() && is_pointer(v.type) && is_pointer(t)){
            const auto source=element_type(v.type),target=element_type(t);
            if(is_interface(lower.program,source))
                if(const auto* c=lower.program.find_class(target);c && !c->is_interface)
                    return {t,lower.core_call(*this,"ConcretePointer",Type::Ptr,{context,site(),v.value},true)};
            return {t,borrow(source,target,v.value)};
        }
        return {t,v.value};
    }
    if(dst.kind==native_abi::Interface){auto p=temp(t);call("br_box",Type::Void,{context,site(),type(t),p,type(v.type),memory(v)});return {t,p};}
    if(dst.aggregate&&src.aggregate){auto p=temp(t);copy_value(t,p,v.value);return {t,p};}
    bool si=src.kind==native_abi::Signed||src.kind==native_abi::Unsigned,di=dst.kind==native_abi::Signed||dst.kind==native_abi::Unsigned;
    if(lower.core() && t=="char" && si) {
        auto scalar=cast(v.value,Type::I64,src.kind==native_abi::Signed?"sext":"zext");
        auto surrogate=instruction(Op::And,Type::I32,{cmp("uge",scalar,integer(Type::I64,0xd800)),cmp("ule",scalar,integer(Type::I64,0xdfff))});
        guard(instruction(Op::Or,Type::I32,{surrogate,cmp("ugt",scalar,integer(Type::I64,0x10ffff))}),"invalid Unicode scalar");
        return {t,cast(scalar,Type::I32,"trunc")};
    }
    if (!kernel_body && t=="char" && si) {
        // Match native::cast<char32_t>: first check uint representability,
        // then reject surrogate code points and values above U+10FFFF.
        const auto scalar = converted(v,"uint").value;
        const auto from_surrogates = instruction(Op::Sub,Type::I32,{scalar,integer(Type::I32,0xd800)});
        const auto surrogate = cmp("ule",from_surrogates,integer(Type::I32,0x7ff));
        const auto too_large = cmp("ugt",scalar,integer(Type::I32,0x10ffff));
        guard(instruction(Op::Or,Type::I32,{surrogate,too_large}),"char conversion out of Unicode range");
        return {t,scalar};
    }
    if(t!="char"&&si&&di) {
        // Range checks are visible to both code generators. LLVM can remove
        // checks proved by an induction variable or by a preceding branch.
        const bool signed_source=src.kind==native_abi::Signed;
        const bool signed_target=dst.kind==native_abi::Signed;
        if(dst.size<src.size) {
            const auto limit=std::uint64_t{1}<<(dst.size*8-(signed_target?1:0));
            Reg outside=cmp(signed_source&&signed_target?"sgt":"ugt",v.value,integer(src.ir,limit-1));
            if(signed_source&&signed_target) {
                auto below=cmp("slt",v.value,integer(src.ir,std::uint64_t{0}-limit));
                outside=instruction(Op::Or,Type::I32,{outside,below});
            }
            guard(outside,"integer conversion out of range");
            return {t,cast(v.value,dst.ir,"trunc")};
        }
        if((signed_source&&!signed_target)||(!signed_source&&signed_target&&src.size==dst.size))
            guard(cmp("slt",v.value,integer(src.ir,0)),"integer conversion out of range");
        return {t,cast(v.value,dst.ir,signed_source&&signed_target?"sext":"zext")};
    }
    if(si&&dst.kind==native_abi::Float)return {t,instruction(Op::Cast,dst.ir,{v.value},0,src.kind==native_abi::Signed?"sitofp":"uitofp")};
    if(src.kind==native_abi::Float&&dst.kind==native_abi::Float&&dst.size>=src.size)return {t,cast(v.value,dst.ir,"fpext")};
    if (!kernel_body && src.kind == native_abi::Float) {
        if (di && t != "char") return {t, checked_float_to_integer(v.value, src, dst)};
        if (dst.kind == native_abi::Float) return {t, check_finite(cast(v.value, dst.ir, "fptrunc"))};
    }
    auto p=temp(t);call("br_cast",Type::Void,{context,site(),type(t),p,type(v.type),memory(v)});return from_memory(t,p);
}
void Builder::assign(const std::string& t,Reg dest,Value v){v=converted(std::move(v),t);if(t=="Arena"){if(dest!=v.value){store(dest,load(Type::Ptr,v.value));store(v.value,null());}}else if(lower.layout(t).aggregate)copy_value(t,dest,v.value);else store(dest,v.value);}
Reg Builder::receiver(const Expr& member){const auto& e=*member.args[0];if(member.arrow){auto v=expression(e);guard(cmp("eq",v.value,null()),"null pointer dereference");return v.value;}return expression(e).value;}
Reg Builder::sequence_size(Reg slot,std::size_t member,bool bounded_buffer) {
    const Reg header=load(Type::Ptr,slot);
    const Reg result=alloc(sizeof(std::int32_t),false);
    const Label empty=block(), present=block(), done=block();
    branch(cmp("eq",header,null()),empty,present);
    current=empty;
    store(result,integer(Type::I32,0));
    jump(done);
    current=present;
    const auto address = offset(header,member);
    const Reg count = bounded_buffer ? load_buffer_size(address) : load(Type::I64,address);
    // Buffer.Reserve takes int and every growth path caps count and capacity
    // at int.MaxValue. Arrays can have a larger total element count.
    store(result,bounded_buffer ? cast(count,Type::I32,"trunc") : converted({"ulong",count},"int").value);
    jump(done);
    current=done;
    return load(Type::I32,result);
}
Builder::PrivateArray* Builder::private_array(const Expr& access) {
    if (access.kind != K::Index || access.args.size() != 2) return nullptr;
    const auto& base = *access.args.front();
    if (base.kind != K::Name || base.binding != B::Local) return nullptr;
    auto item = private_arrays.find(base.local_id);
    return item == private_arrays.end() ? nullptr : &item->second;
}

Reg Builder::sequence_index(const std::string& owner_type,const std::string& element,Reg slot,Reg index) {
    const Reg header=load(Type::Ptr,slot);
    const Label missing=block(),present=block();
    branch(cmp("eq",header,null()),missing,present);
    current=missing;
    // The runtime materializes default storage before reporting the bounds
    // error. Preserve that cold path, including an allocation failure in an arena.
    call("br_index",Type::Ptr,{context,site(),type(owner_type),slot,index},false);
    jump(failure);
    current=present;
    const auto count_address = offset(header,offsetof(BrSequenceHeader,count));
    const Reg count = collection_kind(owner_type) == "List" ? load_buffer_size(count_address) : load(Type::I64,count_address);
    guard(cmp("uge",index,count),"index out of range");
    const Reg data=load_sequence_data(header);
    // The allocator already validated capacity * stride and index < count.
    const Reg byte_offset=instruction(Op::Mul,Type::I64,{index,integer(Type::I64,lower.layout(element).size)});
    return instruction(Op::Offset,Type::Ptr,{data,byte_offset});
}
Reg Builder::reference(const Expr& e){
    at=e.token.at;
    // Fresh local allocations have distinct live backing stores. Only tag
    // addresses derived from that allocation, leaving arbitrary pointers,
    // passed-in arrays and handles returned by calls unqualified.
    auto element_address = [&](Reg data,Reg bytes) {
        const auto result = instruction(Op::Offset,Type::Ptr,{data,bytes});
        if (auto* array = private_array(e); array && !array->nullable)
            element_tags.emplace(result,ElementTag{fn.name+".array."+std::to_string(e.args[0]->local_id),e.type});
        return result;
    };
    if (auto it = affine_data.find(&e); it != affine_data.end() && utf8_write_index.first != &e) {
        const auto index = expression(*e.args[1]).value;
        const auto bytes = instruction(Op::Mul,Type::I64,
            {cast(index,Type::I64,"zext"),integer(Type::I64,lower.layout(e.type).size)});
        return element_address(it->second,bytes);
    }
    if(e.binding==B::Local)return locals.at(e.local_id);
    if(e.binding==B::This)return self;
    if(e.binding==B::Field){if(e.is_static)return call(lower.getter(e.owner,e.text),Type::Ptr,{context},true,false);auto object=e.kind==K::Member?receiver(e):self;return lower.field(*this,e.owner,e.text,object);}
    if(e.kind==K::OptionalValue){auto p=expression(*e.args[0]).value;at=e.token.at;guard(cmp("eq",load(Type::I8,p),integer(Type::I8,0)),"optional value is empty");return offset(p,8);}
    if(e.kind==K::Unary&&e.text=="*"){auto p=expression(*e.args[0]).value;at=e.token.at;guard(cmp("eq",p,null()),"null pointer dereference");return p;}
    if(e.kind==K::Call&&is_reference(e.declared_type))return call_expr(e).value;
    if(e.kind==K::Index){auto container=expression(*e.args[0]);
        if(is_pointer(e.owner)){
            auto key=expression(*e.args[1]);at=e.token.at;
            guard(cmp("eq",container.value,null()),"null pointer dereference");
            // Raw pointers have no array bounds. Scale their signed/unsigned
            // index in the caller, using the normal checked integer arithmetic.
            const auto stride = lower.layout(element_type(e.owner)).size;
            const auto& index = *e.args[1];
            if (index.kind == K::Literal && is_integer(index.type) && stride != 0) {
                const auto value = literal_value(index);
                const auto elements = is_unsigned(index.type) ? std::stoull(value.to_string())
                    : static_cast<std::uint64_t>(value.as_int());
                if (elements <= static_cast<std::uint64_t>(INT32_MAX) / stride)
                    return offset(container.value, elements * stride);
            }
            auto index_type=is_unsigned(key.type)?"ulong":"long";
            key=converted(std::move(key),index_type);
            auto bytes=arithmetic("*",key,{index_type,integer(Type::I64,stride)});
            return instruction(Op::Offset,Type::Ptr,{container.value,bytes.value});
        }
        if(array_rank(e.owner)>1){std::vector<Value> keys;for(std::size_t d=1;d<e.args.size();++d)keys.push_back(expression(*e.args[d]));auto indices=alloc(keys.size()*8,false);for(std::size_t d=0;d<keys.size();++d){auto i=converted(std::move(keys[d]),"long");store(offset(indices,d*8),i.value);}at=e.token.at;return call("br_array_index",Type::Ptr,{context,site(),type(e.owner),container.value,indices,integer(Type::I64,e.args.size()-1)});}
        auto key=expression(*e.args[1]);at=e.token.at;
        if(collection_kind(e.owner)=="Dictionary"){key=converted(std::move(key),generic_args(e.owner)[0]);return call("br_key",Type::Ptr,{context,site(),type(e.owner),container.value,memory(key),integer(Type::I32,e.is_static?1:0)});}
        key=converted(std::move(key),"long");
        if (utf8_write_index.first == &e) utf8_write_index.second = key.value;
        if (auto* fact = private_array(e); fact && fact->data) {
            const auto invalid = cmp("uge", key.value, fact->length);
            if (fact->nullable) {
                const auto cold = block(), valid = block();
                branch(invalid,cold,valid);
                current = cold;
                call("br_index",Type::Ptr,{context,site(),type(e.owner),container.value,key.value},false);
                jump(failure);
                current = valid;
            } else guard(invalid, "index out of range");
            const auto bytes = instruction(Op::Mul, Type::I64, {key.value, integer(Type::I64, lower.layout(e.type).size)});
            return element_address(fact->data,bytes);
        }
        return sequence_index(e.owner,e.type,container.value,key.value);}
    if(e.lvalue){auto v=expression(e);return memory(v);}
    throw Error(e.token.at,"native backend: expression does not have an address","BLT4001");
}
Value Builder::arithmetic(const std::string& op,Value a,Value b,bool known_finite){
    if(op=="+"&&(a.type=="string"||b.type=="string")){
        auto as_text=[&](Value v){if(v.type=="string")return v;auto ptr=temp("string");call("br_text",Type::Void,{context,site(),ptr,type(v.type),memory(v)});return Value{"string",ptr};};
        a=as_text(std::move(a));b=as_text(std::move(b));auto p=temp("string");call("br_concat",Type::Void,{context,site(),p,a.value,b.value});return {"string",p};
    }
    auto t=common_numeric(a.type,b.type);a=converted(std::move(a),t);b=converted(std::move(b),t);auto l=lower.layout(t);
    if(l.kind==native_abi::Float){
        if(op=="/")guard(cmp("oeq",b.value,integer(l.ir,0)),"division by zero");
        auto result=instruction(op=="+"?Op::FAdd:op=="-"?Op::FSub:op=="*"?Op::FMul:Op::FDiv,l.ir,{a.value,b.value});
        if (known_finite) return {t,result};
        return {t,check_finite(result)};
    }
    bool sign=l.kind==native_abi::Signed;
    if(op=="+"||op=="-"||op=="*"){
        Instruction i;i.op=op=="+"?Op::CheckedAdd:op=="-"?Op::CheckedSub:Op::CheckedMul;i.result=reg(l.ir);i.overflow=reg(Type::I32);i.args={a.value,b.value};i.text=sign?"s":"u";auto result=i.result,flag=i.overflow;emit(i);
        guard(flag,!sign&&op=="-"?"unsigned underflow in -":"integer overflow in "+op);return {t,result};
    }
    guard(cmp("eq",b.value,integer(l.ir,0)),op=="/"?"division by zero":"remainder by zero");
    if(sign){auto min=integer(l.ir,std::uint64_t(1)<<(l.size*8-1));auto minusone=integer(l.ir,std::numeric_limits<std::uint64_t>::max());auto special=instruction(Op::And,Type::I32,{cmp("eq",a.value,min),cmp("eq",b.value,minusone)});
        if(op=="/")guard(special,"integer overflow in /");
        else {auto slot=temp(t,false);auto special_block=block(),normal=block(),done=block();branch(special,special_block,normal);current=special_block;store(slot,integer(l.ir,0));jump(done);current=normal;store(slot,instruction(Op::SRem,l.ir,{a.value,b.value}));jump(done);current=done;return from_memory(t,slot);}}
    return {t,instruction(op=="/"?(sign?Op::SDiv:Op::UDiv):(sign?Op::SRem:Op::URem),l.ir,{a.value,b.value})};
}

Value Builder::bitwise(const std::string& op,Value a,Value b,const std::string& result){
    a=converted(std::move(a),result);auto l=lower.layout(result);
    if(op=="<<"||op==">>"){
        auto count_layout=lower.layout(b.type);
        if(count_layout.kind==native_abi::Signed)guard(cmp("slt",b.value,integer(count_layout.ir,0)),"negative shift count");
        auto count=cast(b.value,Type::I64,"zext");guard(cmp("uge",count,integer(Type::I64,l.size*8)),"shift count is outside the integer bit width");
        auto shift=cast(count,l.ir,"trunc");return {result,instruction(op=="<<"?Op::Shl:l.kind==native_abi::Signed?Op::AShr:Op::LShr,l.ir,{a.value,shift})};
    }
    b=converted(std::move(b),result);return {result,instruction(op=="&"?Op::And:op=="|"?Op::Or:Op::Xor,l.ir,{a.value,b.value})};
}

void Builder::initialize(const std::string& t,Reg dest) {
    const auto& l = lower.layout(t);
    if (l.zero_default && lower.options.optimize_loops && !lower.module.instrument_budget) {
        instruction(Op::Zero,Type::Void,{dest},l.size);
        return;
    }
    if (l.aggregate) clear_value(t,dest,"br_reset");
    else instruction(Op::Zero,Type::Void,{dest},l.size);
    if (l.kind==native_abi::Class || l.kind==native_abi::Struct || l.kind==native_abi::LibraryValue)
        call(lower.init_name(t),Type::Void,{context,dest},true,false);
}

} // namespace bolt::ir::lowering
