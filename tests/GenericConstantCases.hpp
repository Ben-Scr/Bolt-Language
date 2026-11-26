// These programs run through both the interpreter and every available native backend.
struct GenericConstantCase {
    const char* name;
    const char* source;
    const char* expected;
    const char* error;
};

static const GenericConstantCase generic_constant_cases[] = {
    {"math-generic-constant-types", R"(
        import Math
        import Type
        string Name<T>(T value){return Type.Name<T>();}
        static class Program {public:void Main(){
            Print(Name(Math.Pi)+","+Name(Math.E)+","+Name(Math.Tau));
            Print(Name(Math.Pi<float>)+","+Name(Math.E<float>)+","+Name(Math.Tau<float>));
            Print(Name(Math.Pi<double>)+","+Name(Math.E<double>)+","+Name(Math.Tau<double>));
            Print(Math.Pi==Math.Pi<double> && Math.E==Math.E<double> && Math.Tau==Math.Tau<double>);
        }}
    )", "double,double,double\nfloat,float,float\ndouble,double,double\ntrue\n", ""},
    {"math-generic-constant-bits", R"(
        import Math
        static class Program {public:void Main(){
            Print(Reinterpret<uint>(Math.Pi<float>)); Print(Reinterpret<ulong>(Math.Pi<double>));
            Print(Reinterpret<uint>(Math.E<float>)); Print(Reinterpret<ulong>(Math.E<double>));
            Print(Reinterpret<uint>(Math.Tau<float>)); Print(Reinterpret<ulong>(Math.Tau<double>));
        }}
    )", "1078530011\n4614256656552045848\n1076754516\n4613303445314885481\n1086918619\n4618760256179416344\n", ""},
    {"generic-constant-declarations-and-composition", R"(
        import Math
        import Type
        const T Pi<T> : FloatingPoint = (T)Math.Pi;
        const string Label<T> = "constant";
        static class Constants {public:
            const T Tau<T> : FloatingPoint = Pi<T> + Pi<T>;
            T Read<T>() : FloatingPoint {return Tau<T>;}
        }
        class Explicit {public:static const T E<T> : FloatingPoint = (T)Math.E;}
        string Name<T>(T value){return Type.Name<T>();}
        static class Program {public:void Main(){
            Print(Name(Pi<float>)+","+Name(Pi<double>));
            Print(Pi<float>==Math.Pi<float> && Pi<double>==Math.Pi<double>);
            Print(Constants.Tau<float>==Math.Tau<float>);
            Print(Constants.Read<double>()==Math.Tau<double>);
            Print(Explicit.E<float>==Math.E<float> && Explicit.E<double>==Math.E<double>);
            Print(Label<int>);
        }}
    )", "float,double\ntrue\ntrue\ntrue\ntrue\nconstant\n", ""},
    {"generic-constants-in-generic-classes-and-inheritance", R"(
        import Math
        class Constants {public:static const T Pi<T> : FloatingPoint = (T)Math.Pi;}
        class Inherited : Constants {}
        class Box<U> {public:
            static const T Pi<T> : FloatingPoint = (T)Math.Pi;
            static const T Largest<T> : Number = (T)U.MaxValue;
        }
        static class Program {public:void Main(){
            Print(Inherited.Pi<float>==Math.Pi<float>);
            Print(Box<int>.Pi<double>==Math.Pi<double>);
            Print(Box<short>.Largest<int>);
            Print(Box<byte>.Largest<double>);
        }}
    )", "true\ntrue\n32767\n255\n", ""},
    {"generic-constant-field-initializers", R"(
        import Math
        const T Pi<T> : FloatingPoint = (T)Math.Pi;
        class Circle {public:
            float First = Math.Pi<float>;
            double Second = Pi<double>;
            static float Third = Pi<float> * 2f;
        }
        static class Program {public:void Main(){
            Circle circle;
            Print(circle.First==Math.Pi<float>);
            Print(circle.Second==Math.Pi<double>);
            Print(Circle.Third==Math.Tau<float>);
        }}
    )", "true\ntrue\ntrue\n", ""},
    {"generic-constant-expression-boundaries", R"(
        import Math
        const T Pi<T> : FloatingPoint = (T)Math.Pi;
        T Identity<T>(T value){return value;}
        static class Program {public:void Main(){
            Print(Pi<float> > 3f && Pi<double> < 4.0);
            Print(Pi<float> + Pi<float> == Math.Tau<float>);
            Print(Identity<double>(Pi<double>)==Math.Pi<double>);
            Print(-Pi<float> < -3f);
        }}
    )", "true\ntrue\ntrue\ntrue\n", ""},
    {"generic-constant-adjacent-assignment-and-equality", R"(
        const T Value<T>=(T)1;
        static class Program {public:void Main(){
            Print(Value<int>==1);
            int a=2; int b=1;
            Print(a>=b); Print(b>=a);
        }}
    )", "true\ntrue\nfalse\n", ""},
    {"math-constant-invalid-type", "import Math\nstatic class Program {public:void Main(){Print(Math.Pi<int>);}}", "", "FloatingPoint"},
    {"math-constant-wrong-arity", "import Math\nstatic class Program {public:void Main(){Print(Math.Tau<float,double>);}}", "", "type argument"},
    {"math-constant-repeated-type-arguments", "import Math\nstatic class Program {public:void Main(){Print(Math.Pi<float><double>);}}", "", "expected"},
    {"math-constant-write", "import Math\nstatic class Program {public:void Main(){Math.Pi<float> = 3f;}}", "", "assignment requires"},
    {"math-constant-increment", "import Math\nstatic class Program {public:void Main(){Math.E<double>++;}}", "", "assignment requires"},
    {"math-constant-address", "import Math\nstatic class Program {public:void Main(){float* value=&Math.Pi<float>;}}", "", "lvalue"},
    {"math-constant-call", "import Math\nstatic class Program {public:void Main(){Print(Math.Pi<float>());}}", "", "not callable"},
    {"generic-constant-constraint", "const T Value<T> : FloatingPoint = (T)1; static class Program {public:void Main(){Print(Value<int>);}}", "", "does not satisfy concept"},
    {"generic-constant-wrong-arity", "const T Value<T> = (T)1; static class Program {public:void Main(){Print(Value<int,double>);}}", "", "no matching overload"},
    {"generic-constant-missing-type-arguments", "const T Value<T> = (T)1; static class Program {public:void Main(){Print(Value);}}", "", "requires explicit type arguments"},
    {"generic-constant-call", "const T Value<T> = (T)1; static class Program {public:void Main(){Print(Value<int>());}}", "", "omit the call parentheses"},
    {"generic-constant-write", "const T Value<T> = (T)1; static class Program {public:void Main(){Value<int> = 2;}}", "", "assignment requires"},
    {"generic-constant-increment", "const T Value<T> = (T)1; static class Program {public:void Main(){Value<int>++;}}", "", "assignment requires"},
    {"generic-constant-address", "const T Value<T> = (T)1; static class Program {public:void Main(){int* value=&Value<int>;}}", "", "lvalue"},
    {"generic-constant-needs-static", "class Constants {public:const T Value<T> = (T)1;} static class Program {public:void Main(){}}", "", "generic constants must be static"},
    {"generic-constant-no-pack", "const int Value<T...> = 1; static class Program {public:void Main(){}}", "", "generic constants require fixed type parameters"},
    {"generic-constant-private", "class Constants {static const T Value<T> = (T)1;} static class Program {public:void Main(){Print(Constants.Value<int>);}}", "", "private"},
    {"generic-constant-recursion", "const T Value<T> = Value<T>; static class Program {public:void Main(){Print(Value<int>);}}", "", "recursive generic constant initializer"},
    {"generic-constant-impure-initializer", "int Next(){return 1;} const T Value<T> = (T)Next(); static class Program {public:void Main(){Print(Value<int>);}}", "", "generic constant initializers must be side-effect-free"},
    {"generic-constant-constructor-initializer", "class Item {public:Item(){}} const T Value<T> = T(); static class Program {public:void Main(){Item value=Value<Item>;}}", "", "generic constant initializers must be side-effect-free"},
    {"generic-constant-operator-initializer", R"(
        class Item {public:
            Item(){}
            static Item operator +(Item a,Item b){return Item();}
        }
        const T Value<T> = T() + T();
        static class Program {public:void Main(){Item value=Value<Item>;}}
    )", "", "generic constant initializers must be side-effect-free"},
};
