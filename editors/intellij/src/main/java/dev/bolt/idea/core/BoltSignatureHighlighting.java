package dev.bolt.idea.core;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.function.Function;

/** Colors compiler-provided signatures without reparsing source or guessing unknown type kinds. */
public final class BoltSignatureHighlighting {
    public record Span(int start, int end, String kind) { }

    private static final class Token {
        final int start, end;
        final String text;
        final BoltScanner.Kind lexical;
        String kind;

        Token(String label, BoltScanner.Token token) {
            start = token.start(); end = token.end(); lexical = token.kind();
            text = label.substring(start, end);
            kind = switch (lexical) {
                case KEYWORD -> "keyword";
                case CONTROL -> "control";
                case TYPE -> "primitiveType";
                case STRING -> "string";
                case CHARACTER -> "character";
                case NUMBER -> "number";
                case OPERATOR -> "operator";
                case BRACE -> "braces";
                case PAREN -> "parentheses";
                case BRACKET -> "brackets";
                case LINE_COMMENT, BLOCK_COMMENT -> "comment";
                case PREPROCESSOR -> "macro";
                case COMMA, SEMICOLON, DOT, BAD -> "punctuation";
                default -> "identifier";
            };
        }

        boolean identifier() { return lexical == BoltScanner.Kind.IDENTIFIER; }
    }

    private BoltSignatureHighlighting() { }

    public static List<Span> spans(String label, String symbolName, String category,
                                   Function<String, String> typeKind) {
        if (label == null || label.isEmpty()) return List.of();
        List<Token> all = new ArrayList<>(), syntax = new ArrayList<>();
        for (var scanned : BoltScanner.scan(label)) {
            if (scanned.kind() == BoltScanner.Kind.SPACE) continue;
            Token token = new Token(label, scanned);
            all.add(token);
            if (scanned.kind() != BoltScanner.Kind.LINE_COMMENT && scanned.kind() != BoltScanner.Kind.BLOCK_COMMENT)
                syntax.add(token);
        }
        boolean callable = "method".equals(category) || "function".equals(category) || "constructor".equals(category);
        if (callable) method(syntax, symbolName, category, typeKind);
        else {
            types(syntax, 0, syntax.size(), Set.of(), typeKind);
            for (int i = syntax.size() - 1; i >= 0; i--) {
                Token token = syntax.get(i);
                if (token.identifier() && token.text.equals(symbolName)) {
                    if (category != null && !category.isEmpty()) token.kind = category;
                    break;
                }
            }
        }
        return all.stream().map(token -> new Span(token.start, token.end, token.kind)).toList();
    }

    private static void method(List<Token> tokens, String name, String category, Function<String, String> typeKind) {
        for (int i = 0; i < tokens.size(); i++) {
            Token candidate = tokens.get(i);
            if (!candidate.identifier() || !candidate.text.equals(name)) continue;
            int next = i + 1, genericEnd = -1;
            if (is(tokens, next, "<")) {
                genericEnd = angleEnd(tokens, next, tokens.size());
                if (genericEnd < 0) continue;
                next = genericEnd + 1;
            }
            if (!is(tokens, next, "(")) continue;
            candidate.kind = "method";
            if("constructor".equals(category)&&typeKind!=null) {
                String ownerKind=typeKind.apply(name);
                if("class".equals(ownerKind)||"struct".equals(ownerKind))candidate.kind=ownerKind;
            }
            Set<String> generics = new HashSet<>();
            if (genericEnd >= 0) {
                for (int j = i + 2; j < genericEnd; j++) {
                    Token token = tokens.get(j);
                    if (token.identifier()) {
                        token.kind = "typeParameter";
                        generics.add(token.text);
                    }
                }
            }
            types(tokens, 0, i, generics, typeKind);
            int afterParameters = parameters(tokens, next + 1, generics, typeKind);
            // Generic constraints are part of the signature too. Keep their type names
            // and generic arguments semantically colored instead of leaving them as
            // plain identifiers, e.g. `void Print<T>(T value) : IStringable<T>`.
            types(tokens, afterParameters, tokens.size(), generics, typeKind);
            return;
        }
        // Unrecognized or incomplete labels still retain lexical and known type colors.
        types(tokens, 0, tokens.size(), Set.of(), typeKind);
    }

    private static int parameters(List<Token> tokens, int start, Set<String> generics,
                                  Function<String, String> typeKind) {
        int segment = start, parens = 0, brackets = 0, braces = 0, angles = 0;
        boolean defaultValue = false;
        for (int i = start; i < tokens.size(); i++) {
            String text = tokens.get(i).text;
            boolean top = parens == 0 && brackets == 0 && braces == 0 && angles == 0;
            if (top && (text.equals(",") || text.equals(")"))) {
                parameter(tokens, segment, i, generics, typeKind);
                if (text.equals(")")) return i + 1;
                segment = i + 1; defaultValue = false;
                continue;
            }
            if (top && text.equals("=")) defaultValue = true;
            switch (text) {
                case "(" -> parens++;
                case ")" -> parens = Math.max(0, parens - 1);
                case "[" -> brackets++;
                case "]" -> brackets = Math.max(0, brackets - 1);
                case "{" -> braces++;
                case "}" -> braces = Math.max(0, braces - 1);
                case "<" -> {
                    if (!defaultValue || angles > 0 || genericCall(tokens, i)) angles++;
                }
                case ">" -> angles = Math.max(0, angles - 1);
                case ">>" -> angles = Math.max(0, angles - 2);
                default -> { }
            }
        }
        parameter(tokens, segment, tokens.size(), generics, typeKind);
        return tokens.size();
    }

    private static void parameter(List<Token> tokens, int start, int end, Set<String> generics,
                                  Function<String, String> typeKind) {
        int name = -1, angles = 0, brackets = 0;
        for (int i = start; i < end; i++) {
            Token token = tokens.get(i);
            if (token.text.equals("=")) break;
            if (angles == 0 && brackets == 0 && token.identifier()) name = i;
            switch (token.text) {
                case "<" -> angles++;
                case ">" -> angles = Math.max(0, angles - 1);
                case ">>" -> angles = Math.max(0, angles - 2);
                case "[" -> brackets++;
                case "]" -> brackets = Math.max(0, brackets - 1);
                default -> { }
            }
        }
        // A single unknown identifier can be an unnamed type; do not invent a parameter name.
        if (name > start && !is(tokens, name - 1, ".") && !is(tokens, name - 1, ":")) {
            types(tokens, start, name, generics, typeKind);
            tokens.get(name).kind = "parameter";
        } else types(tokens, start, end, generics, typeKind);
    }

    private static void types(List<Token> tokens, int start, int end, Set<String> generics,
                              Function<String, String> typeKind) {
        for (int i = start; i < end; i++) {
            Token token = tokens.get(i);
            if (!token.identifier()) continue;
            if (generics.contains(token.text)) { token.kind = "typeParameter"; continue; }
            if (typeKind == null) continue;
            String qualified = token.text;
            int previous = i - 1;
            while (previous > start && is(tokens, previous, ".") && tokens.get(previous - 1).identifier()) {
                qualified = tokens.get(previous - 1).text + "." + qualified;
                previous -= 2;
            }
            String kind = typeKind.apply(qualified);
            if ((kind == null || kind.isEmpty()) && !qualified.equals(token.text)) kind = typeKind.apply(token.text);
            if (kind != null && !kind.isEmpty()) token.kind = kind;
        }
    }

    private static boolean genericCall(List<Token> tokens, int at) {
        if (at == 0 || !tokens.get(at - 1).identifier()) return false;
        int end = angleEnd(tokens, at, tokens.size());
        return end >= 0 && (is(tokens, end + 1, "(") || is(tokens, end + 1, "."));
    }

    private static int angleEnd(List<Token> tokens, int at, int limit) {
        int depth = 0;
        for (int i = at; i < limit; i++) {
            switch (tokens.get(i).text) {
                case "<" -> depth++;
                case ">" -> depth--;
                case ">>" -> depth -= 2;
                case "(", ")", "=", ";" -> { return -1; }
                default -> { }
            }
            if (depth <= 0) return i;
        }
        return -1;
    }

    private static boolean is(List<Token> tokens, int at, String text) {
        return at >= 0 && at < tokens.size() && tokens.get(at).text.equals(text);
    }
}
