// Target-typed default values shared by the interpreter and native test suites.
struct DefaultCase {
    const char* name;
    const char* source;
    const char* expected;
    const char* error;
};

static const DefaultCase default_cases[] = {
    {"default-primitive-values", R"(
        enum State:byte {Ready=1}
        static class Program {public:void Main(){
            short a=default; short b=default; int c=default; long d=default;
            byte e=default; ushort f=default; uint g=default; ulong h=default;
            float single=default; double real=default;
            const bool flag=default; const string text=default;
            int* pointer=default; State state=default;
            Print(a+","+b+","+c+","+d+","+e+","+f+","+g+","+h);
            Print(single+","+real+","+flag+",["+text+"]");
            Print(pointer==null); Print(byte(state));
        }}
    )", "0,0,0,0,0,0,0,0\n0,0,false,[]\ntrue\n0\n", ""},
    {"default-field-initializers", R"(
        class Item {public:int Value=7;Item(){Value+=2;}}
        class Holder {public:
            int Number=default; const bool Flag=default; string Text=default;
            int* Pointer=default; int[] Values=default; Item Object=default;
            static int Count=default; static string Label=default;
        }
        static class Program {public:void Main(){
            Holder holder;
            Print(holder.Number+","+holder.Flag+",["+holder.Text+"]");
            Print(holder.Pointer==null); Print(holder.Values.Length); Print(holder.Object.Value);
            Print(Holder.Count+",["+Holder.Label+"]");
        }}
    )", "0,false,[]\ntrue\n0\n9\n0,[]\n", ""},
    {"default-assignment-destinations", R"(
        class Item {public:int Value=4;}
        static class Program {public:void Main(){
            int number=9; int& alias=number; alias=default;
            Item item; item.Value=default;
            int[] values={4,5}; values[1]=default;
            string text="old"; text=default;
            int* pointer=&number; pointer=default;
            Print(number+","+item.Value+","+values[0]+","+values[1]+",["+text+"]");
            Print(pointer==null); values=default; Print(values.Length);
        }}
    )", "0,0,4,0,[]\ntrue\n0\n", ""},
    {"default-empty-arrays-and-collections", R"(
        import System.Collections.Generic
        class NoDefault {public:NoDefault(int value){}}
        static class Program {public:void Main(){
            int[] values=default; NoDefault[] objects=default;
            List<NoDefault> list=default; Dictionary<string,int> dictionary=default;
            HashSet<int> set=default; Queue<string> queue=default;
            Print(values.Length+","+objects.Length+","+list.Count+","+dictionary.Count+","+set.Count+","+queue.Count);
            dictionary["key"]=3; set.Add(1); queue.Enqueue("old");
            dictionary=default; set=default; queue=default;
            Print(dictionary.Count+","+set.Count+","+queue.Count);
        }}
    )", "0,0,0,0,0,0\n0,0,0\n", ""},
    {"default-class-construction-and-returns", R"(
        class Item {public:
            static int Count=0; int Value=7; string Name="ready";
            Item(){Count++;Value+=2;}
        }
        Item Make(){return default;}
        T MakeDefault<T>(){return default;}
        static class Program {public:void Main(){
            Item.Count=0; Item item=default;
            Print(Item.Count); Print(item.Value+","+item.Name);
            item.Value=20; item=default; Print(Item.Count); Print(item.Value);
            Item returned=Make(); Print(Item.Count); Print(returned.Value);
            Item generic=MakeDefault<Item>(); Print(Item.Count); Print(generic.Value);
            Print(MakeDefault<int>()); Print("["+MakeDefault<string>()+"]");
        }}
    )", "1\n9,ready\n2\n9\n3\n9\n4\n9\n0\n[]\n", ""},
    {"default-typed-array-elements", R"(
        class Item {public:int Value=8;}
        static class Program {public:void Main(){
            int[] numbers={default,2,default};
            string[] text={"value",default};
            int[][] nested={default,{default,3}};
            Item[] objects={default,default}; objects[0].Value=11;
            Print(numbers[0]+","+numbers[1]+","+numbers[2]+",["+text[1]+"]");
            Print(nested[0].Length+","+nested[1][0]+","+nested[1][1]);
            Print(objects[0].Value+","+objects[1].Value);
        }}
    )", "0,2,0,[]\n0,0,3\n11,8\n", ""},
    {"default-call-and-cast-contexts", R"(
        import System.Collections.Generic
        class Item {public:int Value;Item(int value){Value=value;}}
        int Read(int value){return value;}
        T Identity<T>(T value){return value;}
        T Second<T>(T first,T second){return second;}
        static class Program {public:void Main(){
            Print(Read(default)); Print(Identity<int>(default));
            Print(Second(3,default)); Print(Second(default,4));
            Item item=Item(default); Print(item.Value);
            List<int> values;values.Add(default);Print(values[0]);
            int number=(int)default; Print(number);
            int* pointer=new int(default);Print(*pointer);delete pointer;
        }}
    )", "0\n0\n0\n4\n0\n0\n0\n0\n", ""},
    {"default-loops-and-contextual-expressions", R"(
        import Math
        class Item {public:static int Count=0;int Value=7;Item(){Count++;}}
        static class Program {public:void Main(){
            for(int i=0;i<3;i++){
                Item item=default; string text=default;
                Print(item.Value);item.Value=50;text+="x";Print(text);
            }
            Print(Item.Count);
            int number=(Print("once"),default); Print(number);
            Print(number==default); Print(default==number);
            var inferred=(int)default; Print(inferred);
            Print(Math.Max<int>(default,4));
        }}
    )", "7\nx\n7\nx\n7\nx\n3\nonce\n0\ntrue\ntrue\n0\n4\n", ""},
    {"default-generic-constants", R"(
        const T Initial<T> = default;
        static class Program {public:void Main(){
            Print(Initial<int>); Print("["+Initial<string>+"]");
            int[] values=Initial<int[]>; Print(values.Length);
        }}
    )", "0\n[]\n0\n", ""},
    {"default-needs-inferred-target", R"(
        static class Program {public:void Main(){var value=default;}}
    )", "", "'default' requires a target type"},
    {"default-needs-expression-target", R"(
        static class Program {public:void Main(){default;}}
    )", "", "'default' requires a target type"},
    {"default-ambiguous-call", R"(
        int Read(int value){return value;}
        string Read(string value){return value;}
        static class Program {public:void Main(){Read(default);}}
    )", "", "ambiguous"},
    {"default-cannot-infer-type-argument", R"(
        T Identity<T>(T value){return value;}
        static class Program {public:void Main(){Identity(default);}}
    )", "", "no matching overload"},
    {"default-cannot-bind-reference", R"(
        static class Program {public:void Main(){int& value=default;}}
    )", "", "reference"},
    {"default-cannot-bind-const-reference", R"(
        static class Program {public:void Main(){const int& value=default;}}
    )", "", "reference"},
    {"default-cannot-return-reference", R"(
        int& Invalid(){return default;}
        static class Program {public:void Main(){}}
    )", "", "reference"},
    {"default-requires-parameterless-constructor", R"(
        class Item {public:Item(int value){}}
        static class Program {public:void Main(){Item value=default;}}
    )", "", "no matching overload"},
    {"default-requires-accessible-constructor", R"(
        class Item {Item(){}}
        static class Program {public:void Main(){Item value=default;}}
    )", "", "private"},
    {"default-cannot-construct-interface", R"(
        interface IValue {int Read();}
        static class Program {public:void Main(){IValue value=default;}}
    )", "", "cannot be constructed"},
    {"default-cannot-write-const", R"(
        static class Program {public:void Main(){const int value=1;value=default;}}
    )", "", "const"},
    {"default-cannot-create-arena", R"(
        static class Program {public:void Main(){Arena owner=default;}}
    )", "", "Arena"},
    {"default-cannot-be-local-name", R"(
        static class Program {public:void Main(){var default=1;}}
    )", "", "expected an identifier"},
    {"default-cannot-be-parameter-name", R"(
        void Invalid(int default){}
        static class Program {public:void Main(){}}
    )", "", "expected an identifier"},
    {"default-cannot-be-field-name", R"(
        class Item {public:int default;}
        static class Program {public:void Main(){}}
    )", "", "expected an identifier"},
};
