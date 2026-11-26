// Shared observable arena semantics; representation-specific byte counts vary.
struct ArenaCase {const char* name;const char* source;const char* expected;const char* error;};
static const ArenaCase arena_cases[] = {
    {"arena-basic-and-reuse", R"(
        static class Program {public:void Main(){
            Arena work=Arena.Fixed(65536);
            int* first=new(work) int(7); Print(*first);
            Print(work.UsedBytes>0ul); Print(work.CapacityBytes==65536ul);
            work.Reset(); Print(work.UsedBytes==0ul); Print(work.PeakBytes>0ul);
            int* next=new(work) int(9); Print(*next);
            work.Dispose(); work.Dispose(); Print(work.CapacityBytes==0ul);
        }}
    )", "7\ntrue\ntrue\ntrue\ntrue\n9\ntrue\n", ""},
    {"arena-nested-storage-and-copy", R"(
        import System.Collections.Generic
        class Payload {public:
            string Title; int[] Numbers; List<string> Items;
            Dictionary<string,string> Names; Queue<string> Pending; HashSet<string> Tags;
            IStringable Box=(IStringable)0; int* Borrowed;
            Payload(string title){Title=title;Numbers=new int[3];}
        }
        static class Program {public:
            Payload* Create(Arena& destination,string title){return new(destination) Payload(title);}
            void Main(){
                Arena work=Arena.Fixed(1024*1024); Arena retained=Arena.Growable(1024,1024*1024);
                Payload* source=Create(work,"source"); ulong before=work.UsedBytes;
                source->Title="a title replaced after construction"; source->Numbers[1]=42;
                for(int i=0;i<40;++i)source->Items.Add("a persistent collection string");
                source->Names.Add("key","dictionary value");source->Pending.Enqueue("queue value");
                source->Tags.Add("set value");source->Box="boxed text";
                int local=17;source->Borrowed=&local;
                Print(work.UsedBytes>before);Print(work.UsedBytes<=work.CapacityBytes);
                Payload outside=*source;Payload* survivor=Create(retained,"placeholder");*survivor=*source;
                work.Reset();Print(work.UsedBytes==0ul);
                Print(outside.Title);Print(outside.Numbers[1]);Print(outside.Items.Count);
                Print(outside.Names["key"]);Print(outside.Pending.Dequeue());Print(outside.Tags.Contains("set value"));
                Print(outside.Box);Print(*outside.Borrowed);Print(survivor->Title);Print(survivor->Items.Count);
                retained.Dispose();Print(outside.Items.Count);
            }
        }
    )", "true\ntrue\ntrue\na title replaced after construction\n42\n40\ndictionary value\nqueue value\ntrue\nboxed text\n17\na title replaced after construction\n40\n40\n", ""},
    {"arena-return-independent-value", R"(
        import System.Collections.Generic
        class Result {public:string Text;List<int> Values;Result(string text){Text=text;Values.Add(42);}}
        static class Program {public:
            Result Make(){Arena work=Arena.Fixed(65536);Result* data=new(work) Result("surviving result");return *data;}
            void Main(){Result saved=Make();Print(saved.Text);Print(saved.Values[0]);}
        }
    )", "surviving result\n42\n", ""},
    {"arena-loops-and-independent-heap", R"(
        static class Program {public:
            int* Allocate(Arena& owner,int value){return new(owner) int(value);}
            void Main(){
                Arena work=Arena.Fixed(65536);int* heap=new int(11);int local=3;int* borrowed=&local;
                for(int i=0;i<6;++i){int* temporary=Allocate(work,i);Print(*temporary);work.Reset();}
                Print(*heap);Print(*borrowed);delete heap;
            }
        }
    )", "0\n1\n2\n3\n4\n5\n11\n3\n", ""},
    {"arena-grow-with-stable-roots", R"(
        class Block {public:int A;int B;int C;int D;}
        static class Program {public:void Main(){
            Arena work=Arena.Growable(512,65536);Block* first=new(work) Block();first->A=19;
            for(int i=0;i<30;++i){Block* next=new(work) Block();next->A=i;}
            Print(work.CapacityBytes>512ul);Print(work.CapacityBytes<=work.LimitBytes);Print(first->A);
        }}
    )", "true\ntrue\n19\n", ""},
    {"arena-nested-value-containers", R"(
        import System.Collections.Generic
        class Item {public:string Text;Item(){Text="an owned string longer than an inline string buffer";}}
        class Data {public:List<Item> Items;Dictionary<string,Item> Map;Queue<Item> Queue;HashSet<string> Set;}
        static class Program {public:void Main(){
            Arena a=Arena.Fixed(1024*1024);Data* data=new(a) Data();Item item;
            for(int i=0;i<20;++i)data->Items.Add(item);
            data->Map.Set("key",item);data->Queue.Enqueue(item);data->Set.Add(item.Text);
            Data copy=*data;a.Reset();Print(copy.Items[19].Text);Print(copy.Map["key"].Text);
            Item dequeued=copy.Queue.Dequeue();Print(dequeued.Text);Print(copy.Set.Count);
        }}
    )", "an owned string longer than an inline string buffer\nan owned string longer than an inline string buffer\nan owned string longer than an inline string buffer\n1\n", ""},
    {"arena-invalid-capacity", "static class Program{public:void Main(){Arena a=Arena.Fixed(0);}}", "", "invalid arena capacity"},
    {"arena-invalid-growth", "static class Program{public:void Main(){Arena a=Arena.Growable(1024,32);}}", "", "invalid arena capacity"},
    {"arena-exhaustion", "static class Program{public:void Main(){Arena a=Arena.Fixed(1);int* p=new(a) int(4);}}", "", "arena capacity exhausted"},
    {"arena-deletion-rejected", "static class Program{public:void Main(){Arena a=Arena.Fixed(65536);int* p=new(a) int(4);delete p;}}", "", "arena allocation"},
    {"arena-disposed-allocation", "static class Program{public:void Main(){Arena a=Arena.Fixed(65536);a.Dispose();int* p=new(a) int(4);}}", "", "arena has been disposed"},
    {"arena-disposed-reset", "static class Program{public:void Main(){Arena a=Arena.Fixed(65536);a.Dispose();a.Reset();}}", "", "arena has been disposed"},
    {"arena-constructor-failure-cleanup", R"(
        class Item {public:string Text;Item(int zero){Text="owned data";int fail=1/zero;}}
        static class Program{public:void Main(){Arena a=Arena.Fixed(65536);Item* p=new(a) Item(0);}}
    )", "", "division by zero"},
};
