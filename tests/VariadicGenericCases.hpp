#pragma once
struct VariadicGenericCase {const char* name;const char* source;const char* expected;const char* error;bool compile_error;};
inline const VariadicGenericCase variadic_generic_cases[] = {
{"heterogeneous-packs-events-17",R"BOLT(class Player {public:int Score=9;}
class Bus<T...> {
public:
    event Action<T...> Changed;
    void Raise(T... values) { Changed(values...); }
}
R Forward<R, O, T...>(O& receiver,T... values) { return receiver.Accept(values...); }
void Dispatch<T...>(Action<T...> callback,T... values) { callback(values...); }
class Receiver {
public:
    int Count=0;
    int Accept(int number,string text,Player player) { return number+text.Length+player.Score; }
    void Mixed(int number,string text,Player player) { Count+=number+text.Length+player.Score; }
    void Empty() { Count++; }
    void Mutate(int& number,string& text) { number+=4;text+="!"; }
    void Many(int p0,int p1,int p2,int p3,int p4,int p5,int p6,int p7,int p8,int p9,int p10,int p11,int p12,int p13,int p14,int p15,int p16) { Count+=p0+p16; }
}
static class Program {
public:
    void Main() {
        Receiver receiver;Player player;
        Action<int,string,Player> mixed=receiver.Mixed;
        Dispatch<int,string,Player>(mixed,3,"yes",player);
        Print(receiver.Count);
        Print(Forward<int,Receiver,int,string,Player>(receiver,2,"ok",player));
        Action empty=receiver.Empty;Dispatch<>(empty);Print(receiver.Count);
        int number=5;string text="ref";
        Bus<int&,string&> refs;refs.Changed+=receiver.Mutate;
        refs.Raise(number,text);Print(number);Print(text);
        refs.Changed-=receiver.Mutate;refs.Raise(number,text);Print(number);
        Bus<int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int> many;
        many.Changed+=receiver.Many;many.Raise(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);Print(receiver.Count);
        many.Changed-=receiver.Many;many.Raise(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);Print(receiver.Count);
        Action<int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int> large=receiver.Many;large(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);Print(receiver.Count);
        Action<int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int> same=receiver.Many;
        Print(large.InvocationId()==same.InvocationId());
        Print(large.InvocationKey()==same.InvocationKey());
        Print(large.InvocationTarget()==same.InvocationTarget());
    }
}
)BOLT","15\n13\n16\n9\nref!\n9\n34\n34\n52\ntrue\ntrue\ntrue\n","",false},
{"pack-inferred-return",R"BOLT(
R Forward<R,T...>(R result,T... values) { return result; }
static class Program {public:void Main(){Print(Forward(7,"hello",3.5));Print(Forward(9));}}
)BOLT","7\n9\n","",false},
{"pack-constraint-each",R"BOLT(
void Numbers<T...>(T... values):Integer<T> {}
static class Program {public:void Main(){Numbers(1,"bad");}}
)BOLT","","do not satisfy",false},
{"pack-constraint-empty-and-shorthand",R"BOLT(
int Numbers<T...>(T... values):Integer{return 4;}
static class Program {public:void Main(){Print(Numbers());Print(Numbers(1,2l,(byte)3));}}
)BOLT","4\n4\n","",false},
{"pack-explicit-arity",R"BOLT(
void Sink<T...>(T... values) {}
static class Program {public:void Main(){Sink<int,string>(1);}}
)BOLT","","no matching overload",false},
{"pack-class-empty",R"BOLT(
class Box<T...>{public:int Apply(T... values){return 3;}}
static class Program {public:void Main(){Box<> box;Print(box.Apply());}}
)BOLT","3\n","",false},
{"pack-fixed-default",R"BOLT(
int Default<T...>(int first=6,T... values){return first;}
static class Program {public:void Main(){Print(Default());Print(Default(8,"extra",true));}}
)BOLT","6\n8\n","",false},
{"pack-types-without-value-pack",R"BOLT(
class Box<T...>{public:int Value=7;}
Box<T...> Make<T...>(){Box<T...> result;return result;}
int Count<T...>(Box<T...> box){return box.Value;}
static class Program {public:void Main(){Box<int,string> pair=Make<int,string>();Box<> empty=Make<>();Print(Count(pair));Print(Count(empty));}}
)BOLT","7\n7\n","",false},
{"pack-unused-types-instantiate-once",R"BOLT(
int Count<T...>(){return 7;}
void Once<T...>(){Print("once");}
int IntegerTypes<T...>():Integer<T>{return 9;}
static class Program {public:void Main(){
Print(Count<>());Print(Count<int,string>());
Print(Count<int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int,int>());
Once<>();Once<int,string>();Print(IntegerTypes<>());Print(IntegerTypes<int,long,byte>());
}}
)BOLT","7\n7\n7\nonce\nonce\n9\n9\n","",false},
{"pack-unused-types-constrained",R"BOLT(
int IntegerTypes<T...>():Integer<T>{return 9;}
static class Program {public:void Main(){Print(IntegerTypes<int,string>());}}
)BOLT","","do not satisfy",false},
{"pack-inference-from-delegate-reference",R"BOLT(
void Dispatch<T...>(Action<T...> callback,T... values){callback(values...);}
static class Program {public:void Increment(int& number){number++;}void Main(){Action<int&> callback=Increment;int value=3;Dispatch(callback,value);Print(value);}}
)BOLT","4\n","",false},
{"pack-generic-method-group",R"BOLT(
void Sink(int number,string text){Print(number);Print(text);}
void Handler<T...>(T... values){Sink(values...);}
static class Program {public:void Main(){Action<int,string> handler=Handler;handler(4,"typed");}}
)BOLT","4\ntyped\n","",false},
{"pack-expression-expansion",R"BOLT(
T Twice<T>(T value):Addable<T>{return value+value;}
void Dispatch<T...>(Action<T...> callback,T... values){callback(Twice<T>(values)...);}
static class Program {public:
void Show(int number,string text){Print(number);Print(text);}
void Main(){Action<int,string> callback=Show;Dispatch<int,string>(callback,4,"go");}
}
)BOLT","8\ngogo\n","",false},
{"pack-const-and-reference-return",R"BOLT(
int Read(const int& number,const long& wide){return number+(int)wide;}
int Forward<T...>(const T&... values){return Read(values...);}
R& Borrow<R,T...>(R& value,T... rest){return value;}
static class Program {public:void Main(){const int first=4;const long second=5l;Print(Forward(first,second));int value=7;int& alias=Borrow(value,"unused",true);alias=9;Print(value);}}
)BOLT","9\n9\n","",false},
{"removed-numbered-action",R"BOLT(
static class Program {public:void Main(){System.Action1<int> value;}}
)BOLT","","unknown type",false},
};
