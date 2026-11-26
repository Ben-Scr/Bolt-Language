// Parsing contracts shared by the interpreter and both direct native backends.
struct NumericParsingCase {
    std::string name;
    std::string source;
    std::string expected;
    std::string error;
    bool execute = false;
};

static std::vector<NumericParsingCase> numeric_parsing_cases() {
    std::vector<NumericParsingCase> cases = {
        {"numeric-parse-canonical-types", R"(
            import Type
            string Name<T>(T value){return Type.Name<T>();}
            static class Program {public:void Main(){
                Print(Name(short.Parse("1"))+","+Name(short.Parse("1")));
                Print(Name(int.Parse("1"))+","+Name(long.Parse("1")));
                Print(Name(byte.Parse("1"))+","+Name(ushort.Parse("1")));
                Print(Name(uint.Parse("1"))+","+Name(ulong.Parse("1")));
                Print(Name(float.Parse("1"))+","+Name(double.Parse("1")));
                var signedAlias=int.Parse("-42"); var unsignedAlias=uint.Parse("42");
                Print(Name(signedAlias)+","+Name(unsignedAlias));
                Print(signedAlias+","+unsignedAlias);
                int signedValue; uint unsignedValue;
                Print(int.TryParse("7",signedValue)); Print(uint.TryParse("8",unsignedValue));
                Print(signedValue+","+unsignedValue);
            }}
        )", "short,short\nint,long\nbyte,ushort\nuint,ulong\nfloat,double\nint,uint\n-42,42\ntrue\ntrue\n7,8\n", ""},
        {"numeric-parse-integer-boundaries", R"(
            static class Program {public:void Main(){
                Print(short.Parse("-128")+","+short.Parse("127"));
                Print(short.Parse("-32768")+","+short.Parse("32767"));
                Print(int.Parse("-2147483648")+","+int.Parse("2147483647"));
                Print(long.Parse("-9223372036854775808")+","+long.Parse("9223372036854775807"));
                Print(byte.Parse("0")+","+byte.Parse("255"));
                Print(ushort.Parse("0")+","+ushort.Parse("65535"));
                Print(uint.Parse("0")+","+uint.Parse("4294967295"));
                Print(ulong.Parse("0")+","+ulong.Parse("18446744073709551615"));
                Print(long.Parse("9007199254740993")); Print(ulong.Parse("9007199254740993"));
            }}
        )", "-128,127\n-32768,32767\n-2147483648,2147483647\n-9223372036854775808,9223372036854775807\n0,255\n0,65535\n0,4294967295\n0,18446744073709551615\n9007199254740993\n9007199254740993\n", ""},
        {"numeric-parse-decimal-grammar", R"(
            static class Program {public:void Main(){
                Print(int.Parse(" \t\r\n+0042\n\r\t ")); Print(int.Parse("-0"));
                Print(ulong.Parse(" +00042 "));
                Print(float.Parse(" +1.25e+2 ")); Print(double.Parse("-.5E1"));
                Print(double.Parse(".125")); Print(float.Parse("2."));
                Print(Reinterpret<uint>(float.Parse("-0")));
                Print(Reinterpret<ulong>(double.Parse("-0.0")));
                Print(float.Parse("3.4028234663852886e38")==float.MaxValue);
                Print(double.Parse("1.7976931348623157e308")==double.MaxValue);
                Print(Reinterpret<uint>(float.Parse("1.401298464324817e-45")));
                Print(Reinterpret<ulong>(double.Parse("4.9406564584124654e-324")));
            }}
        )", "42\n0\n42\n125\n-5\n0.125\n2\n2147483648\n9223372036854775808\ntrue\ntrue\n1\n1\n", ""},
        {"numeric-tryparse-reference-destinations", R"(
            import System.Collections.Generic
            class Item {public:int Value=2; static int Shared=3;}
            int& Ref(int& value){return value;}
            static class Program {public:void Main(){
                Item item; int[] values={4,5}; List<int> list; list.Add(6);
                int value=1; int& alias=value; int* pointer=&value;
                Print(int.TryParse("10",alias)); Print(int.TryParse("11",*pointer));
                Print(int.TryParse("12",item.Value)); Print(int.TryParse("13",Item.Shared));
                Print(int.TryParse("14",values[1])); Print(int.TryParse("15",list[0]));
                Print(int.TryParse("16",Ref(value)));
                Print(value+","+item.Value+","+Item.Shared+","+values[0]+","+values[1]+","+list[0]);
                Print(int.TryParse("bad",values[1])); Print(values[1]);
            }}
        )", "true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n16,12,13,4,14,15\nfalse\n14\n", ""},
        {"numeric-tryparse-argument-order", R"(
            string Input(int& calls,int& index){calls++;index=1;return "17";}
            int& Destination(int[]& values,int& calls,int& index){calls+=10;return values[index++];}
            int& MutateInput(string& text,int& value){text="bad";return value;}
            static class Program {public:void Main(){
                int calls=0; int index=0; int[] values={1,2,3};
                Print(int.TryParse(Input(calls,index),Destination(values,calls,index)));
                Print(calls+","+index+","+values[0]+","+values[1]+","+values[2]);
                string text="41"; int value=9;
                Print(int.TryParse(text,MutateInput(text,value))); Print(text+","+value);
                index=0; Print(int.TryParse("bad",values[index++])); Print(index+","+values[0]);
            }}
        )", "true\n11,2,1,17,3\ntrue\nbad,41\nfalse\n1,1\n", ""},
        {"numeric-parse-generic-types", R"(
            T Parse<T>(string text) : Number {return T.Parse(text);}
            bool TryParse<T>(string text,T& value) : Number {return T.TryParse(text,value);}
            static class Program {public:void Main(){
                Print(Parse<short>("-128")); Print(Parse<ulong>("18446744073709551615"));
                Print(Parse<float>("1.5")); double value=3.0;
                Print(TryParse("2.5e1",value)); Print(value);
                Print(TryParse("bad",value)); Print(value);
            }}
        )", "-128\n18446744073709551615\n1.5\ntrue\n25\nfalse\n25\n", ""},
    };
    auto program=[](const std::string& body){return "static class Program {public:void Main(){"+body+"}}";};
    struct IntegerRange { const char* type; const char* lowest; const char* highest; const char* below; const char* above; };
    const IntegerRange ranges[] = {
        
        {"short", "-32768", "32767", "-32769", "32768"},
        {"int", "-2147483648", "2147483647", "-2147483649", "2147483648"},
        {"long", "-9223372036854775808", "9223372036854775807", "-9223372036854775809", "9223372036854775808"},
        {"byte", "0", "255", "-1", "256"},
        {"ushort", "0", "65535", "-1", "65536"},
        {"uint", "0", "4294967295", "-1", "4294967296"},
        {"ulong", "0", "18446744073709551615", "-1", "18446744073709551616"},
    };
    for(const auto& range:ranges) {
        const std::string type=range.type;
        cases.push_back({"numeric-tryparse-range-"+type, program(type+" value=7;"
            "Print("+type+".TryParse(\""+range.lowest+"\",value));Print(value);"
            "Print("+type+".TryParse(\""+range.highest+"\",value));Print(value);"
            "Print("+type+".TryParse(\""+range.below+"\",value));Print(value);"
            "Print("+type+".TryParse(\""+range.above+"\",value));Print(value);"),
            "true\n"+std::string(range.lowest)+"\ntrue\n"+range.highest+"\nfalse\n"+range.highest+"\nfalse\n"+range.highest+"\n", ""});
    }
    // Use escaped Bolt strings so embedded NUL is tested without truncating the source.
    const char* malformed[] = {"", " \\t\\n", "+", "-", "++1", "+-1", "--1", "1 2", "42abc", "1,000", "0x10", "42\\0", "\\042", "42u", "3.5f", ".", "1e", "1e+", "1e-", "NaN", "Infinity", "-inf"};
    const char* numeric_types[] = {"short", "short", "int", "long", "byte", "ushort", "uint", "ulong", "float", "double"};
    for(const std::string type:numeric_types) {
        std::string body=type+" value=7;", expected;
        for(const char* text:malformed) {
            body+="Print("+type+".TryParse(\""+text+"\",value));Print(value);";
            expected+="false\n7\n";
        }
        if(type!="float"&&type!="double") {
            for(const char* text:{"3.5", "1e2", "1.0"}) {
                body+="Print("+type+".TryParse(\""+text+"\",value));Print(value);";
                expected+="false\n7\n";
            }
        }
        if(type=="byte"||type=="ushort"||type=="uint"||type=="ulong") {
            body+="Print("+type+".TryParse(\"-0\",value));Print(value);";
            expected+="false\n7\n";
        }
        cases.push_back({"numeric-tryparse-invalid-text-"+type, program(body), expected, ""});
    }
    for(const std::string type:{"float", "double"}) {
        cases.push_back({"numeric-tryparse-float-range-"+type, program(type+" value=7;"
            "Print("+type+".TryParse(\"1.25e2\",value));Print(value);"
            "Print("+type+".TryParse(\"1e999\",value));Print(value);"
            "Print("+type+".TryParse(\"-1e999\",value));Print(value);"
            "Print("+type+".TryParse(\"1e-999\",value));Print(value);"),
            "true\n125\nfalse\n125\nfalse\n125\nfalse\n125\n", ""});
    }
    struct InvalidParse { const char* name; const char* expression; };
    const InvalidParse failures[] = {
        {"empty", "int.Parse(\"\")"},
        {"junk", "int.Parse(\"42abc\")"},
        {"fractional-integer", "int.Parse(\"3.5\")"},
        {"integer-overflow", "long.Parse(\"9223372036854775808\")"},
        {"unsigned-overflow", "ulong.Parse(\"18446744073709551616\")"},
        {"unsigned-negative", "uint.Parse(\"-0\")"},
        {"floating-overflow", "float.Parse(\"1e39\")"},
        {"floating-underflow", "double.Parse(\"1e-999\")"},
        {"nonfinite", "double.Parse(\"NaN\")"},
    };
    for(const auto& failure:failures)
        cases.push_back({"numeric-parse-error-"+std::string(failure.name),program("Print("+std::string(failure.expression)+");"),"","numeric text",true});
    const NumericParsingCase semantic[] = {
        {"numeric-parse-missing-input", program("int.Parse();"), "", "argument"},
        {"numeric-parse-extra-input", program("int.Parse(\"1\",\"2\");"), "", "argument"},
        {"numeric-tryparse-missing-output", program("int.TryParse(\"1\");"), "", "argument"},
        {"numeric-parse-input-type", program("int.Parse(1);"), "", "string"},
        {"numeric-tryparse-input-type", program("int value;int.TryParse(1,value);"), "", "string"},
        {"numeric-tryparse-output-type", program("long value;int.TryParse(\"1\",value);"), "", "reference"},
        {"numeric-tryparse-output-literal", program("int.TryParse(\"1\",0);"), "", "reference"},
        {"numeric-tryparse-output-expression", program("int value;int.TryParse(\"1\",value+1);"), "", "reference"},
        {"numeric-tryparse-output-const", program("const int value=7;int.TryParse(\"1\",value);"), "", "reference"},
        {"numeric-parse-not-generic", program("int.Parse<int>(\"1\");"), "", "not generic"},
        {"numeric-parse-not-instance-method", program("int value;value.Parse(\"1\");"), "", "member"},
    };
    cases.insert(cases.end(),std::begin(semantic),std::end(semantic));
    for(const std::string legacy:{"int8","int16","int32","int64","uint8","uint16","uint32","uint64","sbyte","i8","u8","i16","u16","i32","u32","i64","u64"})
        cases.push_back({"removed-primitive-"+legacy,program(legacy+" value;"),"","unknown type"});
    cases.push_back({"canonical-integer-concepts-and-suffixes",R"(
        import Type
        bool UnsignedValue<T>(T value) : Unsigned<T> { return Type.IsUnsigned<T>(); }
        bool SignedValue<T>(T value) : Signed<T> { return !Type.IsUnsigned<T>(); }
        string Name<T>(T value) : Integer<T> { return Type.Name<T>(); }
        static class Program {public:void Main(){
            Print(UnsignedValue(byte.MaxValue)); Print(UnsignedValue(ushort.MaxValue));
            Print(UnsignedValue(uint.MaxValue)); Print(UnsignedValue(ulong.MaxValue));
            Print(SignedValue(short.MinValue)); Print(SignedValue(int.MinValue)); Print(SignedValue(long.MinValue));
            Print(Name(1u)+","+Name(1l)+","+Name(1ul));
            Print(Name(byte(255)+ushort(65535))); Print(Name(uint.MaxValue+int(-1)));
        }}
    )","true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\nuint,long,ulong\nint\nlong\n",""});
    return cases;
}
