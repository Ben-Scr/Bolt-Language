// Exercise the current instance through the interpreter and every native backend.
struct ThisCase {
    const char* name;
    const char* source;
    const char* expected;
    const char* error;
};

static const ThisCase this_cases[] = {
    {"this-members-shadowing-and-constructors", R"(
        class Counter {public:
            int Value;
            Counter(int Value){this.Value=Value;}
            void Add(int Value){this.Value+=Value;}
            int Read(){return this.Value;}
            int Update(int Value){this.Add(Value);{int Value=2;this.Add(Value);}return this.Read();}
        }
        static class Program {public:void Main(){
            Counter first=Counter(4); Counter second=Counter(20);
            Print(first.Update(3)); Print(second.Read()); Print(first.Value);
        }}
    )", "9\n20\n9\n", ""},
    {"this-reference-and-pointer-aliases", R"(
        class Counter {public:
            int Value;
            Counter(int Value){this.Value=Value;}
            Counter& Self(){return this;}
            Counter* Address(){return &this;}
            void Update(){Counter& alias=this;alias.Value+=2;Counter* pointer=&this;pointer->Value+=3;}
        }
        static class Program {public:void Main(){
            Counter value=Counter(1); value.Update();
            Counter& alias=value.Self(); alias.Value+=4;
            Counter* pointer=value.Address(); pointer->Value+=5;
            Print(value.Value); Print(pointer==&value);
        }}
    )", "15\ntrue\n", ""},
    {"this-arguments-and-value-copies", R"(
        class Counter {public:
            int Value;
            Counter(int Value){this.Value=Value;}
            Counter Copy(){return this;}
            void Update(){Bump(this);TouchCopy(this);var copy=this;copy.Value=99;}
        }
        void Bump(Counter& value){value.Value+=3;}
        void TouchCopy(Counter value){value.Value=100;}
        static class Program {public:void Main(){
            Counter value=Counter(2); value.Update();
            Counter copy=value.Copy(); copy.Value=7;
            Print(value.Value); Print(copy.Value);
        }}
    )", "5\n7\n", ""},
    {"this-generic-and-inherited-receivers", R"(
        class Box<T> {public:
            T Value;
            Box(T Value){this.Value=Value;}
            Box<T>& Self(){return this;}
            T Read(){return this.Value;}
        }
        class Base {public:
            int Value;
            int Read(){return this.Value;}
            int Dispatch(){return this.Read();}
            void Set(int Value){this.Value=Value;}
        }
        class Child:Base {public:
            override int Read(){return this.Value+10;}
        }
        static class Program {public:void Main(){
            Box<int> box=Box<int>(3); Box<int>& alias=box.Self(); alias.Value=6;
            Print(box.Read());
            Child child; child.Set(4); Base& base=child;
            Print(child.Dispatch()); Print(base.Dispatch());
        }}
    )", "6\n14\n14\n", ""},
    {"this-static-method", R"(
        class Item {public:static void Invalid(){this;}}
        static class Program {public:void Main(){}}
    )", "", "non-static instance"},
    {"this-static-class-method", R"(
        static class Program {public:void Main(){this;}}
    )", "", "non-static instance"},
    {"this-global-function", R"(
        void Invalid(){this;}
        static class Program {public:void Main(){}}
    )", "", "non-static instance"},
    {"this-instance-field-initializer", R"(
        class Item {public:int Value=1;int Other=this.Value;}
        static class Program {public:void Main(){}}
    )", "", "non-static instance"},
    {"this-static-field-initializer", R"(
        class Item {public:int Value;static int Other=this.Value;}
        static class Program {public:void Main(){}}
    )", "", "non-static instance"},
    {"this-cannot-be-reassigned", R"(
        class Item {public:void Invalid(Item other){this=other;}}
        static class Program {public:void Main(){}}
    )", "", "assignment requires"},
    {"this-cannot-be-local-name", R"(
        class Item {public:void Invalid(){var this=1;}}
        static class Program {public:void Main(){}}
    )", "", "expected an identifier"},
    {"this-cannot-be-parameter-name", R"(
        class Item {public:void Invalid(int this){}}
        static class Program {public:void Main(){}}
    )", "", "expected an identifier"},
    {"this-cannot-be-field-name", R"(
        class Item {public:int this;}
        static class Program {public:void Main(){}}
    )", "", "expected an identifier"},
    {"this-cannot-have-type-arguments", R"(
        class Item {public:Item Copy(){return this<int>;}}
        static class Program {public:void Main(){}}
    )", "", "'this' is not generic"},
    {"this-member-cannot-have-receiver-type-arguments", R"(
        class Item {public:int Value;int Read(){return this<int>.Value;}}
        static class Program {public:void Main(){}}
    )", "", "'this' is not generic"},
    {"this-reference-return-rejects-temporary-receiver", R"(
        class Item {public:Item& Self(){return this;}}
        static class Program {public:void Main(){Item& alias=Item().Self();}}
    )", "", "temporary receiver"},
};
