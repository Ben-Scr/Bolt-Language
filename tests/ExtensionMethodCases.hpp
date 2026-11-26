// Receiver syntax must share the argument/ownership rules on every backend.
struct ExtensionMethodCase {
    const char* name;
    const char* source;
    const char* expected;
    const char* error;
};

static const ExtensionMethodCase extension_method_cases[] = {
    {"extension-global-and-static-syntax", R"(
        int Twice(this int value){return value*2;}
        static class Extras {public:int Plus(this int value,int amount=3){return value+amount;}}
        static class Program {public:void Main(){int value=4;Print(value.Twice());Print(Twice(value));Print(value.Plus());Print(Extras.Plus(value,5));}}
    )", "8\n8\n7\n9\n", ""},
    {"extension-generics-arrays-and-lists", R"(
        import System.Collections.Generic;
        T Identity<T>(this T value){return value;}
        T First<T>(this const List<T>& values){return values.ItemAt(0);}
        T First<T>(this const T[]& values){return values[0];}
        static class Program {public:void Main(){
            List<int> values;values.Add(7);int[] array={9,10};
            Print(values.First());Print(array.First());Print((3).Identity<int>().Identity());
        }}
    )", "7\n9\n3\n", ""},
    {"extension-reference-value-and-const-receivers", R"(
        class Box {public:int Value;Box(int value){Value=value;}}
        void Bump(this Box& value){value.Value+=2;}
        int Copy(this Box value){value.Value+=100;return value.Value;}
        int Read(this const Box& value){return value.Value;}
        static class Program {public:void Main(){
            Box box=Box(1);box.Bump();Print(box.Copy());Print(box.Value);
            const Box constant=Box(8);Print(constant.Read());Print(Box(12).Read());
            Box* pointer=&box;pointer->Bump();Print(box.Value);
        }}
    )", "103\n3\n8\n12\n5\n", ""},
    {"extension-receiver-evaluation-order", R"(
        static class State {public:int Trace=0;int Value=5;
            int& Receiver(){Trace=Trace*10+1;return Value;}
            int Argument(){Trace=Trace*10+2;return 3;}
        }
        int Add(this int& value,int amount){value+=amount;return value;}
        static class Program {public:void Main(){Print(State.Receiver().Add(State.Argument()));Print(State.Trace);Print(State.Value);}}
    )", "8\n12\n8\n", ""},
    {"extension-instance-overload-precedence", R"(
        class Box {public:int Pick(long value){return 1;}int Other(string value){return 3;}}
        int Pick(this const Box& receiver,int value){return 2;}
        int Other(this const Box& receiver,int value){return 4;}
        static class Program {public:void Main(){Box value;Print(value.Pick(3));Print(value.Other(3));Print(value.Other("x"));}}
    )", "1\n4\n3\n", ""},
    {"extension-string-native-precedence-and-fallback", R"(
        bool Contains(this string value,int code){return code==7;}
        bool Contains(this string value,string text){return false;}
        int Characters(this string value){return value.Length;}
        string ToString(this int value,int prefix){return prefix+":"+value;}
        static class Program {public:void Main(){Print("bolt".Contains("ol"));Print("bolt".Contains(7));Print("bolt".Characters());Print((4).ToString());Print((4).ToString(2));}}
    )", "true\ntrue\n4\n4\n2:4\n", ""},
    {"extension-namespace-local-visibility", R"(
        namespace Sample;
        static class Extras {public:int Twice(this int value){return value*2;}}
        static class Program {public:void Main(){Print((6).Twice());}}
    )", "12\n", ""},
    {"extension-private-visible-in-owner", R"(
        static class Extras {private:int Twice(this int value){return value*2;}public:int Run(){return (5).Twice();}}
        static class Program {public:void Main(){Print(Extras.Run());}}
    )", "10\n", ""},
    {"extension-reference-return", R"(
        class Box {public:int Value;}
        int& Access(this Box& value){return value.Value;}
        static class Program {public:void Main(){Box box;int& alias=box.Access();alias=9;Print(box.Value);}}
    )", "9\n", ""},
    {"extension-variadic-arguments", R"(
        int Sum(this int value,int... rest){for(int i=0;i<rest.Length;i++)value+=rest[i];return value;}
        static class Program {public:void Main(){Print((2).Sum(3,4));}}
    )", "9\n", ""},
    {"extension-nonstatic-rejected", R"(
        class Extras {public:int Twice(this int value){return value*2;}}
        static class Program {public:void Main(){}}
    )", "", "extension methods must be static"},
    {"extension-misplaced-this-rejected", R"(
        int Add(int first,this int receiver){return first+receiver;}
        static class Program {public:void Main(){}}
    )", "", "first extension receiver"},
    {"extension-duplicate-this-rejected", R"(
        int Add(this this int receiver){return receiver;}
        static class Program {public:void Main(){}}
    )", "", "only allowed once"},
    {"extension-optional-receiver-rejected", R"(
        int Add(this int receiver=1){return receiver;}
        static class Program {public:void Main(){}}
    )", "", "receiver cannot be optional or variadic"},
    {"extension-variadic-receiver-rejected", R"(
        int Add(this int... receiver){return 1;}
        static class Program {public:void Main(){}}
    )", "", "receiver cannot be optional or variadic"},
    {"extension-generic-container-rejected", R"(
        static class Extras<T> {public:int Add(this int receiver){return receiver;}}
        static class Program {public:void Main(){}}
    )", "", "non-generic declaring type"},
    {"extension-temporary-mutable-receiver-rejected", R"(
        void Bump(this int& value){value++;}
        static class Program {public:void Main(){(2).Bump();}}
    )", "", "no matching overload"},
    {"extension-const-mutable-receiver-rejected", R"(
        void Bump(this int& value){value++;}
        static class Program {public:void Main(){const int value=2;value.Bump();}}
    )", "", "no matching overload"},
    {"extension-temporary-reference-return-rejected", R"(
        static class State {public:int Value;}
        int& Access(this const int& value){return State.Value;}
        static class Program {public:void Main(){int& alias=(2).Access();}}
    )", "", "temporary extension receiver"},
    {"extension-ambiguous-rejected", R"(
        static class First {public:int Pick(this int value){return value;}}
        static class Second {public:int Pick(this int value){return value;}}
        static class Program {public:void Main(){Print((2).Pick());}}
    )", "", "ambiguous method overload"},
    {"extension-private-hidden", R"(
        class Box {}
        static class Hidden {private:int Read(this const Box& value){return 1;}}
        static class Program {public:void Main(){Box value;Print(value.Read());}}
    )", "", "has no member"},
};
