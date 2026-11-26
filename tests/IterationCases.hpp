// Shared interpreter/native regression cases for the structural iteration contract.
struct IterationCase {const char* name;const char* source;const char* expected;const char* error;bool interpret=true;};
static const IterationCase iteration_cases[] = {
    {"iterable-all-public-collections", R"(
        import System.Collections.Generic
        int Count<T>(T& values) : Iterable<T> {int result=0;for(var value:values)result++;return result;}
        static class Program {public:void Main(){
            List<int> list;list.Add(2);list.Add(3);
            Queue<int> queue;queue.Enqueue(4);
            HashSet<int> set;set.Add(5);set.Add(5);
            Dictionary<string,int> dictionary;dictionary.Add("one",1);
            Print(Count(list));Print(Count(queue));Print(Count(set));Print(Count(dictionary));
        }}
    )", "2\n1\n1\n1\n", ""},
    {"iterable-typed-array-string-collections", R"(
        import System.Collections.Generic
        int Count<T,E>(T& values) : Iterable<T,E> {int result=0;for(var value:values)result++;return result;}
        static class Program {public:void Main(){
            int[] array=[1,2,3];string text="ab";
            List<int> list;list.Add(4);
            Dictionary<string,int> dictionary;dictionary.Add("one",1);
            Print(Count<int[],int>(array));Print(Count<string,char>(text));
            Print(Count<List<int>,int>(list));
            Print(Count<Dictionary<string,int>,KeyValuePair<string,int>>(dictionary));
        }}
    )", "3\n2\n1\n1\n", "", false},
    {"iterable-custom-borrowed-source-and-single-evaluation", R"(
        struct Counter {public:int* Value;Counter(int* value){Value=value;}
            int Current => *Value;bool MoveNext(){(*Value)++;return *Value<=3;}}
        class Range {public:int Calls=0;int Value=0;Counter GetEnumerator(){Calls++;return Counter(&Value);}}
        int Sum<T>(T& values) : Iterable<T,int> {int result=0;for(int value:values)result+=value;return result;}
        static class Program {public:
            int Builds=0;
            Range Make(){Builds++;return Range();}
            void Main(){Range source;Print(Sum(source));Print(source.Calls);Print(source.Value);
                int result=0;for(int value:Make())result+=value;Print(result);Print(Builds);}
        }
    )", "6\n1\n4\n6\n1\n", ""},
    {"iterable-extension-getenumerator", R"(
        class Range {public:int Limit=3;}
        struct Cursor {public:int Limit;int Current=0;Cursor(int limit){Limit=limit;}
            bool MoveNext(){Current++;return Current<=Limit;}}
        static class Ranges {public:static Cursor GetEnumerator(this Range& source){return Cursor(source.Limit);}}
        int Sum<T>(T& values) : Iterable<T,int> {int total=0;for(int value:values)total+=value;return total;}
        static class Program {public:void Main(){Range values;Print(Sum(values));
            int total=0;for(int value:values)total+=value;Print(total);}}
    )", "6\n6\n", ""},
    {"range-nested-break-continue", R"(
        struct Counter {public:int Current=0;bool MoveNext(){Current++;return Current<=4;}}
        class Range {public:Counter GetEnumerator(){return Counter();}}
        static class Program {public:void Main(){Range values;int result=0;
            for(int outer:values){if(outer==2)continue;if(outer==4)break;
                for(int inner:values){if(inner==2)continue;if(inner==4)break;result+=outer*inner;}}
            Print(result);}}
    )", "16\n", ""},
    {"iterable-failed-probe-does-not-activate-invalid-specialization", R"(
        class Range {}
        struct Cursor {public:bool MoveNext(){return false;}}
        Cursor GetEnumerator<T>(this T& value){return Missing;}
        int Pick<T>(T& value):Iterable<T>{return 1;}
        int Pick(Range& value){return 2;}
        static class Program {public:void Main(){Range value;Print(Pick(value));}}
    )", "2\n", ""},
    {"iterable-failed-element-probe-does-not-activate-invalid-specialization", R"(
        class Range {}
        struct Cursor {public:int Current=0;bool MoveNext(){return false;}}
        Cursor GetEnumerator<T>(this T& value){return Missing;}
        int Pick<T>(T& value):Iterable<T,string>{return 1;}
        int Pick(Range& value){return 2;}
        static class Program {public:void Main(){Range value;Print(Pick(value));}}
    )", "2\n", ""},
    {"iterable-failed-probe-specialization-can-be-used-later", R"(
        class Range {}
        struct Cursor {public:int Current=7;bool MoveNext(){return false;}}
        Cursor GetEnumerator<T>(this T& value){return Cursor();}
        int Pick<T>(T& value):Iterable<T,string>{return 1;}
        int Pick(Range& value){return 2;}
        static class Program {public:void Main(){Range value;Print(Pick(value));
            Cursor cursor=value.GetEnumerator();Print(cursor.Current);}}
    )", "2\n7\n", ""},
    {"iterable-failed-probe-does-not-activate-generic-cursor-body", R"(
        class Range {}
        struct Cursor<T> {public:bool MoveNext(){return false;}int Unused(){return Missing;}}
        Cursor<T> GetEnumerator<T>(this T& value){return Cursor<T>();}
        int Pick<T>(T& value):Iterable<T>{return 1;}
        int Pick(Range& value){return 2;}
        static class Program {public:void Main(){Range value;Print(Pick(value));}}
    )", "2\n", ""},
    {"iterable-failed-probe-does-not-hide-later-method-error", R"(
        class Range {}
        struct Cursor {public:bool MoveNext(){return false;}}
        Cursor GetEnumerator<T>(this T& value){return Missing;}
        int Pick<T>(T& value):Iterable<T>{return 1;}
        int Pick(Range& value){return 2;}
        static class Program {public:void Main(){Range value;Print(Pick(value));value.GetEnumerator();}}
    )", "", "unknown identifier 'Missing'"},
    {"iterator-typed-contract", R"(
        struct Counter {public:int Current=0;bool MoveNext(){Current++;return Current<=3;}}
        int Sum<T>(T& iterator) : Iterator<T,int> {int total=0;while(iterator.MoveNext())total+=iterator.Current;return total;}
        static class Program {public:void Main(){Counter iterator;Print(Sum(iterator));}}
    )", "6\n", ""},
    {"iterable-reference-elements", R"(
        import System.Collections.Generic
        void Bump<T>(T& values) : Iterable<T,int&> {for(int& value:values)value+=10;}
        static class Program {public:void Main(){int[] array=[1,2];Bump(array);Print(array[0]);
            List<int> list;list.Add(3);Bump(list);Print(list[0]);}}
    )", "11\n13\n", "", false},
    {"iterable-inherited-protocol", R"(
        struct Counter {public:int Current=0;bool MoveNext(){Current++;return Current<=2;}}
        class Base {public:Counter GetEnumerator(){return Counter();}}
        class Derived : Base {}
        int Sum<T>(T& values) : Iterable<T> {int total=0;for(int value:values)total+=value;return total;}
        static class Program {public:void Main(){Derived values;Print(Sum(values));}}
    )", "3\n", ""},
    {"iterable-missing-current-concept", R"(
        struct Cursor {public:bool MoveNext(){return false;}}
        class Range {public:Cursor GetEnumerator(){return Cursor();}}
        void Use<T>(T& value) : Iterable<T> {}
        static class Program {public:void Main(){Range values;Use(values);}}
    )", "", "satisfy"},
    {"iterable-nonbool-movenext-concept", R"(
        struct Cursor {public:int Current=0;int MoveNext(){return 0;}}
        class Range {public:Cursor GetEnumerator(){return Cursor();}}
        void Use<T>(T& value) : Iterable<T> {}
        static class Program {public:void Main(){Range values;Use(values);}}
    )", "", "satisfy"},
    {"iterable-private-getenumerator", R"(
        struct Cursor {public:int Current=0;bool MoveNext(){return false;}}
        class Range {private:Cursor GetEnumerator(){return Cursor();}}
        void Use<T>(T& value) : Iterable<T> {}
        static class Program {public:void Main(){Range values;Use(values);}}
    )", "", "satisfy"},
    {"iterator-private-current", R"(
        struct Cursor {private:int Current=0;public:bool MoveNext(){return false;}}
        void Use<T>(T& value) : Iterator<T> {}
        static class Program {public:void Main(){Cursor value;Use(value);}}
    )", "", "satisfy"},
    {"iterator-static-current-rejected", R"(
        struct Cursor {public:static int Current=0;bool MoveNext(){return false;}}
        void Use<T>(T& value) : Iterator<T> {}
        static class Program {public:void Main(){Cursor value;Use(value);}}
    )", "", "satisfy"},
    {"iterable-static-getenumerator-rejected", R"(
        struct Cursor {public:int Current=0;bool MoveNext(){return false;}}
        class Range {public:static Cursor GetEnumerator(){return Cursor();}}
        void Use<T>(T& value) : Iterable<T> {}
        static class Program {public:void Main(){Range values;Use(values);}}
    )", "", "satisfy"},
    {"iterable-recursive-extension-constraint-rejected", R"(
        struct Cursor {public:int Current=0;bool MoveNext(){return false;}}
        class Range {}
        static class Ranges {public:static Cursor GetEnumerator<T>(this T& value) : Iterable<T> {return Cursor();}}
        void Use<T>(T& value) : Iterable<T> {}
        static class Program {public:void Main(){Range values;Use(values);}}
    )", "", "satisfy"},
    {"iterator-reference-bool-movenext-rejected", R"(
        struct Cursor {public:int Current=0;bool More=false;bool& MoveNext(){return More;}}
        void Use<T>(T& value) : Iterator<T> {}
        static class Program {public:void Main(){Cursor value;Use(value);}}
    )", "", "satisfy"},
    {"iterator-void-current-rejected", R"(
        struct Cursor {public:bool MoveNext(){return false;}void Current {get {}}}
        void Use<T>(T& value) : Iterator<T> {}
        static class Program {public:void Main(){Cursor value;Use(value);}}
    )", "", "void"},
    {"iterable-wrong-element", R"(
        void Use<T>(T& value) : Iterable<T,string> {}
        static class Program {public:void Main(){int[] values=[1];Use(values);}}
    )", "", "satisfy"},
    {"iterable-value-element-cannot-satisfy-reference", R"(
        struct Cursor {public:int Current => 0;bool MoveNext(){return false;}}
        class Range {public:Cursor GetEnumerator(){return Cursor();}}
        void Use<T>(T& value) : Iterable<T,int&> {}
        static class Program {public:void Main(){Range values;Use(values);}}
    )", "", "satisfy"},
    {"range-nonbool-movenext", R"(
        struct Cursor {public:int Current=0;int MoveNext(){return 0;}}
        class Range {public:Cursor GetEnumerator(){return Cursor();}}
        static class Program {public:void Main(){Range values;for(int value:values){}}}
    )", "", "MoveNext() must return bool"},
    {"range-missing-current", R"(
        struct Cursor {public:bool MoveNext(){return false;}}
        class Range {public:Cursor GetEnumerator(){return Cursor();}}
        static class Program {public:void Main(){Range values;for(int value:values){}}}
    )", "", "has no member 'Current'"},
    {"range-element-type-must-match", R"(
        struct Cursor {public:int Current=0;bool MoveNext(){return false;}}
        class Range {public:Cursor GetEnumerator(){return Cursor();}}
        static class Program {public:void Main(){Range values;for(double value:values){}}}
    )", "", "range element is 'int', not 'double'"},
    {"range-const-array-cannot-borrow-mutably", R"(
        static class Program {public:void Main(){const int[] values=[1,2];for(int& value:values)value++;}}
    )", "", "const range cannot yield mutable references"},
    {"iterable-string-is-value-sequence", R"(
        void Use<T>(T& value) : Iterable<T,char&> {}
        static class Program {public:void Main(){string value="a";Use(value);}}
    )", "", "satisfy"}
};
