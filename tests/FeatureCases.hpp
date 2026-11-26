// Feature cases mirrored in feature_cases.json and tests/features/.
struct FeatureCase {const char* name; const char* source; const char* expected; const char* error; bool compile_error;};
static const FeatureCase feature_cases[] = {
{R"BOLT(inherited-generic-overloads)BOLT",R"BOLT(import Console

class Filters {
public:
    int Count<T>() { return 1; }
    int Count<T, U>() { return 2; }
}

class Scene : Filters {
public:
    int Match() { return Count<int>() + Count<int, float>(); }
}

static class Program {
public:
    void Main() { Scene scene; Console.WriteLine(scene.Match()); }
}
)BOLT",R"BOLT(3
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-all-features)BOLT",R"BOLT(import Console
import Math
import Random
import Type
import System.Collections.Generic
namespace Demo;
public class Vector2 {public: float X; float Y; Vector2(float x,float y){X=x;Y=y;} }
public class Container<T> {public:T Value; Container(T value){Value=value;} }
public enum Status : byte { Idle=0, Running=1, Finished=2 }
public class Animal {public:void Speak(){Console.WriteLine("Animal");}}
public sealed class Dog : Animal {public:override sealed void Speak(){Console.WriteLine("Dog");}}
T Add<T>(T a,T b) : Number, Addable {return a+b;}
void Bump(int& value,int add){value+=add;}
void Test<T...>(){Console.WriteLine(Type.Name<T>());}
static class Program {public:void Main(){
Vector2 v=Vector2(10,5); Console.WriteLine(v.X+", "+v.Y);
int x=5;Bump(x,10);int* ptr=&x;*ptr+=3;Console.WriteLine(x);
int[] values={1,2,3};values[1]=9;Console.WriteLine(values.Length);Console.WriteLine(values[1]);
Container<int> box=Container<int>(7); Console.WriteLine(box.Value);
Console.WriteLine(Add<uint>(3,7));Test<int,string,double>();
List<int> list;list.Add(4);list.Add(8);Console.WriteLine(list.Count);Console.WriteLine(list[1]);
Dictionary<string,int> dict;dict["a"]=7;int found=0;Console.WriteLine(dict.TryGetValue("a",found));Console.WriteLine(found);
HashSet<int> set;set.Add(1);set.Add(1);Console.WriteLine(set.Count);
Queue<string> queue;queue.Enqueue("job");Console.WriteLine(queue.Dequeue());
Dog dog;Animal& animal=dog;animal.Speak();Animal* a=&dog;a->Speak();
Status state=Status.Running;Console.WriteLine(byte(state));
Console.WriteLine(Math.Floor<double>(3.9));Console.WriteLine(Math.Sin<double>(0));
int r=Random.Next<int>(10,25);Console.WriteLine(r>=10&&r<25);
Vector2* allocated=new Vector2(2,3);Console.WriteLine(allocated->X);delete allocated;
}
}
)BOLT",R"BOLT(10, 5
18
3
9
7
10
int
string
double
2
8
true
7
1
job
Dog
Dog
1
3
0
true
2
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-constructors)BOLT",R"BOLT(
class Point {public:int X; Point(){X=1;} Point(int x){X=x;} Point(int x,int y){X=x+y;} }
static class Program { public: void Main() {
Point a=Point(); Point b=Point(9); Point c(4,5); Console.WriteLine(a.X+b.X+c.X);
} }
)BOLT",R"BOLT(19
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-base-constructor)BOLT",R"BOLT(
class Base {public:int X; Base(int x){X=x;} int Read(){return X;} } class Child : Base {public:Child(int x):base(x+1){} override int Read(){return X*2;} }
static class Program { public: void Main() {
Child c=Child(5); Console.WriteLine(c.X); Console.WriteLine(c.Read());
} }
)BOLT",R"BOLT(6
12
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-constructor-dispatch)BOLT",R"BOLT(
class Base {public:Base(){Speak();} void Speak(){Console.WriteLine("base");}} class Derived:Base {public:Derived(){Speak();} void Speak(){Console.WriteLine("derived");}}
static class Program { public: void Main() {
Derived x;
} }
)BOLT",R"BOLT(base
derived
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-reference-return)BOLT",R"BOLT(
int& Get(int& x){return x;}
static class Program { public: void Main() {
int x=1; Get(x)=8; int& r=Get(x); r++; Console.WriteLine(x);
} }
)BOLT",R"BOLT(9
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-reference-field-return)BOLT",R"BOLT(
class Cell {public:int Value; int& Get(){return Value;} }
static class Program { public: void Main() {
Cell x; int& r=x.Get(); r=5; Console.WriteLine(x.Value);
} }
)BOLT",R"BOLT(5
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-const-reference)BOLT",R"BOLT(
int Read(const int& x){return x;}
static class Program { public: void Main() {
int x=7; Console.WriteLine(Read(x));
} }
)BOLT",R"BOLT(7
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-pointer-null)BOLT",R"BOLT(

static class Program { public: void Main() {
int* p; Console.WriteLine(p==null); int x=3; p=&x; int** q=&p; **q=5; Console.WriteLine(x);
} }
)BOLT",R"BOLT(true
5
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-pointer-value-copy)BOLT",R"BOLT(
class Cell {public:int Value;}
static class Program { public: void Main() {
Cell a; a.Value=2; Cell b=a; b.Value=9; Cell* p=&a; p->Value=6; Console.WriteLine(a.Value); Console.WriteLine(b.Value);
} }
)BOLT",R"BOLT(6
9
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-heap-primitive)BOLT",R"BOLT(

static class Program { public: void Main() {
int* p=new int(7); Console.WriteLine(*p); delete p; p=null; Console.WriteLine(p==null);
} }
)BOLT",R"BOLT(7
true
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-array-value-copy)BOLT",R"BOLT(

static class Program { public: void Main() {
int[] a=new int[3]; a[0]=7; int[] b=a; b[0]=8; Console.WriteLine(a[0]); Console.WriteLine(b[0]); Console.WriteLine(a.Length);
} }
)BOLT",R"BOLT(7
8
3
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-nested-arrays)BOLT",R"BOLT(

static class Program { public: void Main() {
int[][] a={{1,2},{3}}; Console.WriteLine(a[0][1]); Console.WriteLine(a[1].Length);
} }
)BOLT",R"BOLT(2
1
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-bool-arrays-references)BOLT",R"BOLT(import System.Collections.Generic
void Flip(bool& value){value=!value;}
static class Program { public: void Main() {
bool[] a={true,false}; Flip(a[0]); Console.WriteLine(a[0]); List<bool> b; b.Add(false); Flip(b[0]); Console.WriteLine(b[0]);
} }
)BOLT",R"BOLT(false
true
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-array-custom-ctors)BOLT",R"BOLT(
class Point {public:int X; Point(int x){X=x;} }
static class Program { public: void Main() {
Point[] a={Point(4),Point(8)}; Console.WriteLine(a[1].X);
} }
)BOLT",R"BOLT(8
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-generic-inference)BOLT",R"BOLT(
T Add<T>(T a,T b): Addable {return a+b;}
static class Program { public: void Main() {
Console.WriteLine(Add(3,4)); Console.WriteLine(Add("a","b"));
} }
)BOLT",R"BOLT(7
ab
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-generic-multiple)BOLT",R"BOLT(
T1 First<T1,T2>(T1 a,T2 b): T1: Number, T2: Equatable {return a;} class Pair<T1,T2> {public:T1 A; T2 B; Pair(T1 a,T2 b){A=a;B=b;}}
static class Program { public: void Main() {
Console.WriteLine(First<int,string>(9,"a")); Pair<int,string> p=Pair<int,string>(4,"four"); Console.WriteLine(p.A); Console.WriteLine(p.B);
} }
)BOLT",R"BOLT(9
4
four
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-generic-nested)BOLT",R"BOLT(import System.Collections.Generic
class Container<T>{public:T Value; U Echo<U>(U x){return x;} }
static class Program { public: void Main() {
Container<List<int>> c; c.Value.Add(6); Console.WriteLine(c.Value[0]); Console.WriteLine(c.Echo<string>("yes"));
} }
)BOLT",R"BOLT(6
yes
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-variadic-fixed)BOLT",R"BOLT(import Type
void Print<Fixed,T...>(){Console.WriteLine(Type.Name<T>());}
static class Program { public: void Main() {
Print<int,string,double>();
} }
)BOLT",R"BOLT(string
double
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-concept-custom)BOLT",R"BOLT(
concept Scalar<T> = Number<T> && Addable<T>; T Add<T>(T a,T b):Scalar{return a+b;}
static class Program { public: void Main() {
Console.WriteLine(Add<int>(3,4));
} }
)BOLT",R"BOLT(7
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-concept-logic)BOLT",R"BOLT(
concept NotInteger<T> = Equatable<T> && !Integer<T>; T Echo<T>(T x):NotInteger{return x;}
static class Program { public: void Main() {
Console.WriteLine(Echo<string>("ok"));
} }
)BOLT",R"BOLT(ok
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-list-api)BOLT",R"BOLT(import System.Collections.Generic

static class Program { public: void Main() {
List<int> a; a.Add(3); a.Insert(0,1); a.Add(3); Console.WriteLine(a.IndexOf(3)); Console.WriteLine(a.Remove(3)); a.RemoveAt(0); Console.WriteLine(a[0]); List<int> b=a; b[0]=9; Console.WriteLine(a[0]); a.Clear(); Console.WriteLine(a.Count);
} }
)BOLT",R"BOLT(1
true
3
3
0
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-hashset-custom)BOLT",R"BOLT(import System.Collections.Generic
class Key {public:int Id; Key(int id){Id=id;} }
static class Program { public: void Main() {
HashSet<Key> s; Console.WriteLine(s.Add(Key(1))); Console.WriteLine(s.Add(Key(1))); Console.WriteLine(s.Contains(Key(1))); Console.WriteLine(s.Remove(Key(1))); Console.WriteLine(s.Count);
} }
)BOLT",R"BOLT(true
false
true
true
0
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-dictionary-api)BOLT",R"BOLT(import System.Collections.Generic

static class Program { public: void Main() {
Dictionary<string,int> d; d.Add("a",1); d.Set("a",2); Console.WriteLine(d["a"]); int out=9; Console.WriteLine(d.TryGetValue("b",out)); Console.WriteLine(out); Console.WriteLine(d.ContainsKey("a")); Console.WriteLine(d.Remove("a")); d.Clear(); Console.WriteLine(d.Count);
} }
)BOLT",R"BOLT(2
false
9
true
true
0
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-dictionary-custom)BOLT",R"BOLT(import System.Collections.Generic
class Key {public:int Id; Key(int id){Id=id;} }
static class Program { public: void Main() {
Dictionary<Key,string> d; d.Add(Key(1),"one"); Console.WriteLine(d[Key(1)]);
} }
)BOLT",R"BOLT(one
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-queue-peek-enumerator)BOLT",R"BOLT(import System.Collections.Generic
static class Program { public: void Main() {
    Queue<int> q; q.Enqueue(10); q.Enqueue(20); q.Enqueue(30);
    QueueEnumerator<int> it = q.GetEnumerator();
    Console.WriteLine(it.MoveNext()); Console.WriteLine(it.Current);
    Console.WriteLine(q.Peek());
    Console.WriteLine(it.MoveNext()); Console.WriteLine(it.Current);
    Console.WriteLine(q.Dequeue()); q.Enqueue(40);
    Console.WriteLine(q.Dequeue()); Console.WriteLine(q.Dequeue()); Console.WriteLine(q.Dequeue());
} }
)BOLT",R"BOLT(true
10
10
true
20
10
20
30
40
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-queue-api)BOLT",R"BOLT(import System.Collections.Generic

static class Program { public: void Main() {
Queue<int> q; int out=9; Console.WriteLine(q.TryDequeue(out)); Console.WriteLine(out); q.Enqueue(1); q.Enqueue(2); Console.WriteLine(q.Peek()); Console.WriteLine(q.TryDequeue(out)); Console.WriteLine(out); Console.WriteLine(q.Dequeue()); Console.WriteLine(q.Count);
} }
)BOLT",R"BOLT(false
9
1
true
1
2
0
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-enum-all-storage)BOLT",R"BOLT(
enum A:short {First=-128} enum B:ulong {Last=18446744073709551615ul} enum Status {First=4,Second,Third=Second+1}
static class Program { public: void Main() {
A a=A.First; B b=B.Last; Console.WriteLine(short(a)); Console.WriteLine(ulong(b)); Console.WriteLine(Status.Second); Console.WriteLine(Status.Second>Status.First);
} }
)BOLT",R"BOLT(-128
18446744073709551615
5
true
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-math-functions)BOLT",R"BOLT(import Math

static class Program { public: void Main() {
Console.WriteLine(Math.Abs<int>(-9)); Console.WriteLine(Math.Min<int>(2,3)); Console.WriteLine(Math.Max<int>(2,3)); Console.WriteLine(Math.Clamp<int>(9,1,4)); Console.WriteLine(Math.Round<double>(2.5)); Console.WriteLine(Math.Floor<double>(-2.1)); Console.WriteLine(Math.Ceiling<double>(2.1)); Console.WriteLine(Math.Sqrt<double>(9)); Console.WriteLine(Math.Pow<double>(2,3)); Console.WriteLine(Math.Log2<double>(8)); Console.WriteLine(Math.Sin<float>(0)); Console.WriteLine(Math.Pi>3 && Math.E>2);
} }
)BOLT",R"BOLT(9
2
3
4
3
-3
3
3
8
3
0
true
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-random-ranges)BOLT",R"BOLT(import Random
import System.Collections.Generic

static class Program { public: void Main() {
    Random.Seed(123);
    List<float> values;
    bool ok=true;
    bool fractionalFloat=false;
    bool fractionalFloatMax=false;
    bool fractionalDouble=false;
    bool fractionalDoubleMax=false;
    for(int i=0;i<100;i++) {
        int a=Random.Next<int>(10);
        float b=Random.Next<float>(-2,3);
        ulong c=Random.Next<ulong>(3ul,20ul);
        values.Add(Random.Next<float>(0,1.0f));
        float f=Random.Next<float>(1.0f);
        double d=Random.Next<double>(0,1.0);
        double e=Random.Next<double>(1.0);
        ok=ok && a>=0 && a<10 && b>=-2 && b<3 && c>=3ul && c<20ul;
        ok=ok && values[i]>=0 && values[i]<1 && f>=0 && f<1;
        ok=ok && d>=0 && d<1 && e>=0 && e<1;
        fractionalFloat=fractionalFloat || values[i]>0;
        fractionalFloatMax=fractionalFloatMax || f>0;
        fractionalDouble=fractionalDouble || d>0;
        fractionalDoubleMax=fractionalDoubleMax || e>0;
    }
    Console.WriteLine(ok && values.Count==100 && fractionalFloat && fractionalFloatMax && fractionalDouble && fractionalDoubleMax);
} }
)BOLT",R"BOLT(true
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-runtime-new-features)BOLT",R"BOLT(

static class Program { public: void Main() {
Runtime.ExecuteProgram("import System.Collections.Generic\n class Box<T>{public:T X;Box(T x){X=x;}} static class Program{public:void Main(){Box<int> b=Box<int>(8);int& x=b.X;x++;List<int> l;l.Add(x);Console.WriteLine(l[0]);}}");
} }
)BOLT",R"BOLT(9
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-base-slicing)BOLT",R"BOLT(
class Base{public:int X;void Speak(){Console.WriteLine("base");}} class Dog:Base{public:void Speak(){Console.WriteLine("dog");}} void Take(Base b){b.Speak();}
static class Program { public: void Main() {
Dog d; d.X=3; Base& r=d; Base b=r; b.Speak(); r.Speak(); Take(r);
} }
)BOLT",R"BOLT(base
dog
base
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-reference-array-parameter)BOLT",R"BOLT(
void Set(int[]& a){a[0]=7;}
static class Program { public: void Main() {
int[] a={1}; Set(a); Console.WriteLine(a[0]);
} }
)BOLT",R"BOLT(7
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-fully-qualified-collection)BOLT",R"BOLT(

static class Program { public: void Main() {
System.Collections.Generic.List<int> a; a.Add(3); Console.WriteLine(a[0]);
} }
)BOLT",R"BOLT(3
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-constructor-private)BOLT",R"BOLT(
class Cell {Cell(int x){} }
static class Program { public: void Main() {
Cell x=Cell(1);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(private)BOLT",true},
{R"BOLT(v2-constructor-no-default)BOLT",R"BOLT(
class Cell {public:Cell(int x){} }
static class Program { public: void Main() {
Cell x;
} }
)BOLT",R"BOLT()BOLT",R"BOLT(no matching overload)BOLT",true},
{R"BOLT(v2-base-no-default)BOLT",R"BOLT(
class Base {public:Base(int x){}} class Child:Base {}
static class Program { public: void Main() {
Child x;
} }
)BOLT",R"BOLT()BOLT",R"BOLT(no matching overload)BOLT",true},
{R"BOLT(v2-reference-temporary)BOLT",R"BOLT(
void Touch(int& x){x++;}
static class Program { public: void Main() {
Touch(1);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(no matching overload)BOLT",true},
{R"BOLT(v2-reference-null)BOLT",R"BOLT(

static class Program { public: void Main() {
int& x=null;
} }
)BOLT",R"BOLT()BOLT",R"BOLT(cannot assign)BOLT",true},
{R"BOLT(v2-reference-uninitialized)BOLT",R"BOLT(

static class Program { public: void Main() {
int& x;
} }
)BOLT",R"BOLT()BOLT",R"BOLT(initializer)BOLT",true},
{R"BOLT(v2-reference-return-local)BOLT",R"BOLT(
int& Bad(){int x=1;return x;}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(reference to a local)BOLT",true},
{R"BOLT(v2-const-reference-write)BOLT",R"BOLT(
void Bad(const int& x){x++;}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(const)BOLT",true},
{R"BOLT(v2-array-type)BOLT",R"BOLT(

static class Program { public: void Main() {
int[] a={1,"two"};
} }
)BOLT",R"BOLT()BOLT",R"BOLT(cannot assign)BOLT",true},
{R"BOLT(v2-generic-constraint)BOLT",R"BOLT(
T Add<T>(T a,T b):Number,Addable{return a+b;}
static class Program { public: void Main() {
Add<string>("a","b");
} }
)BOLT",R"BOLT()BOLT",R"BOLT(does not satisfy concept)BOLT",true},
{R"BOLT(v2-generic-class-arity)BOLT",R"BOLT(
class Box<T>{public:T Value;}
static class Program { public: void Main() {
Box<int,string> x;
} }
)BOLT",R"BOLT()BOLT",R"BOLT(arity)BOLT",true},
{R"BOLT(v2-generic-pack-return)BOLT",R"BOLT(
int Test<T...>(){return sizeof(T);}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(must return void)BOLT",true},
{R"BOLT(v2-enum-floating)BOLT",R"BOLT(
enum E:float{A}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(underlying type)BOLT",true},
{R"BOLT(v2-enum-custom)BOLT",R"BOLT(
class Cell{} enum E:Cell{A}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(underlying type)BOLT",true},
{R"BOLT(v2-enum-overflow)BOLT",R"BOLT(
enum E:byte{A=256}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(out of range)BOLT",true},
{R"BOLT(v2-enum-strong-typing)BOLT",R"BOLT(
enum E{A}
static class Program { public: void Main() {
int x=E.A;
} }
)BOLT",R"BOLT()BOLT",R"BOLT(cannot assign)BOLT",true},
{R"BOLT(v2-sealed-class)BOLT",R"BOLT(
sealed class Base{} class Child:Base{}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(sealed class)BOLT",true},
{R"BOLT(v2-sealed-method)BOLT",R"BOLT(
class Base {public:sealed void Test(){}} class Child:Base {public:void Test(){}}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(sealed method)BOLT",true},
{R"BOLT(v2-override-missing)BOLT",R"BOLT(
class A {public:override void Test(){}}
static class Program { public: void Main() {

} }
)BOLT",R"BOLT()BOLT",R"BOLT(override does not match)BOLT",true},
{R"BOLT(v2-math-invalid-type)BOLT",R"BOLT(import Math

static class Program { public: void Main() {
Math.Sin<int>(1);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(FloatingPoint)BOLT",true},
{R"BOLT(v2-array-bounds)BOLT",R"BOLT(

static class Program { public: void Main() {
int[] a={1}; Console.WriteLine(a[1]);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(index out of range)BOLT",false},
{R"BOLT(v2-null-dereference)BOLT",R"BOLT(

static class Program { public: void Main() {
int* p=null; Console.WriteLine(*p);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(null pointer)BOLT",false},
{R"BOLT(v2-dictionary-missing)BOLT",R"BOLT(import System.Collections.Generic

static class Program { public: void Main() {
Dictionary<int,int> d; Console.WriteLine(d[1]);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(dictionary key not found)BOLT",false},
{R"BOLT(v2-queue-empty)BOLT",R"BOLT(import System.Collections.Generic

static class Program { public: void Main() {
Queue<int> q; q.Dequeue();
} }
)BOLT",R"BOLT()BOLT",R"BOLT(queue is empty)BOLT",false},
{R"BOLT(v2-random-empty)BOLT",R"BOLT(import Random

static class Program { public: void Main() {
Random.Next<int>(0);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(min < max)BOLT",false},
{R"BOLT(v2-dictionary-duplicate)BOLT",R"BOLT(import System.Collections.Generic

static class Program { public: void Main() {
Dictionary<int,int> d;d.Add(1,2);d.Add(1,3);
} }
)BOLT",R"BOLT()BOLT",R"BOLT(duplicate dictionary key)BOLT",false},
{R"BOLT(v2-const-pointer-binding)BOLT",R"BOLT(void Touch(const int* p){*p+=2;}
static class Program{public:void Main(){int x=1;const int* p=&x;int* q=p;*p=3;Touch(p);Console.WriteLine(*q);}}
)BOLT",R"BOLT(5
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-const-pointer-reference)BOLT",R"BOLT(void Read(const int*& p){*p+=1;}
static class Program{public:void Main(){int x=7;int* p=&x;Read(p);Console.WriteLine(x);}}
)BOLT",R"BOLT(8
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-const-pointer-reassign)BOLT",R"BOLT(
static class Program{public:void Main(){int x=1;const int* p=&x;p=null;}}
)BOLT",R"BOLT()BOLT",R"BOLT(const)BOLT",true},
{R"BOLT(v2-reference-temporary-receiver)BOLT",R"BOLT(class Cell{public:int X;int& Get(){return X;}}
static class Program{public:void Main(){int& r=Cell().Get();}}
)BOLT",R"BOLT()BOLT",R"BOLT(temporary receiver)BOLT",true},
{R"BOLT(v2-reference-local-field-return)BOLT",R"BOLT(class Cell{public:int X;} int& Bad(){Cell c;return c.X;}
static class Program{public:void Main(){}}
)BOLT",R"BOLT()BOLT",R"BOLT(reference to a local)BOLT",true},
{R"BOLT(v2-reference-local-array-return)BOLT",R"BOLT(int& Bad(){int[] a={1};return a[0];}
static class Program{public:void Main(){}}
)BOLT",R"BOLT()BOLT",R"BOLT(reference to a local)BOLT",true},
{R"BOLT(v2-dictionary-nondefault-value)BOLT",R"BOLT(import System.Collections.Generic
class Vector2{public:float X;Vector2(float x){X=x;}} static class Program{public:void Main(){Dictionary<string,Vector2> d;d["a"]=Vector2(3);d["a"]=Vector2(5);Console.WriteLine(d["a"].X);}}
)BOLT",R"BOLT(5
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-pointer-field-default)BOLT",R"BOLT(class Node{public:Node* Next;} static class Program{public:void Main(){Node node;Console.WriteLine(node.Next==null);}}
)BOLT",R"BOLT(true
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-pointer-collections)BOLT",R"BOLT(import System.Collections.Generic
static class Program {public:void Main(){
 int x=1;
 const int*[] a={&x};
 *a[0]=5;
 Console.WriteLine(x);
 List<int> l;
 const List<int>* p=&l;
 p->Add(9);
 Console.WriteLine(p->Count);
 Console.WriteLine(l[0]);
 int[] values={1,2};
 int[]* arrayPointer=&values;
 Console.WriteLine(arrayPointer->Length);
 string text="abc"; string* textPointer=&text;
 Console.WriteLine(textPointer->Length);
}}
)BOLT",R"BOLT(5
1
9
2
3
)BOLT",R"BOLT()BOLT",false},
{R"BOLT(v2-virtual-generic-rejected)BOLT",R"BOLT(class Item {public: virtual T Echo<T>(T value){return value;}}
static class Program {public:void Main(){}}
)BOLT",R"BOLT()BOLT",R"BOLT(generic methods cannot be virtual)BOLT",true},
};
