package dev.bolt.idea.core;

import java.util.*;

/** Dependency-free JSON for the LSP transport. Never evaluates incoming content. */
public final class Json {
    private Json() { }

    public static Map<String, Object> object(Object... entries) {
        if ((entries.length & 1) != 0) throw new IllegalArgumentException("Expected key/value pairs");
        Map<String, Object> result = new LinkedHashMap<>();
        for (int i = 0; i < entries.length; i += 2) result.put((String) entries[i], entries[i + 1]);
        return result;
    }
    public static Map<?, ?> map(Object value) { return value instanceof Map<?, ?> m ? m : Map.of(); }
    public static List<?> list(Object value) { return value instanceof List<?> l ? l : List.of(); }
    public static String string(Object value) { return value instanceof String s ? s : ""; }
    public static long integer(Object value, long fallback) { return value instanceof Number n ? n.longValue() : fallback; }
    public static Object get(Object value, String key) { return map(value).get(key); }

    public static String write(Object value) {
        StringBuilder out = new StringBuilder();
        writeValue(value, out, 0);
        return out.toString();
    }
    private static void writeValue(Object v, StringBuilder out, int depth) {
        if (depth > 128) throw new IllegalArgumentException("JSON nesting limit exceeded");
        if (v == null) { out.append("null"); return; }
        if (v instanceof String s) { quote(s, out); return; }
        if (v instanceof Boolean b) { out.append(b); return; }
        if (v instanceof Number n) {
            if (n instanceof Double d && !Double.isFinite(d) || n instanceof Float f && !Float.isFinite(f))
                throw new IllegalArgumentException("Non-finite JSON number");
            out.append(n); return;
        }
        if (v instanceof Map<?, ?> m) {
            out.append('{'); boolean comma = false;
            for (var e : m.entrySet()) {
                if (!(e.getKey() instanceof String s)) throw new IllegalArgumentException("JSON key is not a string");
                if (comma) out.append(','); comma = true;
                quote(s, out); out.append(':'); writeValue(e.getValue(), out, depth + 1);
            }
            out.append('}'); return;
        }
        if (v instanceof Iterable<?> items) {
            out.append('['); boolean comma = false;
            for (Object item : items) { if (comma) out.append(','); comma = true; writeValue(item, out, depth + 1); }
            out.append(']'); return;
        }
        throw new IllegalArgumentException("Unsupported JSON value: " + v.getClass().getName());
    }
    private static void quote(String s, StringBuilder out) {
        out.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"' -> out.append("\\\"");
                case '\\' -> out.append("\\\\");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                case '\b' -> out.append("\\b");
                case '\f' -> out.append("\\f");
                default -> {
                    if (c < 32 || (Character.isSurrogate(c) &&
                            !(Character.isHighSurrogate(c) && i + 1 < s.length() && Character.isLowSurrogate(s.charAt(i + 1))))) {
                        out.append("\\u");
                        for (int shift = 12; shift >= 0; shift -= 4) out.append("0123456789abcdef".charAt((c >> shift) & 15));
                    } else { out.append(c); if (Character.isHighSurrogate(c)) out.append(s.charAt(++i)); }
                }
            }
        }
        out.append('"');
    }
    public static Object parse(String source) {
        Parser p = new Parser(source);
        Object value = p.value(0); p.space();
        if (p.pos != source.length()) throw p.error("Trailing data");
        return value;
    }
    private static final class Parser {
        final String s; int pos;
        Parser(String s) { this.s = Objects.requireNonNull(s); }
        IllegalArgumentException error(String message) { return new IllegalArgumentException(message + " at JSON offset " + pos); }
        void space() { while (pos < s.length() && " \n\r\t".indexOf(s.charAt(pos)) >= 0) pos++; }
        boolean take(char c) { if (pos < s.length() && s.charAt(pos) == c) { pos++; return true; } return false; }
        void expect(char c) { if (!take(c)) throw error("Expected '" + c + "'"); }
        Object value(int depth) {
            if (depth > 128) throw error("JSON nesting limit exceeded");
            space(); if (pos >= s.length()) throw error("Unexpected end");
            char c = s.charAt(pos);
            if (c == '"') return text();
            if (c == '{') {
                pos++; space(); Map<String, Object> m = new LinkedHashMap<>();
                if (take('}')) return m;
                do {
                    space(); String key = text(); space(); expect(':');
                    if (m.containsKey(key)) throw error("Duplicate key");
                    m.put(key, value(depth + 1)); space();
                    if (take('}')) return m;
                    expect(',');
                } while (true);
            }
            if (c == '[') {
                pos++; space(); List<Object> a = new ArrayList<>();
                if (take(']')) return a;
                do { a.add(value(depth + 1)); space(); if (take(']')) return a; expect(','); } while (true);
            }
            if (s.startsWith("true", pos)) { pos += 4; return true; }
            if (s.startsWith("false", pos)) { pos += 5; return false; }
            if (s.startsWith("null", pos)) { pos += 4; return null; }
            int start = pos; take('-');
            if (take('0')) { /* A subsequent digit is rejected as trailing data. */ }
            else { int digits = pos; while (pos < s.length() && digit(s.charAt(pos))) pos++; if (pos == digits) throw error("Expected a number"); }
            boolean floating = false;
            if (take('.')) { floating = true; int begin = pos; while (pos < s.length() && digit(s.charAt(pos))) pos++; if (begin == pos) throw error("Missing fractional digits"); }
            if (take('e') || take('E')) {
                floating = true; if (!take('+')) take('-'); int begin = pos;
                while (pos < s.length() && digit(s.charAt(pos))) pos++;
                if (begin == pos) throw error("Missing exponent");
            }
            try {
                if (!floating) return Long.parseLong(s.substring(start, pos));
                double result = Double.parseDouble(s.substring(start, pos));
                if (!Double.isFinite(result)) throw error("Number out of range");
                return result;
            } catch (NumberFormatException e) { throw error("Number out of range"); }
        }
        String text() {
            expect('"'); StringBuilder out = new StringBuilder();
            while (pos < s.length()) {
                char c = s.charAt(pos++);
                if (c == '"') return out.toString();
                if (c < 32) throw error("Unescaped control character");
                if (c != '\\') { out.append(c); continue; }
                if (pos == s.length()) throw error("Unterminated escape");
                switch (s.charAt(pos++)) {
                    case '"' -> out.append('"'); case '\\' -> out.append('\\'); case '/' -> out.append('/');
                    case 'b' -> out.append('\b'); case 'f' -> out.append('\f'); case 'n' -> out.append('\n');
                    case 'r' -> out.append('\r'); case 't' -> out.append('\t');
                    case 'u' -> {
                        if (pos + 4 > s.length()) throw error("Incomplete Unicode escape");
                        int cp = 0;
                        for (int i = 0; i < 4; i++) { int d = Character.digit(s.charAt(pos++), 16); if (d < 0) throw error("Invalid Unicode escape"); cp = cp * 16 + d; }
                        out.append((char) cp);
                    }
                    default -> throw error("Invalid escape");
                }
            }
            throw error("Unterminated string");
        }
        static boolean digit(char c) { return c >= '0' && c <= '9'; }
    }
}
