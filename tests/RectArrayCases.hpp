// Rectangular array programs shared across interpreted and native execution.
struct RectArrayCase {
    const char* name;
    const char* source;
    const char* expected;
    const char* error;
    bool runtime_error;
};

static const RectArrayCase rect_array_cases[] = {
    {"rect-array-literals-and-rank", R"(
        static class Program {public:void Main(){
            int[,] matrix={{1,2,3},{4,5,6}};
            int[,,] cube={{{1,2},{3,4}},{{5,6},{7,8}}};
            int[,,,,] wide=new int[1,1,1,1,2];wide[0,0,0,0,1]=9;
            Print(matrix[1,0]+","+matrix[0,2]+","+matrix.Length+","+matrix.Count+","+matrix.Rank);
            Print(matrix.GetLength(0)+","+matrix.GetLength(1));
            Print(cube[1,0,1]+","+cube[1,1,1]+","+cube.Length+","+cube.Rank);
            Print(wide[0,0,0,0,1]+","+wide.Rank+","+wide.GetLength(4));
        }}
    )", "4,3,6,6,2\n2,3\n6,8,8,3\n9,5,2\n", "", false},
    {"rect-array-dynamic-sizes-and-evaluation-order", R"(
        static class Program {public:
            int Next=0;
            int Size(){return ++Next;}
            void Main(){
                int[,] matrix=new int[Size(),Size()];
                Print(Next+","+matrix.GetLength(0)+","+matrix.GetLength(1));
                int[,] values=new int[2,2];int index=0;
                values[index++,index++]=11;Print(index+","+values[0,1]);
                index=0;values[index++,index++]+=++index;
                Print(index+","+values[0,1]);
            }
        }
    )", "2,1,2\n2,11\n3,14\n", "", false},
    {"rect-array-copy-and-element-references", R"(
        void ChangeCopy(int[,] values){values[0,0]=100;}
        void ChangeReference(int[,]& values){values[0,1]=20;}
        int& Element(int[,]& values){return values[1,1];}
        static class Program {public:void Main(){
            int[,] source={{1,2},{3,4}};int[,] copy=source;
            copy[0,0]=9;ChangeCopy(source);ChangeReference(source);
            int& alias=Element(source);alias+=6;int* pointer=&source[1,0];*pointer=8;
            Print(source[0,0]+","+source[0,1]+","+source[1,0]+","+source[1,1]);
            Print(copy[0,0]+","+copy[0,1]+","+copy.GetLength(0)+","+copy.GetLength(1));
        }}
    )", "1,20,8,10\n9,2,2,2\n", "", false},
    {"rect-array-class-string-and-default-elements", R"(
        class Item {public:int Value=7;Item(){Value++;}}
        static class Program {public:void Main(){
            Item[,] objects=new Item[1,2];objects[0,0].Value=11;
            Item[,] copy=objects;copy[0,1].Value=20;
            string[,] names={{"first",default},{default,"last"}};
            bool[,] flags=new bool[2,1];flags[1,0]=true;
            Print(objects[0,0].Value+","+objects[0,1].Value+","+copy[0,1].Value);
            Print(names[0,0]+",["+names[0,1]+"],["+names[1,0]+"],"+names[1,1]);
            Print(flags[0,0]+","+flags[1,0]);
        }}
    )", "11,8,20\nfirst,[],[],last\nfalse,true\n", "", false},
    {"rect-array-empty-dimensions-and-default", R"(
        static class Program {public:void Main(){
            int[,] empty=default;int[,,] cube;
            int[,] rows=new int[2,0];int[,] columns=new int[0,3];
            Print(empty.Length+","+empty.Rank+","+empty.GetLength(0)+","+empty.GetLength(1));
            Print(cube.Length+","+cube.Rank+","+cube.GetLength(0)+","+cube.GetLength(1)+","+cube.GetLength(2));
            Print(rows.Length+","+rows.GetLength(0)+","+rows.GetLength(1));
            Print(columns.Length+","+columns.GetLength(0)+","+columns.GetLength(1));
            rows=default;Print(rows.GetLength(0)+","+rows.GetLength(1));
        }}
    )", "0,2,0,0\n0,3,0,0,0\n0,2,0\n0,0,3\n0,0\n", "", false},
    {"rect-array-fields-generic-types-and-returns", R"(
        import Type
        class Grid<T> {public:T[,] Values;}
        class Holder {public:int[,] Values={{1,2},{3,4}};static int[,] Shared={{5,6}};}
        T Identity<T>(T value){return value;}
        int[,] Make(){return {{7,8}};}
        static class Program {public:void Main(){
            Holder holder;Grid<int> grid;grid.Values=Identity(holder.Values);
            grid.Values[1,0]=9;int[,] returned=Make();var inferred=new int[2,3];
            Print(grid.Values[1,0]+","+holder.Values[1,0]+","+Holder.Shared[0,1]+","+returned[0,1]);
            Print(Type.Name<int[,]>());Print(inferred.Rank+","+inferred.Length);
        }}
    )", "9,3,6,8\nint[,]\n2,6\n", "", false},
    {"rect-array-shape-equality-and-hashing", R"(
        import System.Collections.Generic
        static class Program {public:void Main(){
            int[,] first={{1,2},{3,4}};int[,] copy=first;int[,] flat={{1,2,3,4}};
            Print(first==copy);Print(first==flat);copy[1,1]=8;Print(first!=copy);
            int[,] columns=new int[0,3];int[,] other=new int[0,4];int[,] empty=default;
            Print(columns!=other);Print(columns!=empty);
            HashSet<int[,]> shapes;shapes.Add(columns);shapes.Add(other);shapes.Add(columns);
            Print(shapes.Count);
        }}
    )", "true\nfalse\ntrue\ntrue\ntrue\n2\n", "", false},
    {"rect-array-preserves-jagged-and-vector-arrays", R"(
        static class Program {public:void Main(){
            int[][] jagged={{1,2},{3}};int[] vector={4,5};
            Print(jagged.Rank+","+jagged.GetLength(0)+","+jagged[1].Length+","+jagged[0][1]);
            Print(vector.Rank+","+vector.GetLength(0)+","+vector[1]);
            var inferred={{6,7},{8}};Print(inferred.Rank+","+inferred[1].Length);
        }}
    )", "1,2,1,2\n1,2,5\n1,1\n", "", false},
    {"rect-array-mixed-jagged-types-and-empty-literals", R"(
        static class Program {public:void Main(){
            int[][,] rows={{{1},{2,3}}};
            int[,][] groups={{{1,2}},{{3},{4}}};
            Print(rows.Rank+","+rows.GetLength(0)+","+rows.GetLength(1)+","+rows[0,1][1]);
            Print(groups.Rank+","+groups[0].Rank+","+groups[0].GetLength(1)+","+groups[1].GetLength(0)+","+groups[1][1,0]);
            int[,] empty={};int[,] zeroWidth={{},{}};
            Print(empty.Length+","+empty.GetLength(0)+","+empty.GetLength(1));
            Print(zeroWidth.Length+","+zeroWidth.GetLength(0)+","+zeroWidth.GetLength(1));
        }}
    )", "2,1,2,3\n1,2,2,2,4\n0,0,0\n0,2,0\n", "", false},
    {"rect-array-ragged-initializer", R"(
        static class Program {public:void Main(){int[,] values={{1,2},{3}};}}
    )", "", "rectangular array initializer", false},
    {"rect-array-missing-braces", R"(
        static class Program {public:void Main(){int[,] values={1,2};}}
    )", "", "rectangular array initializer", false},
    {"rect-array-excess-braces", R"(
        static class Program {public:void Main(){int[,] values={{{1,2}}};}}
    )", "", "rectangular array initializer", false},
    {"rect-array-too-few-indices", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values[0]);}}
    )", "", "array rank", false},
    {"rect-array-too-many-indices", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values[0,0,0]);}}
    )", "", "array rank", false},
    {"rect-array-rank-conversion", R"(
        static class Program {public:void Main(){int[,] values=new int[2];}}
    )", "", "cannot assign", false},
    {"rect-array-generic-inference-preserves-rank", R"(
        T First<T>(T[,] values){return values[0,0];}
        static class Program {public:void Main(){int[] values={1,2};Print(First(values));}}
    )", "", "no matching overload", false},
    {"rect-array-floating-size", R"(
        static class Program {public:void Main(){int[,] values=new int[2,1.5];}}
    )", "", "array sizes must be integers", false},
    {"rect-array-floating-index", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values[0,1.5]);}}
    )", "", "array indices must be integers", false},
    {"rect-array-floating-axis", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values.GetLength(1.5));}}
    )", "", "array dimension must be an integer", false},
    {"rect-array-getlength-argument-count", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values.GetLength());}}
    )", "", "GetLength expects one", false},
    {"rect-array-const-element", R"(
        static class Program {public:void Main(){const int[,] values={{1,2}};values[0,1]=3;}}
    )", "", "const", false},
    {"rect-array-element-needs-constructor", R"(
        class Item {public:Item(int value){}}
        static class Program {public:void Main(){Item[,] values=new Item[1,2];}}
    )", "", "no matching overload", false},
    {"rect-array-negative-size", R"(
        static class Program {public:void Main(){int columns=-1;int[,] values=new int[2,columns];}}
    )", "", "array dimensions must be non-negative", true},
    {"rect-array-row-bounds", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values[2,0]);}}
    )", "", "out of range", true},
    {"rect-array-column-bounds", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values[0,2]);}}
    )", "", "out of range", true},
    {"rect-array-negative-index", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values[0,-1]);}}
    )", "", "out of range", true},
    {"rect-array-axis-bounds", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values.GetLength(2));}}
    )", "", "out of range", true},
    {"rect-array-negative-axis", R"(
        static class Program {public:void Main(){int[,] values=new int[2,2];Print(values.GetLength(-1));}}
    )", "", "out of range", true},
};
