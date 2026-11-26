// Produce the same small ABI probe from each supported ISA, without a Darwin SDK.
#include "../src/native/Object.hpp"
#include <bit>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    using namespace bolt::ir;
    using namespace bolt::native_backend::detail;
    if (argc != 4) return 2;
    const bool arm = std::string_view(argv[1]) == "arm64";
    Module module;
    // C checks both register banks, signed/unsigned narrow arguments, Darwin's
    // naturally packed stack slots, and an absolute data-symbol relocation.
    const std::vector<Type> types{
        Type::I16,Type::I8,Type::I16,Type::I32,Type::I32,Type::I32,Type::I32,Type::I32,
        Type::I8,Type::I16,Type::I32,Type::I64,Type::Ptr,
        Type::F32,Type::F64,Type::F32,Type::F64,Type::F32,Type::F64,Type::F32,Type::F64,
        Type::F32,Type::F32,Type::F64};
    External check{"probe_arguments",Type::I32,types,true};
    check.signed_arguments.resize(types.size());
    check.signed_arguments[0] = check.signed_arguments[9] = true;
    module.externals.push_back(check);
    module.globals.push_back({"payload",{7,5,61,37},false,1,{}});
    module.globals.push_back({"entry",std::vector<std::uint8_t>(8),false,8,{{0,"payload",2}}});

    Function caller;
    caller.name = "bolt_probe";
    caller.result = Type::I32;
    caller.exported = true;
    caller.blocks.emplace_back();
    auto& body = caller.blocks.front().code;
    auto add = [&](Op op, Type type, std::uint64_t bits, std::string text = "", std::vector<Reg> args = {}) {
        auto result = static_cast<Reg>(caller.registers.size());
        caller.registers.push_back(type);
        Instruction i;
        i.op = op; i.result = result; i.immediate = bits; i.text = std::move(text); i.args = std::move(args);
        body.push_back(std::move(i));
        return result;
    };
    std::vector<std::uint64_t> integers{0xfff7,251,65522,17,18,19,20,21,0x21,0xff84,0x354769aa,0x11223344556677};
    std::vector<Reg> args;
    for (std::size_t n=0; n<integers.size(); ++n) args.push_back(add(Op::Constant,types[n],integers[n]));
    auto entry = add(Op::Address,Type::Ptr,0,"entry");
    args.push_back(add(Op::Load,Type::Ptr,0,"",{entry}));
    for (std::size_t n=args.size(); n<types.size(); ++n) {
        const double d = n+0.25;
        auto bits = types[n]==Type::F64 ? std::bit_cast<std::uint64_t>(d) : std::bit_cast<std::uint32_t>(static_cast<float>(d));
        args.push_back(add(Op::Constant,types[n],bits));
    }
    const auto called=add(Op::Call,Type::I32,0,"probe_arguments",std::move(args));
    Instruction ret; ret.op=Op::Return; ret.args={called}; body.push_back(ret);
    module.functions.push_back(std::move(caller));

    // Verify incoming stack arguments from Clang-generated C code independently.
    Function callback;
    callback.name="bolt_spilled"; callback.exported=true; callback.parameters=types; callback.result=Type::I32;
    callback.registers.insert(callback.registers.end(),types.begin(),types.end());
    callback.blocks.emplace_back();
    Instruction widen; widen.op=Op::Cast; widen.text="sext"; widen.args={10};
    widen.result=static_cast<Reg>(callback.registers.size()); callback.registers.push_back(Type::I32);
    callback.blocks.front().code.push_back(widen);
    ret.args={widen.result}; callback.blocks.front().code.push_back(ret);
    module.functions.push_back(std::move(callback));

    std::ofstream out(argv[3],std::ios::binary);
    if (std::string_view(argv[2])=="ir") {
        out << llvm_text(module,arm?"arm64-apple-macosx13.0":"x86_64-apple-macosx13.0",false,false);
    } else {
        auto image=arm ? aarch64_image(module,ArmABI::Apple) : x64_image(module);
        auto bytes=macho_object(image,13u<<16);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    }
    return out ? 0 : 1;
}
