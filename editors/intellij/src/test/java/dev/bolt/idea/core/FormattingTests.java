package dev.bolt.idea.core;

import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Formatter invariants use the actual lexer and UTF-16 document edits, without an IDE. */
public final class FormattingTests {
    private static int count;
    private record Lexeme(BoltScanner.Kind kind, String text) { }

    private FormattingTests() { }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static List<Lexeme> lexemes(String source) {
        var result = new ArrayList<Lexeme>();
        for (var token : BoltScanner.scan(source)) {
            if (token.kind() != BoltScanner.Kind.SPACE)
                result.add(new Lexeme(token.kind(), source.substring(token.start(), token.end())));
        }
        return result;
    }

    private static boolean whitespace(String text) {
        for (int i = 0; i < text.length(); i++) if (!Character.isWhitespace(text.charAt(i))) return false;
        return true;
    }

    private static String verify(String source, String name) {
        String result = BoltFormatting.format(source);
        require(lexemes(source).equals(lexemes(result)), name + ": formatter changed a token or literal");
        require(result.equals(BoltFormatting.format(result)), name + ": second formatting changed the result");
        var edits = BoltFormatting.edits(source);
        int previousEnd = 0;
        for (var edit : edits) {
            require(edit.start() >= previousEnd && edit.end() >= edit.start() && edit.end() <= source.length(),
                name + ": edits must be ascending, in bounds and non-overlapping");
            require(whitespace(source.substring(edit.start(), edit.end())) && whitespace(edit.replacement()),
                name + ": edits must only replace whitespace");
            previousEnd = edit.end();
        }
        var applied = new StringBuilder(source);
        for (int i = edits.size() - 1; i >= 0; i--) {
            var edit = edits.get(i);
            applied.replace(edit.start(), edit.end(), edit.replacement());
        }
        require(result.contentEquals(applied), name + ": applying returned edits must equal format(source)");
        require(BoltFormatting.edits(result).isEmpty(), name + ": an already formatted document needs no edits");
        return result;
    }

    private static void passed(String name) {
        count++;
        System.out.println("PASS " + name);
    }

    private static void fixture(String source, String name) {
        verify(source, name);
        passed(name);
    }

    private static void expect(String source, String expected, String name) {
        String actual = verify(source, name);
        require(actual.stripTrailing().equals(expected.stripTrailing()),
            name + ": unexpected layout\nExpected:\n" + expected + "\nActual:\n" + actual);
        passed(name);
    }

    private static void interpolation(String source, String name) {
        var tokens = BoltScanner.scan(source + ";");
        require(tokens.size() == 2 && tokens.get(0).kind() == BoltScanner.Kind.STRING
            && tokens.get(0).start() == 0 && tokens.get(0).end() == source.length()
            && tokens.get(1).kind() == BoltScanner.Kind.SEMICOLON,
            name + ": comments in interpolation must not terminate the string or consume the following token");
        fixture("class C{void Main(){Print(" + source + ");}}", name);
    }

    public static int run() {
        count = 0;
        expect("public class Sample {public:int Value; void Run(){if (Value > 0){Print(Value);}}}", """
            public class Sample
            {
            public:
                int Value;
                void Run()
                {
                    if (Value > 0)
                    {
                        Print(Value);
                    }
                }
            }
            """, "Allman braces, four spaces and outdented class access labels");

        expect("import System\nimport System.Collections.Generic\nclass C{public:int value;}", """
            import System
            import System.Collections.Generic
            class C
            {
            public:
                int value;
            }
            """, "semicolonless imports remain on separate lines");

        expect("class C{void Run(){for (int i=0; i<3; ++i){Print(i);}}}", """
            class C
            {
                void Run()
                {
                    for (int i=0; i<3; ++i)
                    {
                        Print(i);
                    }
                }
            }
            """, "for-header semicolons and expression spacing stay inline");

        expect("class C{void Run(){if (ready)\nwhile (active)\nPrint(1);\nPrint(2);}}", """
            class C
            {
                void Run()
                {
                    if (ready)
                        while (active)
                            Print(1);
                    Print(2);
                }
            }
            """, "nested unbraced control bodies indent without leaking into later statements");

        fixture("class C{void Run(){if (a) if (b) Print(1); else Print(2); elif (c) Print(3); else Print(4); Print(5);}}",
            "unbraced conditional alternatives preserve tokens and stabilize");
        fixture("class C { // opening comment {\npublic: // access }\nvoid Run(){Print(1); // trailing }\n/* { \"\n    preserve this spacing\n} */ Print(2);}}",
            "line and multiline comments preserve their exact contents");
        fixture("class C{void Run(){Print(\"{\\\"} text \\t\");char a='{';char b='}';char q='\\'';Print(\"🧪\");}}",
            "escaped strings, character literals and UTF-16 offsets remain unchanged");
        interpolation("$\"begin{1 /* } \" { */ + 2}end\"", "block comments in interpolation ignore braces and bare quotes");
        interpolation("$\"line{1 // } \" {\n+ 2}end\"", "line comments in interpolation ignore braces and bare quotes");
        interpolation("$\"outer{$\"inner{1 /* } \" */ + 2}\"}end\"", "nested interpolation also protects expression comments");
        interpolation("$\"{{literal}}:{Join(\"{\", \"}\")}\"", "escaped interpolation braces and expression strings remain literal content");

        fixture("#if Windows\nclass C{void Run(){Print(1);}}\n#else\nclass C{void Run(){Print(2);}}\n#endif\n",
            "conditional directives retain distinct line tokens");
        fixture("class C{public:List<List<int>> values;int Value=>values[0][0];T& Item(int i){return values[i];}void Run(){int[,] a={{1,2},{3,4}};for (int n : values[0]) Print(n);}}",
            "generic closers, arrows, references, initializers and range loops retain tokens");
        fixture("class C{[ReadOnly] int Value{get{return 1;}private set{Print(value);}}}",
            "attributes and nested property accessors format without token changes");
        fixture("class C{void Run(){try{Print(1);}catch (Error& error){Print(error);}finally{Print(2);}}}",
            "exception blocks preserve their token sequence");

        String crlf = "import System\r\nclass C{\r\nvoid Run(){Print(1);}\r\n}\r\n";
        String crlfResult = verify(crlf, "CRLF source");
        require(crlfResult.contains("\r\n") && crlfResult.replace("\r\n", "").indexOf('\n') < 0,
            "formatting CRLF source must not introduce lone LF line endings");
        passed("CRLF is preserved for generated indentation and brace lines");

        for (String partial : List.of("", " \t\r\n", "}", "class C{", "class C{void Run(){if (ready)",
                "class C{void Run(){Print(\"unterminated {\n}", "class C{ /* unfinished }",
                "class C{void Run(){Print($\"{1 /* } \"", "class C{ #if X\n}", "class C{char c='\\") ) {
            fixture(partial, "incomplete editor source stays stable: " + partial.replace("\n", "\\n").replace("\r", "\\r"));
        }
        return count;
    }

    /** Pass source files or directories to additionally check real Bolt source without rewriting it. */
    public static void main(String[] args) throws IOException {
        System.out.println(run() + " formatter fixtures passed");
        var files = new ArrayList<Path>();
        for (String argument : args) {
            Path root = Path.of(argument);
            if (Files.isRegularFile(root)) files.add(root);
            else Files.walkFileTree(root, new SimpleFileVisitor<>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) {
                    if (file.getFileName().toString().endsWith(".bolt")) files.add(file);
                    return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult visitFileFailed(Path file, IOException failure) throws IOException {
                    throw new IOException("Cannot inspect formatter corpus path " + file, failure);
                }
            });
        }
        files.sort(Comparator.comparing(Path::toString));
        int failed = 0;
        for (Path file : files) {
            try {
                verify(Files.readString(file), file.toString());
            } catch (AssertionError failure) {
                failed++;
                if (failed <= 8) System.err.println("FAIL " + failure.getMessage());
            }
        }
        if (!files.isEmpty()) System.out.println("Formatter corpus: " + files.size() + " files checked, " + failed + " failures");
        require(failed == 0, "Formatter corpus failed for " + failed + " files");
    }
}
