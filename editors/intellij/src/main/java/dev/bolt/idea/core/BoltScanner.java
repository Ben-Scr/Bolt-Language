package dev.bolt.idea.core;

import java.util.*;

/** Error-tolerant, linear lexical coloring. The native compiler alone diagnoses syntax. */
public final class BoltScanner {
    public enum Kind { SPACE, IDENTIFIER, KEYWORD, CONTROL, TYPE, NUMBER, STRING, CHARACTER,
        LINE_COMMENT, BLOCK_COMMENT, PREPROCESSOR, OPERATOR, BRACE, PAREN, BRACKET, COMMA, SEMICOLON, DOT, BAD }
    public record Token(int start, int end, Kind kind) { }
    public static final Set<String> KEYWORDS = Set.of(
        "import", "namespace", "class", "struct", "interface", "enum", "concept", "require", "operator",
        "public", "private", "protected", "internal", "get", "set", "static", "const", "sealed", "abstract", "virtual",
        "override", "this", "base", "true", "false", "null", "default", "new", "delete", "var", "extern", "async", "event", "implicit", "explicit", "sizeof");
    public static final Set<String> CONTROL = Set.of(
        "if", "elif", "else", "while", "for", "return", "break", "continue", "try", "catch", "finally",
        "throw", "await", "switch", "case", "do");
    public static final Set<String> TYPES = Set.of(
        "void", "bool", "char", "string", "float", "double", "byte", "short", "ushort", "int", "uint", "long", "ulong");
    // Recognize reserved words directly in IDEA's CharSequence without copying each identifier.
    private static final class Prefix {
        final Prefix[] letters = new Prefix[26];
        Kind kind = Kind.IDENTIFIER;
    }
    private static final Prefix WORDS = new Prefix();
    static {
        addWords(KEYWORDS, Kind.KEYWORD);
        addWords(CONTROL, Kind.CONTROL);
        addWords(TYPES, Kind.TYPE);
    }
    private static void addWords(Set<String> words, Kind kind) {
        for (String word : words) {
            Prefix node = WORDS;
            for (int i = 0; i < word.length(); i++) {
                int letter = word.charAt(i) - 'a';
                if (node.letters[letter] == null) node.letters[letter] = new Prefix();
                node = node.letters[letter];
            }
            node.kind = kind;
        }
    }
    public static Kind wordKind(CharSequence text, int start, int end) {
        Prefix node = WORDS;
        for (int i = start; i < end; i++) {
            int letter = text.charAt(i) - 'a';
            if (letter < 0 || letter >= 26 || (node = node.letters[letter]) == null) return Kind.IDENTIFIER;
        }
        return node.kind;
    }
    private BoltScanner() { }

    public static Token next(CharSequence s, int start, int limit) {
        if (start < 0 || start > limit || limit > s.length()) throw new IllegalArgumentException("Invalid lexer range");
        if (start == limit) return null;
        int p = start; char c = s.charAt(p++);
        if (c == '#') {
            int line = start;
            while (line > 0) {
                char before = s.charAt(line - 1);
                if (before == '\n' || before == '\r' || !Character.isWhitespace(before)) break;
                line--;
            }
            // Color only the directive itself as preprocessor syntax. Its expression
            // is regular Bolt syntax, so literals such as true/false keep the normal
            // keyword color in #if/#elif conditions.
            if (line == 0 || s.charAt(line - 1) == '\n' || s.charAt(line - 1) == '\r') {
                while (p < limit && (Character.isLetterOrDigit(s.charAt(p)) || s.charAt(p) == '_')) p++;
                return new Token(start, p, Kind.PREPROCESSOR);
            }
        }
        if (Character.isWhitespace(c)) { while (p < limit && Character.isWhitespace(s.charAt(p))) p++; return new Token(start, p, Kind.SPACE); }
        if (c == '/' && p < limit && s.charAt(p) == '/') {
            while (p < limit && s.charAt(p) != '\n' && s.charAt(p) != '\r') p++;
            return new Token(start, p, Kind.LINE_COMMENT);
        }
        if (c == '/' && p < limit && s.charAt(p) == '*') {
            p++; while (p < limit) { if (s.charAt(p++) == '*' && p < limit && s.charAt(p) == '/') { p++; break; } }
            return new Token(start, p, Kind.BLOCK_COMMENT);
        }
        if (c == '$' && p < limit && s.charAt(p) == '"')
            return new Token(start, interpolatedEnd(s, p + 1, limit, 0), Kind.STRING);
        if (c == '"' || c == '\'') return new Token(start, quotedEnd(s, p, limit, c), c == '"' ? Kind.STRING : Kind.CHARACTER);
        if (Character.isLetter(c) || c == '_') {
            while (p < limit && (Character.isLetterOrDigit(s.charAt(p)) || s.charAt(p) == '_')) p++;
            return new Token(start, p, wordKind(s, start, p));
        }
        if (Character.isDigit(c) || c == '.' && p < limit && Character.isDigit(s.charAt(p))) {
            if (c == '0' && p < limit && (s.charAt(p) == 'x' || s.charAt(p) == 'X' || s.charAt(p) == 'b' || s.charAt(p) == 'B')) {
                int radix = (s.charAt(p++) == 'b' || s.charAt(p - 1) == 'B') ? 2 : 16;
                while (p < limit && (Character.digit(s.charAt(p), radix) >= 0 || s.charAt(p) == '_')) p++;
            } else {
                while (p < limit && (Character.isDigit(s.charAt(p)) || s.charAt(p) == '_')) p++;
                if (p + 1 < limit && s.charAt(p) == '.' && Character.isDigit(s.charAt(p + 1))) {
                    p++; while (p < limit && (Character.isDigit(s.charAt(p)) || s.charAt(p) == '_')) p++;
                }
                if (p < limit && (s.charAt(p) == 'e' || s.charAt(p) == 'E')) {
                    int exponent = p + 1;
                    if (exponent < limit && (s.charAt(exponent) == '+' || s.charAt(exponent) == '-')) exponent++;
                    if (exponent < limit && Character.isDigit(s.charAt(exponent))) {
                        p = exponent + 1; while (p < limit && (Character.isDigit(s.charAt(p)) || s.charAt(p) == '_')) p++;
                    }
                }
            }
            while (p < limit && "uUlLfFdD".indexOf(s.charAt(p)) >= 0) p++;
            return new Token(start, p, Kind.NUMBER);
        }
        Kind kind = switch (c) {
            case '{', '}' -> Kind.BRACE; case '(', ')' -> Kind.PAREN; case '[', ']' -> Kind.BRACKET;
            case ',' -> Kind.COMMA; case ';' -> Kind.SEMICOLON; case '.' -> Kind.DOT;
            default -> "+-*/%=<>!&|^~?:@".indexOf(c) >= 0 ? Kind.OPERATOR : Kind.BAD;
        };
        if ((kind == Kind.OPERATOR || kind == Kind.DOT) && p < limit) {
            char next = s.charAt(p);
            boolean pair = switch (c) {
                case '<', '>', '&', '|', '+' -> next == c || next == '=';
                case '-' -> next == '-' || next == '=' || next == '>';
                case '?' -> next == '?' || next == '.';
                case '=', '!', '*', '/', '%', '^' -> next == '=';
                default -> false;
            };
            if (pair) {
                p++;
                if (next == c && (c == '<' || c == '>' || c == '?') && p < limit && s.charAt(p) == '=') p++;
                return new Token(start, p, Kind.OPERATOR);
            }
            if (c == '.' && next == '.' && p + 1 < limit && s.charAt(p + 1) == '.')
                return new Token(start, p + 2, Kind.OPERATOR);
        }
        return new Token(start, p, kind);
    }
    private static int quotedEnd(CharSequence s, int p, int limit, char quote) {
        while (p < limit) {
            char c = s.charAt(p++);
            if (c == '\\' && p < limit) { p++; continue; }
            if (c == quote) break;
            if (c == '\n' || c == '\r') { p--; break; }
        }
        return p;
    }
    private static int interpolatedEnd(CharSequence s, int p, int limit, int nesting) {
        if (nesting > 64) return quotedEnd(s, p, limit, '"');
        while (p < limit) {
            char c = s.charAt(p++);
            if (c == '\\' && p < limit) { p++; continue; }
            if (c == '"') return p;
            if (c == '\n' || c == '\r') return p - 1;
            if (c != '{') continue;
            if (p < limit && s.charAt(p) == '{') { p++; continue; }
            int depth = 1;
            while (p < limit && depth > 0) {
                c = s.charAt(p++);
                if (c == '/' && p < limit && s.charAt(p) == '/') {
                    p++; while (p < limit && s.charAt(p) != '\n') p++;
                } else if (c == '/' && p < limit && s.charAt(p) == '*') {
                    p++; while (p < limit) { if (s.charAt(p++) == '*' && p < limit && s.charAt(p) == '/') { p++; break; } }
                } else if (c == '"' || c == '\'') p = quotedEnd(s, p, limit, c);
                else if (c == '$' && p < limit && s.charAt(p) == '"') p = interpolatedEnd(s, p + 1, limit, nesting + 1);
                else if (c == '{') depth++; else if (c == '}') depth--;
            }
        }
        return p;
    }
    public static List<Token> scan(CharSequence text) {
        List<Token> result = new ArrayList<>();
        for (int p = 0; p < text.length();) { Token t = next(text, p, text.length()); result.add(t); p = t.end(); }
        return result;
    }
}
