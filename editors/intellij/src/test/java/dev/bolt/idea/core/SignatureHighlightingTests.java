package dev.bolt.idea.core;

import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

/** Signature semantics are tested independently of IntelliJ themes and HTML rendering. */
public final class SignatureHighlightingTests {
    private static int count;
    private static final Map<String, String> TYPES = Map.ofEntries(
        Map.entry("Vec2", "struct"), Map.entry("List", "class"), Map.entry("Map", "class"),
        Map.entry("IAction", "interface"), Map.entry("IStringable", "interface"), Map.entry("Color", "enum"), Map.entry("Addable", "concept"),
        Map.entry("System", "namespace"), Map.entry("System.Collections", "namespace"),
        Map.entry("System.Collections.List", "class"), Map.entry("Geometry.Vector", "struct"));

    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        count++; System.out.println("PASS " + name);
    }

    private static List<BoltSignatureHighlighting.Span> spans(String text, String name) {
        return BoltSignatureHighlighting.spans(text, name, "method", TYPES::get);
    }

    private static List<String> words(String text, List<BoltSignatureHighlighting.Span> spans, String kind) {
        return spans.stream().filter(span -> span.kind().equals(kind))
            .map(span -> text.substring(span.start(), span.end())).toList();
    }

    private static void expect(String text, String name, String kind, String... expected) {
        check(words(text, spans(text, name), kind).equals(List.of(expected)),
            "signature " + kind + " colors: " + text);
    }

    public static int run() {
        count = 0;
        String simple = "void Test(string s)";
        expect(simple, "Test", "primitiveType", "void", "string");
        expect(simple, "Test", "method", "Test");
        expect(simple, "Test", "parameter", "s");
        expect("Vec2 Offset(Vec2 value, float amount)", "Offset", "struct", "Vec2", "Vec2");
        expect("Vec2 Offset(Vec2 value, float amount)", "Offset", "parameter", "value", "amount");
        expect("Vec2(float x, float y)", "Vec2", "method", "Vec2");
        expect("Vec2(float x, float y)", "Vec2", "parameter", "x", "y");
        String generic = "T Foo<T>(const T& value, int count = 1)";
        expect(generic, "Foo", "typeParameter", "T", "T", "T");
        expect(generic, "Foo", "parameter", "value", "count");
        String constrained = "void Print<T>(T value) : IStringable<T>";
        expect(constrained, "Print", "interface", "IStringable");
        expect(constrained, "Print", "typeParameter", "T", "T", "T");
        expect(generic, "Foo", "number", "1");
        String nested = "System.Collections.List<Geometry.Vector> Build(Map<string, List<Vec2>> values, int[,] ranks)";
        expect(nested, "Build", "namespace", "System", "Collections");
        expect(nested, "Build", "struct", "Vector", "Vec2");
        expect(nested, "Build", "class", "List", "Map", "List");
        expect(nested, "Build", "parameter", "values", "ranks");
        String packs = "void Join<T...>(T... values, int... flags)";
        expect(packs, "Join", "typeParameter", "T", "T");
        expect(packs, "Join", "parameter", "values", "flags");
        String defaults = "void Print(string text = \"a, \\\"b), <x>\", char c = ',', int count = Sum(1, 2), Vec2 point = Vec2(3, 4))";
        expect(defaults, "Print", "parameter", "text", "c", "count", "point");
        expect(defaults, "Print", "string", "\"a, \\\"b), <x>\"");
        expect(defaults, "Print", "character", "','");
        expect("void Use(Vec2 point = Make<int, float>(1, 2), bool flag = 1 < 2, int next = 3)",
            "Use", "parameter", "point", "flag", "next");
        expect("void Use(int[] values = {1, 2}, int next = 3)", "Use", "parameter", "values", "next");
        expect("Unknown Find(Unknown value, Other other)", "Find", "identifier", "Unknown", "Unknown", "Other");
        expect("void Overload()", "Overload", "parameter");
        String field = "Vec2 Vec2";
        var fieldSpans = BoltSignatureHighlighting.spans(field, "Vec2", "field", TYPES::get);
        check(words(field, fieldSpans, "struct").equals(List.of("Vec2")) &&
            words(field, fieldSpans, "field").equals(List.of("Vec2")), "field name does not override its same-named type");
        String kinds = "IAction Run(Color value, Addable predicate)";
        expect(kinds, "Run", "interface", "IAction");
        expect(kinds, "Run", "enum", "Color");
        expect(kinds, "Run", "concept", "Addable");
        String unknown = "Unknown Missing(Unknown arg)";
        var noResolver = BoltSignatureHighlighting.spans(unknown, "Missing", "method", null);
        check(words(unknown, noResolver, "identifier").equals(List.of("Unknown", "Unknown")),
            "missing semantic snapshot keeps unknown types neutral");
        String fallback = "Missing.Vec2 Move(Known.Vec2 value)";
        Map<String, String> fallbackKinds = Map.of("Vec2", "struct", "Known.Vec2", "class");
        var fallbackSpans = BoltSignatureHighlighting.spans(fallback, "Move", "method",
            name -> fallbackKinds.getOrDefault(name, ""));
        check(words(fallback, fallbackSpans, "struct").equals(List.of("Vec2")),
            "empty unknown type resolution falls back to the known short type name");
        check(words(fallback, fallbackSpans, "class").equals(List.of("Vec2")),
            "known qualified type category takes precedence over the short name");
        for (String text : List.of(simple, generic, nested, packs, defaults, "void Editing(List<Vec2> value", "", "\n/* 🧪 */ void Ä(string ü)")) {
            var actual = spans(text, text.contains("Ä") ? "Ä" : "Editing");
            int end = 0;
            for (var span : actual) {
                if (span.start() < end || span.end() <= span.start() || span.end() > text.length())
                    throw new AssertionError("Invalid signature span: " + span);
                end = span.end();
            }
            String restored = actual.stream().map(span -> text.substring(span.start(), span.end())).collect(Collectors.joining());
            String lexical = BoltScanner.scan(text).stream().filter(token -> token.kind() != BoltScanner.Kind.SPACE)
                .map(token -> text.substring(token.start(), token.end())).collect(Collectors.joining());
            check(restored.equals(lexical), "signature spans preserve lexical text and UTF-16 offsets: " + text);
        }
        return count;
    }
}
