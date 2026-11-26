package dev.bolt.idea.core;

import java.util.Arrays;
import java.util.List;

/** Folding ranges use editor offsets and need neither a compiler process nor the IntelliJ SDK. */
public final class FoldingTests {
    private static int count;

    private static BoltFolding.Range block(String source, String text) {
        int start = source.indexOf(text);
        if (start < 0 || source.indexOf(text, start + 1) >= 0)
            throw new AssertionError("Expected one occurrence of block: " + text);
        return new BoltFolding.Range(start, start + text.length());
    }

    private static void expect(CharSequence source, String name, BoltFolding.Range... expected) {
        List<BoltFolding.Range> actual = BoltFolding.ranges(source);
        if (!actual.equals(Arrays.asList(expected)))
            throw new AssertionError(name + ": expected " + Arrays.toString(expected) + ", got " + actual);
        count++;
        System.out.println("PASS " + name);
    }

    public static int run() {
        count = 0;
        String control = "{\n            Print(value);\n        }";
        String method = "{\n        if (value > 0) " + control + "\n    }";
        String getter = "{\n            return value;\n        }";
        String property = "{\n        get " + getter + "\n        set { value = value; }\n    }";
        String type = "{\n    void Run() " + method + "\n    int Value " + property + "\n}";
        String nested = "class Example " + type;
        expect(nested, "nested class, method, control flow and property folds are independent and ordered",
            block(nested, type), block(nested, method), block(nested, control),
            block(nested, property), block(nested, getter));

        String vec2 = """
            struct Vec2 : IStringable {
                float X, Y;

                public Vec2(float x, float y){
                    X = x;
                    Y = y;
                }

                public string ToString(){
                    return $"({X}, {Y})";
                }
            }

            public static class Program{
                DateTime dt => DateTime.Now;

                public static void Main(){
                    Vec2* vec2 = new Vec2(2f, 3f);
                    Print(vec2);
                    Print(&vec2);
                    Print(*vec2);

                    Print("Hello World");
                }
            }
            """;
        int structStart = vec2.indexOf('{');
        int structEnd = vec2.indexOf("\n}\n") + 2;
        int classStart = vec2.indexOf('{', structEnd);
        int constructorStart = vec2.indexOf('{', vec2.indexOf("public Vec2("));
        int constructorEnd = vec2.indexOf('}', constructorStart) + 1;
        int toStringStart = vec2.indexOf('{', vec2.indexOf("public string ToString"));
        int toStringEnd = vec2.indexOf("\n    }", toStringStart) + 6;
        int mainStart = vec2.indexOf('{', vec2.indexOf("public static void Main"));
        int mainEnd = vec2.indexOf('}', mainStart) + 1;
        expect(vec2, "Vec2 screenshot yields five folds without treating interpolation as a code block",
            new BoltFolding.Range(structStart, structEnd),
            new BoltFolding.Range(constructorStart, constructorEnd),
            new BoltFolding.Range(toStringStart, toStringEnd),
            new BoltFolding.Range(classStart, vec2.lastIndexOf('}') + 1),
            new BoltFolding.Range(mainStart, mainEnd));

        expect("", "empty editor buffer has no folds");
        expect("class Empty {} void Run() { Print(1); }", "single-line blocks remain visible");
        String mixed = "class Mixed {\n    void Empty() {}\n    void Run() { Print(1); }\n}";
        expect(mixed, "single-line children do not prevent the containing multiline fold",
            new BoltFolding.Range(mixed.indexOf('{'), mixed.length()));

        String literals = "void Run() {\n"
            + "    Print(\"{\\\"} still text {\");\n"
            + "    char open = '{'; char close = '}'; char quote = '\\'';\n"
            + "    // } { unmatched comment braces {{{\n"
            + "    /* }\n       { } } */\n"
            + "    Print($\"{Join(\"{\", \"}\")} {{literal}}\");\n"
            + "}";
        expect(literals, "escaped quotes, character literals, comments and interpolation cannot alter block matching",
            new BoltFolding.Range(literals.indexOf('{'), literals.length()));
        expect("// {\n// }\n/* {\n} */\nPrint(\"{\");\nPrint(\"}\");",
            "braces confined to comments and strings never create folds");
        String interpolation = "Print($\"{Build({\n    Value = \"}\";\n})}\");";
        expect(interpolation, "multiline expression inside interpolated string does not become a brace fold");

        expect("}\n{", "unmatched closing and opening braces have no folds");
        expect("class Editing {\n    void Pending() {\n", "unfinished blocks have no folds");
        String completeMethod = "{\n    Print(42);\n}";
        String unfinishedClass = "class Editing {\nvoid Ready() " + completeMethod;
        expect(unfinishedClass, "matched child remains foldable while enclosing class is unfinished",
            block(unfinishedClass, completeMethod));
        String extraClose = "}\nvoid Ready() " + completeMethod + "\n}";
        expect(extraClose, "stray closing braces do not suppress later complete blocks",
            block(extraClose, completeMethod));
        String partialString = "void Editing() {\n    Print(\"unterminated {\n}";
        expect(partialString, "incomplete ordinary string recovers at the newline during editing",
            new BoltFolding.Range(partialString.indexOf('{'), partialString.length()));
        expect("void Editing() {\n    /* }\n", "unterminated block comment cannot close a code block");

        for (String newline : List.of("\n", "\r\n", "\r")) {
            String unicode = "// 🧪 Unicode prefix" + newline + "class Ä {" + newline + "    Print(\"🧪\");" + newline + "}";
            expect(unicode, "UTF-16 offsets preserve supplementary characters with line ending " + newline.replace("\r", "CR").replace("\n", "LF"),
                new BoltFolding.Range(unicode.indexOf('{'), unicode.length()));
        }
        StringBuilder edited = new StringBuilder("void Run() {\n    Print(42);\n}");
        expect(edited, "folding accepts the editor's CharSequence buffer",
            new BoltFolding.Range(edited.indexOf("{"), edited.length()));
        edited.deleteCharAt(edited.length() - 1);
        expect(edited, "removing the closing brace removes the old fold on recomputation");
        edited.append('}');
        expect(edited, "restoring the closing brace restores the fold",
            new BoltFolding.Range(edited.indexOf("{"), edited.length()));
        return count;
    }
}
