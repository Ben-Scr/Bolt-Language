#include <bolt/Bolt.hpp>
#include <bolt/native/Jit.hpp>
#include <bolt/native/IR.hpp>
#include "ArenaNativeOwnership.hpp"
#include "NativeIntegerHash.hpp"
#include "NativeStorageShift.hpp"
#include "UnicodeBlockCases.hpp"
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#include "GenericConstantCases.hpp"
#include "ThisCases.hpp"
#include "IterationCases.hpp"
#include "ExtensionMethodCases.hpp"
#include "DefaultCases.hpp"
#include "ArenaCases.hpp"
#include "NumericParsingCases.hpp"
#include "RectArrayCases.hpp"
#include "SizeOfFormattingCases.hpp"
#include "VariadicGenericCases.hpp"
int passed=0,failed=0;
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void test(const std::string& name,const std::function<void()>& fn){
    try{fn();++passed;std::cout<<"PASS "<<name<<'\n';}
    catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
}
std::string wrap(const std::string& body){return "import Math\nimport Random\nimport Type\nimport System.Collections.Generic\nstatic class Program { public: void Main() {\n"+body+"\n} }\n";}
}
int main(){
    // Keep the last completed case visible if native generated code crashes.
    std::cout.setf(std::ios::unitbuf);
    using namespace bolt;
    test("unicode-block-boundaries",[]{bolt::tests::verify_utf8_block_boundaries();});
    test("native-integer-hash", bolt::tests::verify_native_integer_hash);
    test("native-arena-owned-storage",[]{bolt::tests::verify_native_arena_ownership();});
    test("native-ir-zero-uses-bolt-runtime",[]{
        ir::Module module;
        ir::Function fn;
        fn.name="zero_test";
        fn.result=ir::Type::Void;
        fn.parameters={ir::Type::Ptr};
        fn.registers={ir::Type::Void,ir::Type::Ptr};
        fn.blocks.emplace_back();
        ir::Instruction zero;zero.op=ir::Op::Zero;zero.args={1};zero.immediate=64;fn.blocks[0].code.push_back(zero);
        ir::Instruction ret;ret.op=ir::Op::Return;fn.blocks[0].code.push_back(ret);
        module.functions.push_back(std::move(fn));
        const auto text=ir::dump(module);
        check(text.find("call void @br_zero(ptr %r1, i64 64)")!=std::string::npos,"zeroing does not use BoltRuntime");
        check(text.find("llvm.memset")==std::string::npos,"LLVM memset intrinsic can reintroduce a CRT memset dependency");
    });
    std::vector<JitOptions> modes;
    if(native_backend::fast_supported()){JitOptions fast;fast.backend="fast";modes.push_back(fast);}
    std::string reason;
    bool have_llvm=false;
    try{reason=native_backend::llvm_version();have_llvm=true;}catch(const std::exception& e){reason=e.what();}
    if(have_llvm){
        test("llvm-release/storage-shift-alignments-and-fallback",tests::verify_storage_shifts);
        JitOptions llvm;llvm.backend="llvm";modes.push_back(llvm);llvm.release=true;modes.push_back(llvm);
    }else std::cout<<"SKIP LLVM tests: "<<reason<<'\n';
    if(modes.empty()){std::cout<<"SKIP: no installed native backend supports this host\n";return 0;}
    for(const auto& mode:modes){
        const std::string prefix=mode.backend+(mode.release?"-release":"-debug")+"/";
        auto execute=[&](const std::string& source,const std::string& input="",int expected=0){
            std::istringstream in(input);std::ostringstream out;auto opts=mode;opts.input=&in;opts.output=&out;
            NativeProgram compiled(compile_source(source,"native-unit.bolt"),opts);
            check(compiled.run()==expected,"incorrect native exit status");return out.str();
        };
        auto output=[&](const std::string& name,const std::string& body,const std::string& expected){
            test(prefix+name,[&]{check(execute(wrap(body))==expected,"native output mismatch");});
        };
        auto rejects=[&](const std::string& name,const std::string& source,const std::string& diagnostic){
            test(prefix+name,[&]{try{(void)execute(source);}catch(const Error& e){check(e.what()&&std::string(e.what()).find(diagnostic)!=std::string::npos,"wrong diagnostic: "+std::string(e.what()));return;}throw std::runtime_error("expected a Bolt diagnostic");});
        };
        test(prefix+"memory-operations-contract",[&]{
            check(execute(R"BOLT(import System
import System.Memory
import Bolt.Runtime

public static class Program {
public:
    int Main() {
        byte[] data = new byte[70];
        for (int i = 0; i < data.Length; i++) data[i] = (byte)i;
        byte* p = &data[0];
        ulong address = MemoryOps.Address(p);
        if (MemoryOps.Address(null) != 0ul || MemoryOps.Address(&data[1]) != address + 1ul) return 1;
        Memory.Copy(null, null, 0ul);
        Memory.Move(null, null, 0ul);
        Memory.Set(null, (byte)1, 0ul);
        if (Memory.Compare(null, null, 0ul) != 0) return 2;
        Memory.Move(&data[3], &data[1], 31ul);
        for (int i = 0; i < 31; i++) if (data[3 + i] != (byte)(1 + i)) return 3;
        Memory.Move(&data[1], &data[3], 31ul);
        for (int i = 0; i < 31; i++) if (data[1 + i] != (byte)(1 + i)) return 4;
        Memory.Copy(&data[37], &data[1], 31ul);
        if (Memory.Compare(&data[37], &data[1], 31ul) != 0) return 5;
        Memory.Set(&data[67], (byte)255, 1ul);
        if (Memory.Compare(&data[37], &data[1], 31ul) != 1) return 6;
        if (Memory.Compare(&data[1], &data[37], 31ul) != -1) return 7;
        Memory.Move(p, p, 70ul);
        try { Memory.Copy(p, p, 1ul); return 8; } catch (ArgumentException e) {}
        try { Memory.Set(null, (byte)1, 1ul); return 9; } catch (ArgumentNullException e) {}
        try { Memory.Copy(p, &data[1], 2ul); return 10; } catch (ArgumentException e) {}
        try { Memory.Copy(&data[1], p, 2ul); return 11; } catch (ArgumentException e) {}
        try { Memory.Copy(p, &data[1], ulong.MaxValue); return 12; } catch (OverflowException e) {}
        try { Memory.Move(p, p, ulong.MaxValue); return 13; } catch (OverflowException e) {}
        try { Memory.Compare(p, p, ulong.MaxValue); return 14; } catch (OverflowException e) {}
        try { Memory.Set(p, (byte)25, ulong.MaxValue); return 15; } catch (OverflowException e) {}
        try { Memory.Copy(null, p, ulong.MaxValue); return 16; } catch (ArgumentNullException e) {}
        if (data[0] != 0 || data[1] != 1 || data[67] != 255) return 17;
        return 0;
    }
}
)BOLT") == "", "unexpected memory test output");
        });
        test(prefix+"string-receiver-before-argument-side-effect",[&]{
            check(execute(R"BOLT(public static class Program {
public:
    int Coordinates(int x, int y) {
        int offset = 3;
        int result = x * 2 + y;
        return result + offset;
    }
    string Change(string& s) { s = "replacement"; return "first"; }
    bool CheckAlias(string& value) { return value.Contains(Change(value)); }
    int Main() {
        string text = "first text";
        if (!CheckAlias(text) || text != "replacement") return 1;
        if (Coordinates(5, 6) != 19) return 2;
        return 0;
    }
}
)BOLT") == "", "unexpected alias test output");
        });
        test(prefix+"typed-buffer-to-array",[&]{
            check(execute(R"BOLT(import Bolt.Runtime

public static class Program {
public:
    int Main() {
        Buffer<int> empty;
        if (empty.ToArray().Length != 0) return 1;
        empty.Add(3);
        int[] values = empty.ToArray();
        empty[0] = 9;
        if (values[0] != 3) return 2;
        values[0] = 11;
        if (empty[0] != 9) return 3;
        Buffer<string> words;
        words.Add("first");
        words.Add("second");
        string[] copy = words.ToArray();
        words[0] = "change";
        words.Clear();
        if (copy.Length != 2 || copy[0] != "first" || copy[1] != "second") return 4;
        Buffer<int*> pointers;
        int x = 63;
        pointers.Add(&x);
        int*[] addresses = pointers.ToArray();
        pointers.Clear();
        if (*addresses[0] != 63) return 5;
        return 0;
    }
}
)BOLT") == "", "unexpected ToArray test output");
        });
        test(prefix+"buffer-append-growth-and-value-copies",[&]{
            check(execute(R"BOLT(import Bolt.Runtime

public static class Program {
public:
    int Main() {
        Buffer<int> data;
        data.Reserve(1);
        data.Add(5);
        data.Add(data[0]);
        if (data[0] != 5 || data[1] != 5) return 1;
        int[] single = [31];
        int changes = 0;
        for (int i = 0; i < 4096; i++) {
            int old = data.Capacity;
            data.AppendRange(single, 0, 1);
            if (old != data.Capacity) changes++;
        }
        if (changes > 13 || data.Length != 4098 || data[4097] != 31) return 2;
        Buffer<int> duplicate = data;
        data[0] = 7;
        if (duplicate[0] != 5) return 3;
        string[] source = ["old", "tail"];
        Buffer<string> words;
        words.AppendRange(source, 0, 2);
        source[0] = "new";
        if (words[0] != "old") return 4;
        Buffer<string> other = words;
        words.Clear();
        if (other[0] != "old" || other[1] != "tail") return 5;
        return 0;
    }
}
)BOLT") == "", "unexpected buffer test output");
        });
        for(const auto& feature:iteration_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{const auto actual=execute(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
        }
        for(const auto& feature:extension_method_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{const auto actual=execute(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
        }
        for(const auto& feature:numeric_parsing_cases()){
            if(!feature.error.empty())rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{
                const auto actual=execute(feature.source);
                check(actual==feature.expected,"expected ["+feature.expected+"], got ["+actual+"]");
            });
        }
        for(const auto& feature:variadic_generic_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{check(execute(feature.source)==feature.expected,"variadic native output mismatch");});
        }
        for(const auto& feature:sizeof_formatting_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{const auto actual=execute(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
        }
        test(prefix+"pointer-address-output",[&]{check(correct_pointer_output(execute(pointer_output_source)),"pointer addresses/value/reference formatting mismatch");});
        for(const auto& feature:generic_constant_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{
                const auto actual=execute(feature.source);
                check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");
            });
        }
        for(const auto& feature:this_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{
                const auto actual=execute(feature.source);
                check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");
            });
        }
        for(const auto& feature:default_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{
                const auto actual=execute(feature.source);
                check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");
            });
        }
        for(const auto& feature:arena_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{auto actual=execute(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
        }
        test(prefix+"arena-unbraced-branch-cleanup",[&]{
            struct ObservedOutput:std::stringbuf {
                std::size_t maximum=0;
                std::streamsize xsputn(const char* s,std::streamsize n)override{maximum=std::max(maximum,memory::arenas.size());return std::stringbuf::xsputn(s,n);}
            } buffer;
            auto baseline=memory::arenas.size();std::ostream out(&buffer);auto options=mode;options.output=&out;
            NativeProgram compiled(compile_source(wrap("if(true) Arena first=Arena.Fixed(4096); Print(1); if(false) Print(0); else Arena second=Arena.Fixed(4096); Print(2);")),options);
            check(compiled.run()==0&&buffer.str()=="1\n2\n","unbraced branch output mismatch");
            check(buffer.maximum==baseline&&memory::arenas.size()==baseline,"branch arena survived its lexical scope");
        });
        test(prefix+"arena-failure-cleans-owner",[&]{
            auto baseline=memory::arenas.size();
            try{(void)execute("class Item{public:string Text;Item(int n){Text=\"owned\";int bad=1/n;}} static class Program{public:void Main(){Arena work=Arena.Fixed(65536);Item* p=new(work) Item(0);}}");}
            catch(const Error& e){check(std::string(e.what()).find("division by zero")!=std::string::npos,"wrong constructor failure");check(memory::arenas.size()==baseline,"failed constructor leaked its arena owner");return;}
            throw std::runtime_error("constructor failure was not reported");
        });
        for(const auto& feature:rect_array_cases){
            if(*feature.error)rejects(feature.name,feature.source,feature.error);
            else test(prefix+feature.name,[&]{
                const auto actual=execute(feature.source);
                check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");
            });
        }
        output("explicit-casts","Print((string)0); Print((int)-3.8); Print((double)7); Print((bool)true);", "0\n-3\n7\ntrue\n");
        output("var-locals-and-loops", "var value=0; for(var i=0;i<4;i++){value+=i;} {var value=value+1;Print(value);} Print(value); var text=\"Bolt\"; text+=\"!\"; Print(text); var flag=value==6; Print(flag);", "7\n6\nBolt!\ntrue\n");
        test(prefix+"var-inferred-types",[&]{check(execute(R"(
            import Type
            string Name<T>(T value){return Type.Name<T>();}
            static class Program {public:void Main(){
                var integer=0; var wide=1l; var positive=1u; var big=1ul;
                var single=1f; var real=1.0; var flag=true; var text="Bolt";
                var small=short(3); var promoted=small+1;
                Print(Name(integer)+","+Name(wide)+","+Name(positive)+","+Name(big));
                Print(Name(single)+","+Name(real)+","+Name(flag)+","+Name(text));
                Print(Name(small)+","+Name(promoted));
                integer=2.9; Print(integer); Print(Name(integer));
            }}
        )") == "int,long,uint,ulong\nfloat,double,bool,string\nshort,int\n2\nint\n","var inferred type mismatch");});
        test(prefix+"var-calls-and-value-copies",[&]{check(execute(R"(
            class Item {public:int X; Item(int value){X=value;}}
            T Identity<T>(T value){var copy=value;return copy;}
            Item Make(int value){return Item(value);}
            int& Ref(int& value){return value;}
            static class Program {public:void Main(){
                var original=Make(7); var copy=Identity(original); copy.X=9;
                var direct=Item(11); var number=Ref(original.X); number++;
                var pointer=&original.X; *pointer=12;
                Print(original.X+","+copy.X+","+direct.X+","+number);
            }}
        )") == "12,9,11,8\n","var call or value semantics mismatch");});
        output("var-arrays-and-collections", R"(
            var values={1,2,3}; var copied=values; copied[0]=9;
            var allocated=new int[2]; allocated[1]=7; var nested={{4,5},{6}};
            List<int> source; source.Add(3); var list=source; list[0]=8;
            var created=List<int>(); created.Add(11);
            Print(values[0]+","+copied[0]+","+allocated[1]+","+nested[0][1]);
            Print(source[0]+","+list[0]+","+created[0]);
        )", "1,9,7,5\n3,8,11\n");
        output("literal-string-affixes", R"(
            string[] s=["", "a\0b", "x0123456789abcdef", "0123456!", "ä😀"];
            int i=0;
            Print(s[i++].EndsWith("") && i==1);
            Print(s[1].StartsWith("a\0") && s[1].EndsWith("\0b"));
            Print(s[2].EndsWith("0123456789abcdef") && s[2].StartsWith("x01234567"));
            Print(s[2].StartsWith("x0123456789abcdef") && !s[3].EndsWith("01234567"));
            Print(!s[0].StartsWith("x") && !s[3].EndsWith("0123456789"));
            Print(s[4].StartsWith("ä") && s[4].EndsWith("😀"));
        )","true\ntrue\ntrue\ntrue\ntrue\ntrue\n");
        test(prefix+"plain-buffer-append-alias",[&]{check(execute(R"(
            import Bolt.Runtime
            struct Pair {public: int X; int Y;}
            static class Program {public: void Main(){
                Buffer<Pair> items; items.Reserve(1);
                Pair p; p.X=8; p.Y=17; items.Add(p);
                items.Add(items[0]); items.Reserve(8); items.Add(items[1]);
                items[0].X=99; items[1].Y=42;
                Print(items[1].X+","+items[2].Y);
            }}
        )")=="8,17\n","record append lost its by-value snapshot");});
        test(prefix+"reserved-list-refill-and-checked-sum",[&]{check(execute(R"(
            import System
            import System.Collections.Generic
            static class Program { public: void Main() {
                List<int> values; values.Reserve(20);
                int salt=1;
                values.Clear();
                for(int i=0;i<7;i++) values.Add((i^salt)&7);
                salt=5;
                int first=values[0]; int last=values[6];
                Print(first+","+last+","+values.Count);
                int sum=int.MaxValue-3;
                try { for(int i=0;i<values.Count;i++) sum+=values[i]; }
                catch(OverflowException e) { Print(sum); }
                values.Clear();
                try { int missing=values[0]; Print(missing); }
                catch(IndexOutOfRangeException e) { Print(values.Count); }
            }}
        )")=="1,7,7\n2147483645\n0\n","projected values, overflow prefix or cleared bounds changed");});
        test(prefix+"list-element-borrow-keeps-storage",[&]{check(execute(R"(
            import System.Collections.Generic
            static class Program { public: void Main() {
                List<int> values; values.Reserve(4);
                values.Clear(); for(int i=0;i<4;i++) values.Add(i);
                int& item=values[2]; item=29; Print(values[2]);
            }}
        )")=="29\n","a borrowed list element lost its storage");});
        test(prefix+"list-refill-negative-and-irregular-length",[&]{check(execute(R"(
            import System
            import System.Collections.Generic
            static class Program { public: void Main() {
                List<int> values; values.Reserve(24);
                int sign=int.MinValue, mask=5;
                values.Clear(); for(int i=0;i<13;i++) values.Add((i&mask)|sign);
                sign=0; mask=0;
                int sum=int.MaxValue;
                try { for(int i=0;i<values.Length;i++) sum+=values[i]; }
                catch(OverflowException e) { Print(sum); }
            }}
        )")=="-2147483648\n","negative prefix changed on projected overflow");});
        test(prefix+"materialized-list-partial-append",[&]{check(execute(R"(
            import System
            import System.Collections.Generic
            static class Program { public: void Main() {
                List<int> values;
                for(int i=0;i<9;i++) values.Add(i);
                try { for(int i=0;i<8;i++) values.Add((int.MaxValue-3)+i); }
                catch(OverflowException e) { Print(values.Count+","+values[12]); }
                var it=values.GetEnumerator();
                for(int i=0;i<2;i++) values.Add(i);
                try { it.MoveNext(); } catch(InvalidOperationException e) { Print(values.Count); }
            }}
        )")=="13,2147483647\n15\n","bulk append lost growth, completed prefix or revision");});
        test(prefix+"scalar-buffer-insert-remove",[&]{check(execute(R"(
            import System
            import Bolt.Runtime
            static class Program { public:
                int Replace(Buffer<int>& b) {
                    Buffer<int> next; next.Reserve(8); next.Add(19); b.Swap(next); return 5;
                }
                void Main() {
                    Buffer<int> b; b.Insert(0,2); b.Reserve(8); b.Insert(1,7);
                    b.Insert(0,b[1]); b.RemoveAt(1);
                    Print(b[0]+","+b[1]);
                    b.Insert(1,Replace(b)); Print(b[0]+","+b[1]);
                    try { b.RemoveAt(-1); } catch(Exception e) { Print(b.Length); }
                    try { b.Insert(3,42); } catch(Exception e) { Print(b.Length); }
                    try { for(int v : b) { b.RemoveAt(0); } }
                    catch(Exception e) { Print(b.Length); }
                }
            }
        )")=="7,7\n19,5\n2\n2\n1\n","scalar buffer shift lost alias, bounds or iteration semantics");});
        test(prefix+"list-edit-pair-index-order",[&]{check(execute(R"(
            import System.Collections.Generic
            static class Program { public: int Main() {
                for(int n=0;n<=5;n++) {
                    for(int p=0;p<=n;p++) for(int q=0;q<=n;q++) for(int r=0;r<=n;r++) {
                        List<int> list; list.Reserve(n+1);
                        for(int i=0;i<n;i++) list.Add(i);
                        int observed=7;
                        list.Insert(p,19);
                        observed-=list[r];
                        list.RemoveAt(q);
                        if(list.Count!=n || observed!=7-(r==p?19:(r<p?r:r-1))) return 1;
                        for(int i=0;i<n;i++) {
                            int at=i<q?i:i+1;
                            if(list[i]!=(at==p?19:(at<p?at:at-1))) return 2;
                        }
                    }
                }
                return 0;
            }}
        )")=="","list edit lost an endpoint or observed the wrong intermediate element");});
        test(prefix+"list-edit-failure-prefix",[&]{check(execute(R"(
            import System
            import System.Collections.Generic
            static class Program { public:
                int Replace(List<int>& list,int& calls) {
                    calls++; list.Clear(); list.Add(19); return 5;
                }
                int Main() {
                    List<int> list; int calls=0;
                    list.Insert(0,Replace(list,calls));
                    if(calls!=1 || list.Count!=2 || list[0]!=5 || list[1]!=19) return 1;
                    var it=list.GetEnumerator();
                    ulong sum=3ul; int negative=-1;
                    try { list.Insert(1,negative); sum+=(ulong)list[1]; list.RemoveAt(0); return 2; }
                    catch(Exception e) { if(sum!=3ul || list.Count!=3 || list[1]!=-1) return 3; }
                    try { it.MoveNext(); return 4; } catch(InvalidOperationException e) {}
                    list.Reserve(16); int total=int.MaxValue;
                    try { list.Insert(0,7); total+=list[0]; list.RemoveAt(0); return 5; }
                    catch(OverflowException e) { if(total!=int.MaxValue || list.Count!=4 || list[0]!=7) return 6; }
                    total=0;
                    try { list.Insert(0,8); total=list[0]; list.RemoveAt(-1); return 7; }
                    catch(IndexOutOfRangeException e) { if(total!=8 || list.Count!=5) return 8; }
                    try { list.Insert(0,9); total=list[12]; list.RemoveAt(0); return 9; }
                    catch(IndexOutOfRangeException e) { if(total!=8 || list.Count!=6 || list[0]!=9) return 10; }
                    try { list.Insert(-1,77); total=list[0]; list.RemoveAt(0); return 11; }
                    catch(IndexOutOfRangeException e) { if(total!=8 || list.Count!=6) return 12; }
                    var fresh=list.GetEnumerator(); list.Insert(0,22); list.RemoveAt(0);
                    try { fresh.MoveNext(); return 13; } catch(InvalidOperationException e) {}
                    return 0;
                }
            }
        )")=="","a failed list edit changed the observable prefix, evaluation order or revision");});
        test(prefix+"list-edit-in-owning-array",[&]{check(execute(R"(
            import System
            import System.Collections.Generic
            static class Program { public:
                int Main() {
                    List<int>[] lists=new List<int>[19];
                    for(int i=0;i<19;i++) lists[i].Insert(0,i);
                    List<int> copy=lists[1];
                    lists[1].Insert(0,23); lists[1].RemoveAt(1);
                    if(copy.Count!=1 || copy[0]!=1 || lists[0][0]!=0 || lists[1][0]!=23 || lists[2][0]!=2) return 1;
                    try { lists[19].RemoveAt(0); return 2; } catch(IndexOutOfRangeException e) {}
                    int[] positions=new int[19]; positions[13]=7;
                    try {
                        for(int i=0;i<19;i++) lists[i].RemoveAt(positions[i]);
                        return 3;
                    } catch(IndexOutOfRangeException e) {}
                    for(int i=0;i<13;i++) if(lists[i].Count!=0) return 4;
                    for(int i=13;i<19;i++) if(lists[i].Count!=1 || lists[i][0]!=i) return 5;
                    Bolt.Runtime.Buffer<int> outer; outer.Add(1); outer.Add(2);
                    var cursor=lists[13].GetEnumerator();
                    int visits=0;
                    for(int value : outer) {
                        visits+=value;
                        if(value==1) for(int i=13;i<19;i++) lists[i].RemoveAt(0);
                    }
                    if(visits!=3 || lists[18].Count!=0) return 6;
                    try { cursor.MoveNext(); return 7; } catch(InvalidOperationException e) {}
                    return 0;
                }
            }
        )")=="","list-array edit lost a value copy or an outer bound");});
        test(prefix+"short-overlapping-scalar-shifts",[&]{check(execute(R"(
            import System.Collections.Generic
            static class Program { public: int Main() {
                List<byte> values; List<short> halves; List<long> words;
                values.Reserve(132); halves.Reserve(132); words.Reserve(132);
                for(int i=0;i<130;i++) { values.Add((byte)i); halves.Add((short)(-i)); words.Add(9000000000l+i); }
                for(int p=0;p<=130;p++) {
                    values.Insert(p,(byte)233);
                    halves.Insert(p,(short)(-233)); words.Insert(p,9000000233l);
                    for(int i=0;i<=130;i++) {
                        int expected=i==p?233:(i<p?i:i-1);
                        if(values[i]!=expected || halves[i]!=-expected || words[i]!=9000000000l+expected) return 1;
                    }
                    values.RemoveAt(p); halves.RemoveAt(p); words.RemoveAt(p);
                    for(int i=0;i<130;i++) if(values[i]!=i || halves[i]!=-i || words[i]!=9000000000l+i) return 2;
                }
                return 0;
            }}
        )")=="","a scalar shift lost bytes at a short-copy boundary");});
        test(prefix+"widening-sequence-sums-and-checked-prefix",[&]{check(execute(R"(
            import System
            import System.Collections.Generic
            static class Program { public: void Main() {
                int[] x=[2,-3,9];
                List<int> y; y.Add(-7); y.Add(6);
                long a=1l, b=-3l;
                int count=2;
                for(int i=0;i<count;i++) { a+=x[i]; b+=y[i]; }
                Print(a+","+b);
                a=1l; b=-3l; count=3;
                try { for(int i=0;i<count;i++) { a+=x[i]; b+=y[i]; } }
                catch(IndexOutOfRangeException e) { Print(a+","+b); }
                a=long.MaxValue-3l; b=0l; count=2; x[1]=3;
                try { for(int i=0;i<count;i++) { a+=x[i]; b+=y[i]; } }
                catch(OverflowException e) { Print((a==long.MaxValue-1l)+","+b); }
                a=long.MinValue+1l; x[0]=-2;
                try { for(int i=0;i<count;i++) a+=x[i]; }
                catch(OverflowException e) { Print(a==long.MinValue+1l); }
                count=-1;
                for(int i=0;i<count;i++) a+=x[i];
                Print(a==long.MinValue+1l);
            }}
        )")=="0,-4\n9,-4\ntrue,-7\ntrue\ntrue\n","widening sums changed a checked prefix");});
        test(prefix+"private-record-copy-and-self-assignment",[&]{check(execute(R"(
            struct Block { public: long A; long B; long C; long D; long E; long F; long G; long H; long I; }
            static class Program { public: void Main() {
                Block[] a=new Block[3]; Block[] b=new Block[2];
                a[1].A=71; a[1].I=19; b[0]=a[1]; a[2]=b[0]; a[1]=a[1];
                a[1].I=33; Print(a[1].A+","+a[2].I+","+b[0].A+","+a[1].I);
            }}
        )")=="71,19,71,33\n","whole-element copy changed another record");});
        test(prefix+"overlap-through-reinterpreted-array-elements",[&]{check(execute(R"(
            struct Block { public: long A; long B; long C; long D; long E; long F; long G; long H; long I; }
            static class Program { public: void Main() {
                long[] words=new long[10];
                for(int i=0;i<10;i++) words[i]=i+10;
                *((Block*)&words[1])=*((Block*)&words[0]);
                Print(words[1]+","+words[2]+","+words[9]);
            }}
        )")=="10,11,18\n","a partially overlapping record was treated as a whole array element");});
        test(prefix+"short-circuit-in-versioned-array-filter",[&]{check(execute(R"(
            import System
            static class Program { public: void Main() {
                byte[] mask=new byte[32]; int[] seen=new int[4]; int stamp=5;
                mask[0]=1;
                for(int i=0;i<32;i++) {
                    if(mask[i]!=0 && seen[i]!=stamp) seen[i]=stamp;
                }
                mask[9]=1; stamp=6;
                try {
                    for(int i=0;i<32;i++) {
                        if(mask[i]!=0 && seen[i]!=stamp) seen[i]=stamp;
                    }
                } catch(IndexOutOfRangeException e) { Print(seen[0]); }
            }}
        )")=="6\n","array filter lost short circuit or changed the prefix on error");});
        test(prefix+"large-snapshots-keep-independent-storage",[&]{check(execute(R"(
            import Bolt.Runtime
            static class Program {public: void Main(){
                int[] input=new int[16384]; input[16383]=19;
                Buffer<int> b; b.AppendRange(input,0,input.Length);
                int[] first=b.ToArray();
                { int[] shortLived=b.ToArray(); shortLived[0]=99; }
                b[0]=7;
                int[] second=b.ToArray();
                { b.Add(31); int[] longer=b.ToArray(); Print(longer[16384]); }
                b.Truncate(16384);
                int[] third=b.ToArray(); third[16383]=42;
                Print(first[0]+","+second[0]+","+second[16383]+","+b[16383]+","+third[16383]);
            }}
        )")=="31\n0,7,19,19,42\n","array snapshots shared live or stale storage");});
        test(prefix+"var-preserves-resolved-namespace",[&]{
            auto program=parse(R"(
                import System.Collections.Generic
                class Item {public:int X;Item(int value){X=value;}}
                static class Factory {public:
                    Item Make(){return Item(7);}
                    Item[] MakeArray(){return {Item(8)};}
                    List<Item> MakeList(){List<Item> values;values.Add(Item(9));return values;}
                }
            )", "inferred-global.bolt");
            auto consumer=parse(R"(
                namespace Client;
                class Item {public:int Other;}
                static class Program {public:void Main(){
                    var item=Factory.Make();var array=Factory.MakeArray();var list=Factory.MakeList();
                    Print(item.X+","+array[0].X+","+list[0].X);
                }}
            )", "inferred-consumer.bolt");
            for(auto& c:consumer.classes)program.classes.push_back(std::move(c));
            program.sources.insert(consumer.sources.begin(),consumer.sources.end());
            analyze(program);
            std::istringstream in;std::ostringstream out;auto opts=mode;opts.input=&in;opts.output=&out;
            NativeProgram compiled(std::move(program),opts);
            check(compiled.run()==0&&out.str()=="7,8,9\n","var re-resolved a canonical initializer type in the caller's namespace");
        });
        output("const-var", "const var value=7;var copy=value;copy++;Print(value);Print(copy);", "7\n8\n");
        rejects("var-assignment-keeps-type",wrap("var value=0;value=\"text\";"),"cannot assign");
        output("parentheses-not-casts","int a=4; int b=2; Print((a)+b); Print((a)-b); Print((a)*b);", "6\n2\n8\n");
        output("bits-32-64","Print(Reinterpret<uint>(1.0f)); Print(Reinterpret<float>(1065353216u)); Print(Reinterpret<ulong>(1.0)); Print(Reinterpret<double>(4607182418800017408ul));", "1065353216\n1\n4607182418800017408\n1\n");
        output("bits-signed-small","Print(Reinterpret<ushort>(short(-1))); Print(Reinterpret<short>(ushort(65535))); Print(Reinterpret<short>(ushort(65535))); Print(Reinterpret<int>(4294967295u));", "65535\n-1\n-1\n-1\n");
        test(prefix+"sequence-drops-inherited-field-in-reverse-order",[&]{
            check(execute(R"(
                class Base { public: static bool Recording; static int History; int Id;
                    ~Base() { if (Recording) History = History * 10 + Id; }
                }
                class Derived : Base { public: ulong Other; }
                struct Record { public: Derived Value; }
                static class Program { public: void Main() {
                    Record[] records = new Record[3];
                    for (int i=0; i<3; i++) records[i].Value.Id = i+1;
                    Base.Recording = true;
                    records = new Record[0];
                    Base.Recording = false;
                    Print(Base.History);
                } }
            )") == "321\n","sequence cleanup skipped or reordered inherited destructors");
        });
        test(prefix+"literal-pointer-element-offsets",[&]{
            check(execute(R"(
                static class Program { public:
                    void Main() {
                        int[] data={7, 11, 19, 29};
                        int* p=&data[0];
                        p[2u]=31;
                        int* q=&p[3];
                        int n=-2;
                        Print(p[0]+p[2ul]+q[n]);
                    }
                })") == "49\n","constant and dynamic pointer offsets disagree");
        });
        rejects("literal-pointer-null",R"(static class Program {public: void Main() { int* p=null; Print(p[1]); }})","null pointer dereference");
        rejects("literal-pointer-overflow",R"(static class Program {public: void Main() { int[] a={9}; int* p=&a[0]; Print(p[4611686018427387904ul]); }})","integer overflow in *");
        test(prefix+"small-optional-value-copy",[&]{
            check(execute(R"(
                class Handle { public: ulong Id; }
                class Owned { public: string Text; }
                static class Program { public:
                    void Copy(byte?& dest, const byte?& src) { dest = src; }
                    void CopyHandle(Handle?& dest, const Handle?& src) { dest = src; }
                    void Main() {
                        byte? x = (byte)17; byte? y = (byte)3;
                        byte* raw = (byte*)&x; raw[1] = (byte)99;
                        Copy(x,x); Print(raw[1] == (byte)99);
                        Copy(y,x); Print(y.Value == (byte)17 && ((byte*)&y)[1] == (byte)0);
                        Handle h; h.Id = 135ul;
                        Handle? item = h; Handle? copied; CopyHandle(copied,item);
                        Print(copied.Value.Id == 135ul);
                        if (sizeof(ulong*) == 8) {
                            ulong[] words = {1ul, 37ul, 88ul};
                            byte?& first = *((byte?*)&words[0]); byte?& next = *((byte?*)&words[1]);
                            Copy(next,first); Print(words[1] == 1ul && words[2] == 37ul);
                            words[2] = 51ul; Copy(first,next); Print(words[0] == 1ul && words[1] == 51ul);
                            words[0] = 0ul; Copy(next,first); Print(words[1] == 0ul && words[2] == 0ul);
                        } else { Print(true); Print(true); Print(true); }
                        Owned a; a.Text = "kept"; Owned? a1 = a; Owned? a2 = a1; a1 = null;
                        Print(a2.Value.Text);
                    }
                }
            )") == "true\ntrue\ntrue\ntrue\ntrue\ntrue\nkept\n","optional tag, ownership or overlapping payload changed");
        });
        test(prefix+"small-class-value-copy",[&]{
            check(execute(R"(
                class Base { public: byte Tag; virtual int Kind() { return 1; } }
                class Derived : Base { public: override int Kind() { return 2; } }
                class Item { public: Base Part; ulong Id; }
                class Pair { public: ulong First, Last; }
                class Owned { public: string Text; }
                static class Program { public:
                    void Assign(Item& dest, const Item& src) { dest = src; }
                    int KindOf(Base& value) { return value.Kind(); }
                    void Main() {
                        Item[] items = new Item[2];
                        Derived derived; derived.Tag = (byte)7;
                        Base& basePart = derived;
                        items[0].Part = basePart;
                        items[0].Id = 23ul;
                        byte* raw = &items[0].Part.Tag;
                        raw[1] = (byte)91;
                        Assign(items[0],items[0]);
                        Print(raw[1] == (byte)91);
                        Assign(items[1],items[0]);
                        byte* copied = &items[1].Part.Tag;
                        Print(copied[1] == (byte)0 && KindOf(items[1].Part) == 1);
                        Print(items[1].Part.Tag == (byte)7 && items[1].Id == 23ul);
                        ulong[] words = new ulong[4];
                        Pair& first = *((Pair*)&words[0]);
                        Pair& shifted = *((Pair*)&words[1]);
                        first.First = 11ul; first.Last = 22ul;
                        shifted = first;
                        Print(shifted.First == 11ul && shifted.Last == 22ul);
                        Owned a; a.Text = "saved"; Owned b = a; a.Text = "changed";
                        Print(b.Text);
                    }
                }
            )") == "true\ntrue\ntrue\ntrue\nsaved\n","class header, padding, alias or ownership mismatch");
        });
        test(prefix+"native-padded-buffer-snapshot",[&]{
            check(execute(R"(
                import Bolt.Runtime
                struct Padded { public: byte Kind; ulong Id; }
                static class Program { public: void Main() {
                    Padded[] input = new Padded[1];
                    input[0].Kind = (byte)3; input[0].Id = 25ul;
                    byte* bytes = (byte*)&input[0]; bytes[3] = (byte)77;
                    Buffer<Padded> items; items.AppendRange(input, 0, 1);
                    byte* copy = (byte*)&items[0]; Print(copy[3] == (byte)0);
                    Print(items[0].Kind == (byte)3 && items[0].Id == 25ul);
                } }
            )") == "true\ntrue\n","native padding normalization or live fields changed");
        });
        output("numeric-integer-limits", R"(
            Print(short.MinValue+","+short.MaxValue); Print(short.MinValue+","+short.MaxValue);
            Print(int.MinValue+","+int.MaxValue); Print(long.MinValue+","+long.MaxValue);
            Print(byte.MinValue+","+byte.MaxValue); Print(ushort.MinValue+","+ushort.MaxValue);
            Print(uint.MinValue+","+uint.MaxValue); Print(ulong.MinValue+","+ulong.MaxValue);
            Print(int.MinValue==int.MinValue && int.MaxValue==int.MaxValue);
            Print(uint.MinValue==uint.MinValue && uint.MaxValue==uint.MaxValue);
        )", "-32768,32767\n-32768,32767\n-2147483648,2147483647\n-9223372036854775808,9223372036854775807\n0,255\n0,65535\n0,4294967295\n0,18446744073709551615\ntrue\ntrue\n");
        output("numeric-floating-limits", R"(
            Print(Reinterpret<uint>(float.MinValue)); Print(Reinterpret<uint>(float.MaxValue));
            Print(Reinterpret<ulong>(double.MinValue)); Print(Reinterpret<ulong>(double.MaxValue));
        )", "4286578687\n2139095039\n18442240474082181119\n9218868437227405311\n");
        test(prefix+"numeric-limit-initializers",[&]{check(execute(R"(
            enum Long:long {First=long.MinValue,Last=long.MaxValue}
            enum Big:ulong {First=ulong.MinValue,Last=ulong.MaxValue}
            enum NearMax:int {Before=int.MaxValue-1,Last}
            class Limits {public:float Lowest=float.MinValue;double Highest=double.MaxValue;}
            static class Program {public:void Main(){
                Limits values;
                Print(Reinterpret<uint>(values.Lowest)); Print(Reinterpret<ulong>(values.Highest));
                Print(Long.First+","+Long.Last); Print(Big.First+","+Big.Last); Print(NearMax.Last);
            }}
        )") == "4286578687\n9218868437227405311\n-9223372036854775808,9223372036854775807\n0,18446744073709551615\n2147483647\n","numeric constant initializer mismatch");});
        test(prefix+"numeric-generic-limits",[&]{check(execute(R"(
            T Lowest<T>() : Number {return T.MinValue;}
            T Highest<T>() : Number {return T.MaxValue;}
            class Limits<T> {public:T Low=T.MinValue;T High=T.MaxValue;}
            static class Program {public:void Main(){
                Print(Lowest<short>()); Print(Highest<ulong>());
                Print(Reinterpret<uint>(Lowest<float>())); Print(Reinterpret<ulong>(Highest<double>()));
                Limits<short> values; Print(values.Low+","+values.High);
            }}
        )") == "-32768\n18446744073709551615\n4286578687\n9218868437227405311\n-32768,32767\n","generic numeric limits failed");});
        output("nan-payload-and-negative-zero","float n=Reinterpret<float>(2143289345u); Print(Reinterpret<uint>(n)); Print(n!=n); Print(n==n); Print(Reinterpret<uint>(Reinterpret<float>(2147483648u)));", "2143289345\ntrue\nfalse\n2147483648\n");
        output("print-normal-library","Print(); System.ConsoleHelpers.Print(7); Print<string>(\"text\");", "\n7\ntext\n");
        test(prefix+"print-shadowing",[&]{check(execute("static class Program {public:void Print(int x){Console.WriteLine(x+1);} void Main(){Print(3);}}") == "4\n","helper cannot be shadowed");});
        test(prefix+"generic-cast",[&]{check(execute("static class Program {public:T Convert<T>(int n){return (T)n;} void Main(){Print(Convert<double>(9));}}") == "9\n","generic cast failed");});
        test(prefix+"enum-bits",[&]{check(execute("enum Flags:ushort { All=65535 } static class Program {public:void Main(){short a=Reinterpret<short>(Flags.All); Flags b=Reinterpret<Flags>(a); Print(a);Print(b);}}") == "-1\n65535\n","enum representation wrong");});
        rejects("reject-bit-width",wrap("Print(Reinterpret<double>(1));"),"equally sized");
        rejects("reject-owning-bit-pun",wrap("Print(Reinterpret<ulong>(\"text\"));"),"equally sized");
        rejects("reject-bit-bool",wrap("Print(Reinterpret<bool>(byte(1)));"),"equally sized");
        rejects("reject-cast-type",wrap("Print((Missing)5);"),"unknown type");
        rejects("checked-cast",wrap("Print((short)40000);"),"out of range");
        test(prefix+"io-streams",[&]{check(execute(wrap("Print(Console.ReadLine());"),"custom stream\n")=="custom stream\n","streams ignored");});
        test(prefix+"compile-once-fresh-runs",[&]{auto opts=mode;std::ostringstream out;opts.output=&out;NativeProgram code(compile_source("static class Program {public:int N=0; int Main(){N++; Print(N);return N;}}"),opts);check(code.run()==1&&code.run()==1&&out.str()=="1\n1\n","module state leaked across fresh executions");});
        test(prefix+"mixed-register-and-stack-abi",[&]{
            const std::string source=R"(
                static class Program {public:
                    double Mix(int a,float b,int c,double d,int e,float f,int g,double h,int i,float j,int k,double l,int m,float n,int o,double p,int q,float r,int s,double t){return a+b+c+d+e+f+g+h+i+j+k+l+m+n+o+p+q+r+s+t;}
                    void Main(){Print(Mix(1,2f,3,4.0,5,6f,7,8.0,9,10f,11,12.0,13,14f,15,16.0,17,18f,19,20.0));}
                })";
            check(execute(source)=="210\n","mixed ABI argument spill mismatch");
        });
        test(prefix+"aggregate-return-reference-and-copy",[&]{
            const std::string source=R"(
              class Item { public: string Name; int X; Item(int n){X=n;Name="item";} }
              static class Program { public:
                Item Make(int n){return Item(n);} int& Ref(Item& i){return i.X;}
                void Main(){Item a=Make(7);Item b=a;Ref(b)=11;Print(a.Name+":"+a.X+","+b.X);}
              })";
            check(execute(source)=="item:7,11\n","aggregate/reference semantics mismatch");
        });
        test(prefix+"discarded-temporary-full-expression-lifetime",[&]{
            check(execute(R"(
                class Tracked {public:
                    static int Live=0;
                    Tracked(){Live++;}
                    ~Tracked(){Live--;}
                }
                static class Program {public:void Main(){
                    {Tracked retained;Tracked();Print(Tracked.Live);}
                    Print(Tracked.Live);
                }}
            )")=="1\n0\n","discarded temporary survived its statement or destroyed a retained local");
        });
        output("loop-lifetime-cleanup","for(int i=0;i<500;i++){string s=\"test\"+i; List<string> v;v.Add(s);if(i<499)continue;Print(v[0]);}","test499\n");
        output("jit-statements-call","Print(Runtime.ExecuteJit(\"Print(21*2);\"));","42\n0\n");
        output("jit-full-program-call","Print(Runtime.ExecuteProgramJit(\"static class Program {public:int Main(){return 17;}}\"));","17\n");
        rejects("array-bounds",wrap("int[] a=new int[2];Print(a[2]);"),"index out of range");
        test(prefix+"sequence-header-index-and-length",[&]{
            check(execute(R"BOLT(import System
import Bolt.Runtime
struct Row { public: long Id; string Name; }
static class Program {
public:
    int Replace(int[]& numbers) { numbers = {7, 13}; return 1; }
    void Main() {
        int[] sequence;
        Print(sequence.Length);
        sequence = {3};
        Print(sequence[Replace(sequence)]);
        int* p = &sequence[0]; *p = 17;
        Print(sequence[0]);
        for (int i = 0; i < 2; ++i) {
            Print(sequence.GetLength(0));
            sequence = default;
        }
        try { Print(sequence[0]); } catch (IndexOutOfRangeException e) { Print(71); }
        Buffer<string> names;
        names.Add("x"); names.Add("second");
        names[0] = "changed";
        Print(names[1]); Print(names.Count);
        try { Print(names[-1]); } catch (IndexOutOfRangeException e) { Print(73); }
        try { Print(sequence.GetLength(1)); } catch (IndexOutOfRangeException e) { Print(79); }
        Row[] rows = new Row[3];
        rows[1].Id = 12l; rows[1].Name = "middle";
        rows[2].Id = 46l; rows[2].Name = "last";
        Print(rows[1].Name); Print(rows[2].Id);
    }
}
)BOLT")=="0\n13\n17\n2\n0\n71\nsecond\n2\n73\n79\nmiddle\n46\n","unexpected sequence value or diagnostic");
        });
        rejects("null-check",wrap("int* p=null;Print(*p);"),"null pointer");
        rejects("checked-overflow",wrap("int n=2147483647;n++;"),"overflow");
        output("counted-conditional-steps","int n=0; for(int i=0;i<100;i++){if(i<10)n++;else n--;} Print(n);","-80\n");
        rejects("counted-step-at-limit",wrap("int n=2147483646; for(int i=0;i<4;i++){if(i<3)n++;else n--;} Print(n);"),"overflow");
        rejects("counted-step-after-alias",wrap("int n=0; int& r=n; int v=(r=2147483647); for(int i=0;i<3;i++)n++; Print(n);"),"overflow");
        test(prefix+"persistent-counted-loop-budget",[&] {
            const auto compare=[&](const std::string& statements,std::size_t budget) {
                const auto source="public static class Program { public: int Main(){"+statements+"} }";
                auto p=compile_source(source,"loop.bolt",true);
                auto result=[&](bool persistent) {
                    auto options=mode;
                    options.execution.max_steps=budget;
                    NativeProgram image(p,options);
                    try { return std::to_string(persistent?image.run_persistent():image.run()); }
                    catch(const Error& e) { return std::string(e.what()); }
                };
                check(result(true)==result(false),"persistent loop result or diagnostic differs at budget "+std::to_string(budget));
            };
            const std::string loop="for(int i=3;i<28;i++) x=(x*7ul+13ul)%251ul; return (int)x;";
            for(auto budget:{12u,26u,27u,28u,100u}) compare("ulong x=9ul;"+loop,budget);
            compare("ulong x=9876543210ul;"+loop,100);
            compare("ulong x=18446744073709551615ul;"+loop,100);
            compare("ulong x=3ul;for(int i=0;i<21;i++) x=(x*33ul)&127ul;return (int)x;",30);
            compare("int x=1;try {for(int i=0;i<40;i++) x=x*2;} catch {return x/1073741824;} return 0;",50);
        });
        test(prefix+"step-budget",[&]{auto opts=mode;opts.execution.max_steps=100;try{jit_program(wrap("while(true){}"),opts);}catch(const Error& e){check(std::string(e.what()).find("budget")!=std::string::npos,"wrong budget error");return;}throw std::runtime_error("step budget was ignored");});
        test(prefix+"call-depth-budget",[&]{auto opts=mode;opts.execution.max_call_depth=12;try{jit_program("static class Program {public:int Recurse(){return Recurse();} void Main(){Recurse();}}",opts);}catch(const Error& e){check(std::string(e.what()).find("depth")!=std::string::npos,"wrong call-depth error");return;}throw std::runtime_error("call-depth budget was ignored");});
        test(prefix+"source-diagnostic",[&]{auto opts=mode;auto source=std::string("static class Program {\npublic:\nvoid Main(){\nint zero=0;\nint result=1/zero;\n}\n}\n");try{NativeProgram executable(compile_source(source,"precise.bolt"),opts);executable.run();}catch(const Error& e){check(e.diagnostic.location.file=="precise.bolt"&&e.diagnostic.location.line==5&&!e.diagnostic.source_line.empty(),"missing original source line");return;}throw std::runtime_error("division did not fail");});
    }
    test("invalid-backend-is-not-silent-fallback",[]{JitOptions opts;opts.backend="not-a-backend";try{jit_program(wrap("Print(1);"),opts);}catch(const std::runtime_error& e){check(std::string(e.what()).find("backend")!=std::string::npos,"wrong backend error");return;}throw std::runtime_error("invalid backend silently accepted");});
    test("print-is-library-method-not-intrinsic",[]{auto program=compile_source(wrap("Print(1);"));auto* helper=program.find_class("System.ConsoleHelpers");check(helper&&helper->methods.size()>=2,"ordinary library implementation missing");});
    std::cout<<passed<<" native unit checks passed, "<<failed<<" failed\n";
    return failed?1:0;
}
