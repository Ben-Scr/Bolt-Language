#include <bolt/Bolt.hpp>
#include <bolt/Native.hpp>
#include <bolt/library/Exceptions.hpp>
#include "../src/tooling/Json.hpp"
#include "ArenaCppOwnership.hpp"
#include "ArenaValueOwnership.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {
#include "FeatureCases.hpp"
#include "VariadicGenericCases.hpp"
#include "GenericConstantCases.hpp"
#include "ThisCases.hpp"
#include "IterationCases.hpp"
#include "ExtensionMethodCases.hpp"
#include "DefaultCases.hpp"
#include "ArenaCases.hpp"
#include "NumericParsingCases.hpp"
#include "RectArrayCases.hpp"
#include "SizeOfFormattingCases.hpp"
int passed = 0, failed = 0;
std::string wrap(const std::string& body) { return "static class Program { public: void Main() { " + body + " } }"; }
std::string run(const std::string& source, const std::string& input = {}, bolt::ExecutionOptions options = {}) {
    std::istringstream in(input); std::ostringstream out;
    bolt::Runtime runtime(bolt::compile_source(source, "test.bolt"), options, &in, &out);
    if (runtime.run() != 0) throw std::runtime_error("unexpected nonzero exit");
    return out.str();
}
void check(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
void test(const std::string& name, const std::function<void()>& function) {
    try { function(); ++passed; std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
}
void output(const std::string& name, const std::string& body, const std::string& expected) {
    test(name, [&] { const auto actual = run(wrap(body)); check(actual == expected, "expected [" + expected + "], got [" + actual + "]"); });
}
void rejects(const std::string& name, const std::string& source, const std::string& fragment, bool execute = false,
             bolt::ExecutionOptions options = {}) {
    test(name, [&] {
        try {
            if (execute) (void)run(source, {}, options);
            else (void)bolt::compile_source(source, "invalid.bolt");
        } catch (const std::exception& error) {
            check(std::string(error.what()).find(fragment) != std::string::npos,
                "wrong diagnostic: " + std::string(error.what()));
            return;
        }
        throw std::runtime_error("expected failure containing: " + fragment);
    });
}
} // namespace
int main() {
    for(const auto& feature:iteration_cases) {
        if(*feature.error)rejects(feature.name,feature.source,feature.error);
        else if(!feature.interpret)test(feature.name,[&]{(void)bolt::compile_source(feature.source,"iteration.bolt");});
        else test(feature.name,[&]{const auto actual=run(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
    }
    for(const auto& feature:extension_method_cases) {
        if(*feature.error)rejects(feature.name,feature.source,feature.error);
        else test(feature.name,[&]{const auto actual=run(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
    }
    for(const auto& feature:variadic_generic_cases) {
        if(*feature.error)rejects(feature.name,feature.source,feature.error);
        else test(feature.name,[&]{const auto actual=run(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
    }
    test("pointer-address-output",[]{check(correct_pointer_output(run(pointer_output_source)),"pointer addresses/value/reference formatting mismatch");});
    for(const auto& feature:sizeof_formatting_cases) {
        if(*feature.error)rejects(feature.name,feature.source,feature.error);
        else test(feature.name,[&]{const auto actual=run(feature.source);check(actual==feature.expected,"expected ["+std::string(feature.expected)+"], got ["+actual+"]");});
    }
    for(const auto& feature:numeric_parsing_cases()) {
        if(!feature.error.empty())rejects(feature.name,feature.source,feature.error,feature.execute);
        else test(feature.name,[&]{
            const auto actual=run(feature.source);
            check(actual==feature.expected,"expected ["+feature.expected+"], got ["+actual+"]");
        });
    }
    test("cpp-arena-owned-storage", [] { bolt::tests::verify_cpp_arena_ownership(); });
    test("interpreter-arena-owned-storage", [] { bolt::tests::verify_value_arena_ownership(); });
    test("interpreter-arena-lifecycle", [] { bolt::tests::verify_value_arena_lifecycle(); });
    test("arena-front-end", [] {
        auto program=bolt::compile_source(R"(
            T* MakeIn<T>(Arena& destination,T value){return new(destination) T(value);}
            static class Program {public:void Main(){
                Arena work=Arena.Fixed(4096);
                var growing=Arena.Growable(1024,8192);
                int* value=MakeIn(work,7);
                ulong used=work.UsedBytes;
                ulong capacity=work.CapacityBytes;
                ulong peak=work.PeakBytes;
                ulong limit=work.LimitBytes;
                work.Reset();work.Dispose();
            }}
        )", "arena-front-end.bolt");
        auto* global=program.find_class("@Global");
        check(global!=nullptr,"generic arena helper was not parsed");
        bool placement=false;
        for(const auto& method:global->methods)if(method.is_specialization&&method.name=="MakeIn") {
            const auto& expression=*method.body->statements[0]->expression;
            placement=expression.arena&&expression.arena->type=="Arena"&&expression.text=="int";
        }
        check(placement,"generic cloning/substitution lost the arena receiver");
    });
    test("arena-borrowed-pointer-field-can-escape", [] {
        (void)bolt::compile_source(R"(
            class Holder {public:int* Borrowed;}
            int* Extract(int* external){Arena work=Arena.Fixed(4096);Holder* holder=new(work) Holder();holder->Borrowed=external;return holder->Borrowed;}
            static class Program {public:void Main(){}}
        )", "arena-borrowed-field.bolt");
    });
    rejects("arena-default-owner",wrap("Arena work;"),"direct initialization");
    rejects("arena-copy-owner",wrap("Arena work=Arena.Fixed(4096);Arena copy=work;"),"cannot assign");
    rejects("arena-cast-owner",wrap("Arena work=Arena.Fixed(4096);Arena copy=(Arena)work;"),"cannot be cast or copied");
    rejects("arena-bitcast-owner",wrap("Arena work=Reinterpret<Arena>(0ul);"),"equally sized");
    rejects("arena-inferred-copy-owner",wrap("Arena work=Arena.Fixed(4096);var copy=work;"),"cannot assign");
    rejects("arena-assign-owner",wrap("Arena work=Arena.Fixed(4096);work=work;"),"cannot be copied");
    rejects("arena-temporary-factory",wrap("Arena.Fixed(4096);"),"direct initialization");
    rejects("arena-nested-factory",wrap("Arena work=(Arena.Fixed(4096),Arena.Fixed(8192));"),"direct initialization");
    rejects("arena-noninteger-budget",wrap("Arena work=Arena.Fixed(2.5);"),"must be an integer");
    rejects("arena-local-borrow",wrap("Arena work=Arena.Fixed(4096);Arena& alias=work;"),"only as method parameters");
    rejects("arena-pointer-owner",wrap("Arena* work;"),"cannot be pointer targets");
    rejects("arena-address-owner",wrap("Arena work=Arena.Fixed(4096);var address=&work;"),"cannot be copied or addressed");
    rejects("arena-array-owner",wrap("Arena[] work;"),"cannot be pointer targets or array elements");
    rejects("arena-new-owner",wrap("var work=new Arena();"),"directly initialized as local variables");
    rejects("arena-placement-array",wrap("Arena work=Arena.Fixed(4096);var values=new(work) int[4];"),"placement array allocation");
    rejects("arena-placement-nonowner",wrap("int work=4096;var value=new(work) int();"),"requires a mutable Arena");
    rejects("arena-const-mutation",wrap("const Arena work=Arena.Fixed(4096);work.Reset();"),"const Arena");
    rejects("arena-value-parameter","void Use(Arena work){} "+wrap(""),"must borrow an owner");
    rejects("arena-value-return","Arena Make(){return Arena.Fixed(4096);} "+wrap(""),"cannot be returned");
    rejects("arena-borrow-return","Arena& Borrow(Arena& work){return work;} "+wrap(""),"cannot be returned");
    rejects("arena-factory-argument","void Use(Arena& work){} "+wrap("Use(Arena.Fixed(4096));"),"direct initialization");
    rejects("arena-field-owner","class Holder {public:Arena Work;} "+wrap(""),"cannot be stored in fields");
    rejects("arena-collection-owner","import System.Collections.Generic\n"+wrap("List<Arena> values;"),"cannot be collection elements");
    rejects("arena-local-pointer-return","int* Escape(){Arena work=Arena.Fixed(4096);int* value=new(work) int(7);return value;} "+wrap(""),"into a local Arena");
    rejects("arena-local-reference-return","int& Escape(){Arena work=Arena.Fixed(4096);int* value=new(work) int(7);return *value;} "+wrap(""),"into a local Arena");
    output("hello", "Console.WriteLine(\"Hello World!\");", "Hello World!\n");
    test("read-line", [] { check(run(wrap("Console.WriteLine(Console.ReadLine());"), "Bolt\n") == "Bolt\n", "input mismatch"); });
    output("arithmetic", "int x=1; x+=4; x-=1; x*=3; x/=2; x%=4; Console.WriteLine(x);", "2\n");
    output("var-basic", "var value=0; value+=4; Print(value); var text=\"Bolt\"; text+=\"!\"; Print(text); var flag=value==4; Print(flag);", "4\nBolt!\ntrue\n");
    test("var-inferred-types", [] {
        check(run(R"(
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
        )") == "int,long,uint,ulong\nfloat,double,bool,string\nshort,int\n2\nint\n", "var did not preserve the initializer's static type");
    });
    output("var-loop-and-scope", "var total=0; for(var i=0;i<4;i++){total+=i;} {var total=total+1;Print(total);} Print(total);", "7\n6\n");
    output("const-var", "const var value=7; var copy=value; copy++; Print(value);Print(copy);", "7\n8\n");
    test("var-calls-and-value-copies", [] {
        check(run(R"(
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
        )") == "12,9,11,8\n", "var call, class copy, reference read, or pointer inference failed");
    });
    test("var-arrays-and-collections", [] {
        check(run(R"(
            import System.Collections.Generic
            static class Program {public:void Main(){
                var values={1,2,3}; var copied=values; copied[0]=9;
                var allocated=new int[2]; allocated[1]=7;
                var nested={{4,5},{6}};
                List<int> source; source.Add(3); var list=source; list[0]=8;
                var created=List<int>(); created.Add(11);
                Print(values[0]+","+copied[0]+","+allocated[1]+","+nested[0][1]);
                Print(source[0]+","+list[0]+","+created[0]);
            }}
        )") == "1,9,7,5\n3,8,11\n", "var array or collection inference/copy failed");
    });
    test("var-preserves-resolved-namespace", [] {
        auto program=bolt::parse(R"(
            import System.Collections.Generic
            class Item {public:int X;Item(int value){X=value;}}
            static class Factory {public:
                Item Make(){return Item(7);}
                Item[] MakeArray(){return {Item(8)};}
                List<Item> MakeList(){List<Item> values;values.Add(Item(9));return values;}
            }
        )", "inferred-global.bolt");
        auto consumer=bolt::parse(R"(
            namespace Client;
            class Item {public:int Other;}
            static class Program {public:void Main(){
                var item=Factory.Make();var array=Factory.MakeArray();var list=Factory.MakeList();
                Print(item.X+","+array[0].X+","+list[0].X);
            }}
        )", "inferred-consumer.bolt");
        for(auto& c:consumer.classes)program.classes.push_back(std::move(c));
        program.sources.insert(consumer.sources.begin(),consumer.sources.end());
        bolt::analyze(program);
        std::istringstream in;std::ostringstream out;
        bolt::Runtime runtime(std::move(program),{},&in,&out);
        check(runtime.run()==0&&out.str()=="7,8,9\n","var re-resolved a canonical initializer type in the caller's namespace");
    });
    output("increment-order", "int i=1; Console.WriteLine(i++); Console.WriteLine(++i); Console.WriteLine(i--); Console.WriteLine(--i);", "1\n3\n3\n1\n");
    output("comma", "int x=0; x=1, x+=2, x*=3; Console.WriteLine(x);", "9\n");
    output("precedence", "Console.WriteLine(2+3*4); Console.WriteLine((2+3)*4);", "14\n20\n");
    output("short-circuit", "int i=0; bool a=false && (++i > 0); bool b=true || (++i > 0); Console.WriteLine(i); Console.WriteLine(!a && b);", "0\ntrue\n");
    output("boolean-condition-example", "int value=10; if(value > 0 && (value < 5 || value == 10)) Console.WriteLine(\"yes\");", "yes\n");
    output("elif", "int x=2; if(x==0) Console.WriteLine(0); elif(x==1) Console.WriteLine(1); elif(x==2) Console.WriteLine(2); else Console.WriteLine(3);", "2\n");
    output("for-while-break-continue", "int n=0; for(int i=0;i<6;i++){if(i==2)continue; if(i==5)break; n+=i;} while(n<10)n++; Console.WriteLine(n);", "10\n");
    output("for-multiple-updates", "int n=0; for(int i=0;i<3;i++,n+=2){} Console.WriteLine(n);", "6\n");
    output("scope-shadowing", "int x=7; {int x=x+1; Console.WriteLine(x);} Console.WriteLine(x);", "8\n7\n");
    output("string-concat", "float x=5; float y=10; Console.WriteLine(x+\", \"+y); string s=\"flag=\"; s+=true; Console.WriteLine(s);", "5, 10\nflag=true\n");
    output("all-integrals", "short a=-128; short b=-32768; int c=2147483647; long d=-9223372036854775808; byte e=255; ushort f=65535; uint g=4294967295u; ulong h=18446744073709551615ul; Console.WriteLine(a+\",\"+b+\",\"+c+\",\"+d); Console.WriteLine(e+\",\"+f+\",\"+g+\",\"+h);", "-128,-32768,2147483647,-9223372036854775808\n255,65535,4294967295,18446744073709551615\n");
    output("floating-and-casts", "float a=1.5f; double b=2.5; Console.WriteLine(a+b); Console.WriteLine(int(-2.9)); Console.WriteLine(string(true));", "4\n-2\ntrue\n");
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
    test("numeric-limit-types", [] {
        check(run(R"(
            import Type
            string Name<T>(T value){return Type.Name<T>();}
            static class Program {public:void Main(){
                Print(Name(short.MinValue)+","+Name(short.MaxValue));
                Print(Name(short.MinValue)+","+Name(short.MaxValue));
                Print(Name(int.MinValue)+","+Name(int.MaxValue));
                Print(Name(long.MinValue)+","+Name(long.MaxValue));
                Print(Name(byte.MinValue)+","+Name(byte.MaxValue));
                Print(Name(ushort.MinValue)+","+Name(ushort.MaxValue));
                Print(Name(uint.MinValue)+","+Name(uint.MaxValue));
                Print(Name(ulong.MinValue)+","+Name(ulong.MaxValue));
                Print(Name(float.MinValue)+","+Name(float.MaxValue));
                Print(Name(double.MinValue)+","+Name(double.MaxValue));
                Print(Name(int.MaxValue)+","+Name(uint.MaxValue));
            }}
        )") == "short,short\nshort,short\nint,int\nlong,long\nbyte,byte\nushort,ushort\nuint,uint\nulong,ulong\nfloat,float\ndouble,double\nint,uint\n", "numeric constant lost its owning type");
    });
    test("numeric-limit-initializers", [] {
        check(run(R"(
            enum Byte:byte {First=byte.MinValue,Next,Last=byte.MaxValue}
            enum Long:long {First=long.MinValue,Last=long.MaxValue}
            enum Big:ulong {First=ulong.MinValue,Last=ulong.MaxValue}
            enum NearMax:int {Before=int.MaxValue-1,Last}
            class Limits {public:
                int Small=short.MinValue+1;
                static ulong Largest=ulong.MaxValue;
                float Lowest=float.MinValue;
                double Highest=double.MaxValue;
            }
            static class Program {public:void Main(){
                Limits values;
                Print(values.Small); Print(Limits.Largest);
                Print(Reinterpret<uint>(values.Lowest)); Print(Reinterpret<ulong>(values.Highest));
                Print(Byte.First+","+Byte.Next+","+Byte.Last);
                Print(Long.First+","+Long.Last); Print(Big.First+","+Big.Last); Print(NearMax.Last);
            }}
        )") == "-32767\n18446744073709551615\n4286578687\n9218868437227405311\n0,1,255\n-9223372036854775808,9223372036854775807\n0,18446744073709551615\n2147483647\n", "numeric constant initializer mismatch");
    });
    test("numeric-generic-limits", [] {
        check(run(R"(
            T Lowest<T>() : Number {return T.MinValue;}
            T Highest<T>() : Number {return T.MaxValue;}
            class Limits<T> {public:T Low=T.MinValue;T High=T.MaxValue;}
            static class Program {public:void Main(){
                Print(Lowest<short>()); Print(Highest<ulong>());
                Print(Reinterpret<uint>(Lowest<float>())); Print(Reinterpret<ulong>(Highest<double>()));
                Limits<short> values; Print(values.Low+","+values.High);
            }}
        )") == "-32768\n18446744073709551615\n4286578687\n9218868437227405311\n-32768,32767\n", "generic numeric limits failed");
    });
    output("promotions", "uint a=4294967295u; int b=-1; Console.WriteLine(a+b); short c=127; Console.WriteLine(c+1);", "4294967294\n128\n");
    output("defaults", "int i; float f; double d; bool b; string s; Console.WriteLine(i+\",\"+f+\",\"+d+\",\"+b+\",\"+s);", "0,0,0,false,\n");
    output("negative-remainder", "Console.WriteLine(-7%3); Console.WriteLine(-9223372036854775808%-1l);", "-1\n0\n");
    output("compound-evaluation-order", "int x=1; x += (x=3); Console.WriteLine(x); int y=0; Console.WriteLine(y++ + y++);", "4\n1\n");
    output("utf8-and-escape", "Console.WriteLine(\"Bolt: \\t\\\"hi\\\"\\n\");", "Bolt: \t\"hi\"\n\n");
    test("embedded-null", [] { check(run(wrap("Console.Write(\"a\\0b\");")) == std::string("a\0b",3), "NUL was lost"); });
    test("class-copies-and-private-fields", [] {
        check(run(R"(
            public class Vec2 { public: float X; float Y; void Move(){X++;} }
            public class Holder { public: Vec2 V; const int Fixed=3; }
            public class Example { public: int Count(){return iterations;} private: int iterations=100; }
            static class Program { public: void Mutate(Vec2 x){x.X=99;} void Main(){
                Vec2 a; a.X=5; a.Y=10; Vec2 b=a; b.Move(); Mutate(a);
                Holder h; h.V=a; Holder copy=h; copy.V.X=33; h=copy; copy.V.X=44;
                Example e; Console.WriteLine(a.X+","+b.X+","+h.V.X+","+h.Fixed+","+e.Count());
            } }
        )") == "5,6,33,3,100\n", "value semantics mismatch");
    });
    test("static-members-and-recursion", [] {
        check(run(R"(
            class Counter { public: static int Count=3; static void Add(){Count++;} }
            static class Program { public: int Fib(int n){if(n<=1)return n; return Fib(n-1)+Fib(n-2);}
              void Main(){Counter.Add(); Console.WriteLine(Counter.Count); Console.WriteLine(Fib(10));} }
        )") == "4\n55\n", "static/recursion mismatch");
    });
    test("parent-assignment-keeps-field-alive", [] {
        check(run(R"(
            class Item { public: int X; }
            class Box { public: Item V; }
            static class Program { public: void Main(){ Box a; Box b; b.V.X=3;
                a.V.X=(a=b,7); Console.WriteLine(a.V.X);
            } }
        )") == "7\n", "parent reassignment mismatch");
    });
    test("object-return-and-temporary-receiver", [] {
        check(run(R"(
            class Item { public: int X; int Get(){return X;} }
            static class Program { public: Item Make(int x){Item i;i.X=x;return i;}
                void Main(){Console.WriteLine(Make(7).Get()); Console.WriteLine(Make(8).X);} }
        )") == "7\n8\n", "return mismatch");
    });
    output("runtime-statements", "Runtime.Execute(\"int n=3; Console.WriteLine(n*7);\");", "21\n");
    output("runtime-program", "Console.WriteLine(Runtime.ExecuteProgram(\"static class Program {public: int Main(){return 42;}}\"));", "42\n");
    test("entry-exit-code", [] { bolt::Runtime rt(bolt::compile_source("static class Program { public: int Main(){return 7;} }")); check(rt.run()==7,"wrong exit"); });
    test("embedding-static-state", [] {
        auto p=bolt::compile_source("static class API {public: int Count=0; int Next(){return ++Count;}}", "host.bolt", false);
        bolt::Runtime runtime(std::move(p));
        check(runtime.call("API","Next").as_int()==1, "first call");
        check(runtime.call("API","Next").as_int()==2, "state not preserved");
    });
    test("embedding-instance-copy", [] {
        bolt::Runtime runtime(bolt::compile_source("class Item {public:int X;void Set(int n){X=n;}}", "host.bolt", false));
        auto a=runtime.create("Item"); auto b=a;
        runtime.call(b,"Set",{bolt::Value(std::int32_t{9})});
        check(a.object().fields.at("X").as_int()==0 && b.object().fields.at("X").as_int()==9,"copy aliases");
    });
    rejects("unknown-name",wrap("Console.WriteLine(missing);"),"unknown identifier");
    rejects("unknown-type",wrap("Number x;"),"unknown type");
    rejects("var-needs-initializer",wrap("var value;"),"initializer");
    rejects("var-cannot-infer-null",wrap("var value=null;"),"null");
    rejects("var-cannot-infer-void",wrap("var value=Console.WriteLine(1);"),"expected a value");
    rejects("var-cannot-infer-type","class Item {} "+wrap("var value=Item;"),"expected a value");
    rejects("var-cannot-infer-method","static class Program {public:int Value(){return 1;} void Main(){var value=Value;}}","method");
    rejects("var-cannot-infer-empty-array",wrap("var values={};"),"empty array literal");
    rejects("var-assignment-keeps-type",wrap("var value=0;value=\"text\";"),"cannot assign");
    rejects("const-var-write",wrap("const var value=1;value++;"),"const");
    rejects("var-self-initialization",wrap("var value=value;"),"unknown identifier");
    rejects("var-loop-scope",wrap("for(var i=0;i<1;i++){} Print(i);"),"unknown identifier");
    rejects("var-field","class Item {public:var Value=1;} "+wrap(""),"var");
    rejects("var-parameter","static class Program {public:void Use(var value){} void Main(){}}","var");
    rejects("var-return-type","static class Program {public:var Value(){return 1;} void Main(){}}","var");
    rejects("var-pointer-suffix",wrap("int value=1;var* pointer=&value;"),"var");
    rejects("var-reference-suffix",wrap("int value=1;var& reference=value;"),"var");
    rejects("var-array-suffix",wrap("var[] values={1,2};"),"var");
    rejects("const-write",wrap("const int x=1; x++;"),"const");
    rejects("const-needs-init",wrap("const int x;"),"initializer");
    rejects("numeric-condition",wrap("if(1){}"),"bool");
    rejects("mixed-ulong-signed",wrap("ulong a=1ul; Console.WriteLine(a+1);"),"explicit cast");
    rejects("literal-narrowing",wrap("short a=32768;"),"out of range");
    rejects("numeric-limit-write",wrap("int.MaxValue=1;"),"assignment requires");
    rejects("numeric-limit-increment",wrap("byte.MinValue++;"),"assignment requires");
    rejects("numeric-limit-compound-write",wrap("double.MaxValue-=1.0;"),"assignment requires");
    rejects("numeric-limit-address",wrap("int* value=&int.MaxValue;"),"lvalue");
    rejects("numeric-limit-reference",wrap("int& value=int.MinValue;"),"lvalue");
    rejects("numeric-limit-call",wrap("Print(int.MaxValue());"),"not callable");
    rejects("numeric-limit-unknown",wrap("Print(int.Maximum);"),"member");
    rejects("numeric-limit-bool",wrap("Print(bool.MaxValue);"),"unknown identifier");
    rejects("numeric-limit-string",wrap("Print(string.MinValue);"),"unknown identifier");
    rejects("numeric-limit-instance",wrap("int value=1;Print(value.MaxValue);"),"member");
    rejects("numeric-limit-arrow",wrap("Print(int->MaxValue);"),"access numeric type members with .");
    rejects("numeric-limit-narrowing",wrap("short value=int.MaxValue;"),"out of range");
    rejects("numeric-limit-enum-overflow","enum E:byte{Last=byte.MaxValue,Overflow} "+wrap(""),"out of range");
    rejects("private-field","class Item {int x;} static class Program {public:void Main(){Item x;Console.WriteLine(x.x);}}","private");
    rejects("private-method","class Item {void Secret(){}} static class Program {public:void Main(){Item x;x.Secret();}}","private");
    rejects("recursive-layout","class Item {public:Item Self;} "+wrap(""),"recursive by-value");
    rejects("static-instance","static class Item {} "+wrap("Item value;"),"static class");
    rejects("wrong-argument-count","static class Program {public:void F(int n){} void Main(){F();}}","argument count");
    rejects("bad-return","static class Program {public:int F(){if(true)return 1;} void Main(){}}","not all paths");
    rejects("missing-entry","class Item {}","entry class");
    rejects("instance-entry","class Program {public:void Main(){}}","entry must");
    rejects("break-outside-loop-or-switch",wrap("break;"),"break requires a loop or switch");
    rejects("void-field","class Item {void Value;} "+wrap(""),"unknown type");
    rejects("impure-field-init","class Item {int Value=Console.ReadLine();} "+wrap(""),"cannot assign");
    rejects("field-references","class Item {int A=1;int B=A;} "+wrap(""),"field initializers");
    rejects("float-remainder",wrap("Console.WriteLine(3.0%2);"),"requires integer");
    rejects("class-format","class Item {} "+wrap("Item x;Console.WriteLine(x);"),"IStringable<Item>");
    rejects("const-object","class Item {public:int X;} "+wrap("const Item x=Item();x.X=2;"),"const");
    rejects("unterminated-string",wrap("Console.WriteLine(\"broken);"),"unterminated string");
    rejects("unterminated-comment",wrap("/* broken"),"unterminated block comment");
    rejects("division-zero",wrap("int n=0;Console.WriteLine(1/n);"),"division by zero",true);
    rejects("overflow-add",wrap("int n=2147483647;n++;"),"overflow",true);
    rejects("overflow-multiply",wrap("long n=9223372036854775807;n*=2;"),"overflow",true);
    rejects("overflow-negate",wrap("long n=-9223372036854775808;Console.WriteLine(-n);"),"overflow",true);
    rejects("overflow-divide",wrap("long n=-9223372036854775808;Console.WriteLine(n/-1l);"),"overflow",true);
    rejects("unsigned-underflow",wrap("ulong n=0ul;n--;"),"underflow",true);
    rejects("narrowing-variable",wrap("int n=32768;short small=n;"),"out of range",true);
    rejects("float-to-ulong-boundary",wrap("double n=18446744073709551616.0;ulong v=n;"),"out of range",true);
    rejects("runtime-context-isolation",wrap("int outer=3;Runtime.Execute(\"Console.WriteLine(outer);\");"),"unknown identifier",true);
    rejects("step-budget",wrap("for(;;){}"),"budget",true,{100,128,16});
    rejects("call-depth","static class Program{public:void F(){F();}void Main(){F();}}","call-depth",true,{100000,5,16});
    rejects("nested-shared-budget",wrap("for(int i=0;i<100;i++){Runtime.Execute(\"Console.WriteLine(1);\");}"),"budget",true,{100,128,16});
    rejects("runtime-nesting-limit",wrap("Runtime.Execute(\"Runtime.Execute(\\\"Console.WriteLine(1);\\\");\");"),"nesting limit",true,{100000,128,1});
    test("checked-long-multiplication-boundaries", [] {
        using T=std::int64_t; const T lo=std::numeric_limits<T>::min();
        check(bolt::native::mul<T>(lo,1)==lo,"min*1 failed");
        check(bolt::native::mul<T>(T(-3),T(-7))==21,"negative product failed");
        try {(void)bolt::native::mul<T>(lo,T(-1));} catch(const std::runtime_error&){return;}
        throw std::runtime_error("min*-1 did not fail");
    });
    test("embedding-all-integer-constructors", [] {
        std::vector<bolt::Value> values{bolt::Value(std::int16_t{-1}), bolt::Value(std::int16_t{-2}),
            bolt::Value(std::uint8_t{255}), bolt::Value(std::uint16_t{65535}),
            bolt::Value(std::uint32_t{4294967295u})};
        check(values[0].type=="short" && values[1].type=="short" && values[2].type=="byte" &&
              values[3].type=="ushort" && values[4].type=="uint", "host value type lost");
        check(values[4].to_string()=="4294967295", "uint host value changed");
    });
    test("reject-nonfinite-host-values", [] {
        try { (void)bolt::convert_value("float", bolt::Value(std::numeric_limits<float>::infinity())); }
        catch (const std::runtime_error&) { return; }
        throw std::runtime_error("non-finite host value was accepted");
    });
    for (const auto& feature : feature_cases) {
        if (*feature.error) {
            rejects(feature.name, feature.source, feature.error, !feature.compile_error);
        } else {
            test(feature.name, [&] {
                const auto actual = run(feature.source);
                check(actual == feature.expected, "expected [" + std::string(feature.expected) + "], got [" + actual + "]");
            });
        }
    }
    for (const auto& feature : generic_constant_cases) {
        if (*feature.error) rejects(feature.name, feature.source, feature.error);
        else test(feature.name, [&] {
            const auto actual = run(feature.source);
            check(actual == feature.expected, "expected [" + std::string(feature.expected) + "], got [" + actual + "]");
        });
    }
    for (const auto& feature : this_cases) {
        if (*feature.error) rejects(feature.name, feature.source, feature.error);
        else test(feature.name, [&] {
            const auto actual = run(feature.source);
            check(actual == feature.expected, "expected [" + std::string(feature.expected) + "], got [" + actual + "]");
        });
    }
    for (const auto& feature : default_cases) {
        if (*feature.error) rejects(feature.name, feature.source, feature.error);
        else test(feature.name, [&] {
            const auto actual = run(feature.source);
            check(actual == feature.expected, "expected [" + std::string(feature.expected) + "], got [" + actual + "]");
        });
    }
    for (const auto& feature : arena_cases) {
        if (*feature.error) rejects(feature.name, feature.source, feature.error, true);
        else test(feature.name, [&] {
            const auto actual = run(feature.source);
            check(actual == feature.expected, "expected [" + std::string(feature.expected) + "], got [" + actual + "]");
        });
    }
    for (const auto& feature : rect_array_cases) {
        if (*feature.error) rejects(feature.name, feature.source, feature.error, feature.runtime_error);
        else test(feature.name, [&] {
            const auto actual = run(feature.source);
            check(actual == feature.expected, "expected [" + std::string(feature.expected) + "], got [" + actual + "]");
        });
    }
    test("this-context-diagnostic-location", [] {
        try {
            (void)bolt::compile_source("static class Program {public:void Main(){\nthis;\n}}", "this-location.bolt");
        } catch (const bolt::Error& error) {
            check(error.location.file == "this-location.bolt" && error.location.line == 2 &&
                  error.location.column == 1 && error.location.length == 4 && error.diagnostic.code == "BLT2000",
                  "this diagnostic did not identify the keyword");
            return;
        }
        throw std::runtime_error("expected this context diagnostic");
    });
    test("generic-constant-parser-keeps-relational-chain", [] {
        auto program = bolt::parse(wrap("Print(a < b > c);"));
        const auto& call = program.classes[0].methods[0].body->statements[0]->expression;
        check(call && call->kind == bolt::Expr::Kind::Call && call->args.size() == 2, "comparison call was not parsed");
        const auto& expression = *call->args[1];
        check(expression.kind == bolt::Expr::Kind::Binary && expression.text == ">", "greater-than parsed as generic arguments");
        check(expression.args[0]->kind == bolt::Expr::Kind::Binary && expression.args[0]->text == "<", "less-than parsed as generic arguments");
        check(expression.args[0]->args[0]->text == "a" && expression.args[0]->args[1]->text == "b" && expression.args[1]->text == "c", "relational operands changed");
    });
    test("json-roundtrip-nested", [] {
        using J=bolt::tooling::Json;
        auto value=J::parse(R"({"array":[1,true,null,"hello"],"n":-2.5,"object":{"key":"value"}})");
        check(value["array"].array().size()==4,"array lost");
        check(J::parse(value.dump()).dump()==value.dump(),"roundtrip differs");
    });
    test("json-unicode-and-escaping", [] {
        using J=bolt::tooling::Json;
        auto value=J::parse(R"({"s":"\uD83D\uDE00\n\t\"\\\u0000"})");
        auto round=J::parse(value.dump());
        check(round["s"].string()==value["s"].string(),"Unicode roundtrip differs");
        check(value["s"].string().size()==9,"unexpected escaped UTF-8 length");
    });
    test("json-reject-invalid-syntax", [] {
        for(const std::string text:{"", "01", "-01", "1.", "1e", "NaN", "[1,]", "{\"a\":1,}", "true false", "{\"a\":1,\"a\":2}", "1e9999"}) {
            bool threw=false;
            try {(void)bolt::tooling::Json::parse(text);}catch(const std::exception&){threw=true;}
            check(threw,"accepted invalid JSON: "+text);
        }
    });
    test("json-reject-invalid-utf8-and-surrogates", [] {
        for(const std::string text:std::vector<std::string>{"\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", std::string("\"\xc0\x80\""), std::string("\"\xed\xa0\x80\"")}) {
            bool threw=false;
            try {(void)bolt::tooling::Json::parse(text);}catch(const std::exception&){threw=true;}
            check(threw,"accepted invalid Unicode");
        }
    });
    test("json-depth-bound", [] {
        bool threw=false;
        try {(void)bolt::tooling::Json::parse(std::string(120,'[')+"0"+std::string(120,']'));}
        catch(const std::exception&){threw=true;}
        check(threw,"deep JSON did not hit the limit");
    });
    test("diagnostic-renderers-escape-messages", [] {
        bolt::Error error({"test name.bolt",2,3,4},"expected \"value\"\nnot control\x1b", "BLT2002");
        error.diagnostic.source_line="  value";
        auto json=bolt::tooling::Json::parse(bolt::format_diagnostics_json({error.diagnostic}));
        check(json["diagnostics"].array()[0]["location"]["column"].integer()==3,"wrong JSON location");
        auto text=bolt::format_diagnostic(error.diagnostic);
        check(text.find('\x1b')==text.npos,"terminal escape leaked");
        auto msbuild=bolt::format_diagnostic(error.diagnostic,bolt::DiagnosticFormat::MSBuild);
        check(msbuild.find("test name.bolt(2,3): error BLT2002:")==0,"wrong MSBuild prefix");
    });
    test("runtime-diagnostic-categories", [] {
        check(bolt::runtime_diagnostic({},"division by zero").code=="BLT3001","division code");
        check(bolt::runtime_diagnostic({},"array index out of range").code=="BLT3002","array code");
        check(bolt::runtime_diagnostic({},"null pointer").code=="BLT3003","null code");
        check(bolt::runtime_diagnostic({},"integer overflow").code=="BLT3004","overflow code");
    });
    test("native-diagnostic-wrapper-preserves-reference", [] {
        int original=4;
        int& result=bolt::checked_at({"test.bolt",4,5,1},[&]() -> int& {return original;});
        result=9;check(original==9,"wrapper copied reference");
    });
    test("native-diagnostic-wrapper-preserves-inner-location", [] {
        try {
            bolt::checked_at({"outer.bolt",1,1,1},[] {
                return bolt::checked_at({"inner.bolt",5,9,2},[]() -> int {throw std::runtime_error("division by zero");});
            });
        }catch(const bolt::Error& error) {
            check(error.location.file=="inner.bolt"&&error.location.line==5&&error.diagnostic.code=="BLT3001","inner source lost");
            return;
        }
        throw std::runtime_error("expected diagnostic");
    });
    test("cpp-library-guard-preserves-exception-and-location", [] {
        bool translated=false;
        try {
            bolt::library::checked({"outer.bolt",1,1,1}, [] {
                bolt::library::checked({"inner.bolt",5,9,2}, [] { throw std::runtime_error("division by zero"); });
            });
        } catch (const bolt::library::DivideByZeroException& error) {
            translated=error.SourceFile().compare("inner.bolt")==0 && error.SourceLine()==5 && error.SourceColumn()==9;
        }
        check(translated,"nested guard lost the exception type or original source");
        try {
            bolt::library::checked({"outer.bolt",1,1,1}, [] { throw bolt::Error({"foreign.bolt",7,3,1},"diagnostic"); });
        } catch (const bolt::library::SystemException& error) {
            check(error.SourceFile().compare("foreign.bolt")==0 && error.SourceLine()==7 && error.SourceColumn()==3,
                  "converted compiler diagnostic lost its location");
            return;
        }
        throw std::runtime_error("expected converted compiler diagnostic");
    });
    test("lexer-ranges-include-end-and-bom", [] {
        auto tokens=bolt::tokenize("\xef\xbb\xbfint number = 10;", "tokens.bolt");
        check(tokens[0].at.column==4&&tokens[0].at.offset==3&&tokens[0].at.length==3,"BOM byte coordinates");
        check(tokens[1].at.length==6,"identifier range");
        for(std::size_t i=1;i<tokens.size();++i)check(tokens[i].at.offset>=tokens[i-1].at.offset,"token offsets not sorted");
    });
    test("declaration-locations-identify-names", [] {
        auto p=bolt::parse("public class Apple {}\npublic enum Fruit { Banana }\nconcept Numeric<T> = Number<T>;", "names.bolt");
        check(p.classes[0].token.text=="Apple"&&p.classes[0].token.at.column==14,"class name range");
        check(p.enums[0].token.text=="Fruit"&&p.enums[0].token.at.column==13,"enum name range");
        check(p.concepts[0].token.text=="Numeric","concept name range");
    });
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
