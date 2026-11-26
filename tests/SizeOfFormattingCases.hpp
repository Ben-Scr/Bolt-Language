// The same programs validate interpreter and native JIT semantics.
struct SizeOfFormattingCase {
    const char* name;
    const char* source;
    const char* expected;
    const char* error;
};
static const SizeOfFormattingCase sizeof_formatting_cases[] = {
    {"sizeof-primitives-and-indirection", R"(
        static class Program {public:void Main(){
            const int word=sizeof(int);
            Print(sizeof(byte)+","+sizeof(short)+","+sizeof(ushort)+","+word+","+sizeof(uint)+","+sizeof(long)+","+sizeof(ulong));
            Print(sizeof(bool)+","+sizeof(char)+","+sizeof(float)+","+sizeof(double));
            Print(sizeof(int*)+","+sizeof(void*)+","+sizeof(int**)+","+sizeof(int&)+","+sizeof(int?)+","+sizeof(int*?));
            Print(sizeof(string)+","+sizeof(int[])+","+sizeof(int[,]));
        }}
    )", "1,2,2,4,4,8,8\n1,4,4,8\n8,8,8,4,16,16\n8,8,8\n", ""},
    {"sizeof-bolt-layout-and-generic-constants", R"(
        enum Mode:ushort {Ready=1}
        interface Tag {int Read();}
        class Empty {}
        struct Pair {public:byte Tag;long Value;}
        class Player {public:byte Tag;int Score;short Level;int* Next;}
        class Base {public:int Count;}
        class Derived:Base {public:byte Tag;long Value;}
        class Box<T> {public:byte Tag;T Value;}
        const int Bytes<T> = sizeof(T);
        int Size<T>(){return sizeof(T);}
        static class Program {public:void Main(){
            Print(sizeof(Mode)+","+sizeof(Tag)+","+sizeof(Empty)+","+sizeof(Pair)+","+sizeof(Player)+","+sizeof(Derived));
            Print(Bytes<Box<int>>+","+Bytes<Box<long>>+","+Size<Box<Player>>()+","+sizeof(Player?));
        }}
    )", "2,16,8,16,32,32\n16,24,48,40\n", ""},
    {"sizeof-native-struct-layout", R"(
        struct Vec3 {public:float X;float Y;float Z;}
        struct Inner {public:byte A;int B;}
        struct Outer {public:byte Prefix;Inner Value;short Tail;}
        struct Empty {}
        struct PointerRecord {public:byte Tag;void* Address;}
        static class Program {public:void Main(){
            Print(sizeof(Vec3)+","+sizeof(Inner)+","+sizeof(Outer)+","+sizeof(Empty)+","+sizeof(PointerRecord));
        }}
    )", "12,8,16,1,16\n", ""},
    {"sizeof-does-not-construct", R"(
        class Counter {public:static int Count=0;int Value;Counter(){Count++;}}
        class Node {public:Node* Next;Node[] Children;int Value;}
        static class Program {public:void Main(){Print(sizeof(Counter));Print(Counter.Count);Print(sizeof(Node));}}
    )", "16\n0\n32\n", ""},
    {"sizeof-enum-constant-expression", R"(
        namespace Example;
        struct Record {public:byte Flag;long Value;}
        enum Width:int {Byte=sizeof(byte),Long=sizeof(long),RecordBytes=sizeof(Record)+1}
        static class Program {public:void Main(){Print(Width.Byte);Print(Width.Long);Print(Width.RecordBytes);}}
    )", "1\n8\n17\n", ""},
    {"pointer-formatting-and-reference-values", R"(
        string Render<T>(const T& value):IStringable<T>{return (string)value;}
        static class Program {public:void Main(){
            int value=5;int& reference=value;int* pointer=&value;int* empty=null;
            string address=(string)pointer;
            Print(value);Print(reference);Print(*pointer);
            Print(address.StartsWith("0x") && address.Length>2);
            Print(address==(string)&value && address==(string)&reference);
            Print(address==Render(pointer));
            IStringable boxed=pointer;Print(address==(string)boxed);
            Print("pointer="+empty);Console.WriteLine(empty);Print(Render(empty));
            Console.WriteLine(reference);Print(Render(reference));
        }}
    )", "5\n5\n5\ntrue\ntrue\ntrue\ntrue\npointer=0x0\n0x0\n0x0\n5\n5\n", ""},
    {"sizeof-rejects-void", "static class Program {public:void Main(){Print(sizeof(void));}}", "", "unknown type 'void'"},
    {"sizeof-rejects-values", "static class Program {public:void Main(){int value=1;Print(sizeof(value));}}", "", "unknown type 'value'"},
    {"sizeof-rejects-unknown-type", "static class Program {public:void Main(){Print(sizeof(Missing));}}", "", "unknown type 'Missing'"},
};

static const char* pointer_output_source=R"(
    static class Program {public:void Main(){
        int value=5;int& reference=value;int* pointer=&value;
        Print(value);Print(&value);Print(reference);Print(&reference);
        Print(pointer);Print(*pointer);Console.WriteLine(pointer);Print((string)pointer);
    }}
)";
static bool correct_pointer_output(const std::string& output) {
    std::istringstream stream(output);std::vector<std::string> lines;
    for(std::string line;std::getline(stream,line);)lines.push_back(std::move(line));
    if(lines.size()!=8||lines[0]!="5"||lines[2]!="5"||lines[5]!="5")return false;
    const auto& address=lines[1];
    if(!address.starts_with("0x")||address.size()<3||address=="0x0")return false;
    if(address.find_first_not_of("0123456789abcdef",2)!=std::string::npos)return false;
    return lines[3]==address&&lines[4]==address&&lines[6]==address&&lines[7]==address;
}
