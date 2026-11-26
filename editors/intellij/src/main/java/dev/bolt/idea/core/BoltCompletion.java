package dev.bolt.idea.core;

import java.util.*;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;

/** SDK-independent lookup decoding and insertion rules, exercised by the real LSP tests. */
public final class BoltCompletion {
    public record Item(String name, String detail, String type, String category, int kind,
                       List<String> parameters, List<String> typeParameters, boolean isStatic) {
        public boolean attribute() { return category.equals("attribute"); }
        public boolean callable() { return kind == 2 || kind == 3 || kind == 4; }
        public String tail() {
            if (attribute()) return parameters.isEmpty() ? "  attribute" : "(" + String.join(", ", parameters) + ")";
            if (!callable()) return isStatic ? "  static" : "";
            String generics = typeParameters.isEmpty() ? "" : "<" + String.join(", ", typeParameters) + ">";
            return generics + "(" + String.join(", ", parameters) + ")" + (isStatic ? "  static" : "");
        }
    }
    public record Insertion(String suffix, int caret, boolean arguments) { }
    private BoltCompletion() { }
    private static final class LegacyImports {
        static final Map<?,?> types = load();
        private static Map<?,?> load() {
            try (var in = BoltCompletion.class.getResourceAsStream("legacy-import-types.json")) {
                if (in == null) throw new IllegalStateException("Missing bundled Bolt import types");
                return Json.map(Json.parse(new String(in.readAllBytes(), StandardCharsets.UTF_8)));
            } catch (IOException e) { throw new UncheckedIOException(e); }
        }
    }

    public static List<Item> items(Object response) {
        List<?> raw = response instanceof List<?> list ? list : Json.list(Json.get(response, "items"));
        List<Item> out = new ArrayList<>(); Set<String> seen = new HashSet<>();
        for (Object entry : raw) {
            String name = Json.string(Json.get(entry, "insertText"));
            if (name.isEmpty()) name = Json.string(Json.get(entry, "label"));
            // We advertise no snippet support. Never insert a snippet as literal source.
            if (Json.integer(Json.get(entry, "insertTextFormat"), 1) != 1 || name.isBlank()) continue;
            String detail = Json.string(Json.get(entry, "detail"));
            Object data = Json.get(Json.get(entry, "data"), "bolt");
            int kind = (int)Json.integer(Json.get(entry, "kind"), 6);
            String type = Json.string(Json.get(data, "type")), category = Json.string(Json.get(data, "category"));
            if (kind == 9 && detail.equals("Bolt standard module " + name) && LegacyImports.types.get(name) instanceof String target) {
                kind = 7;
                type = target;
                category = target.equals(name) ? "type" : "class";
                detail = (target.equals(name) ? "built-in type " : "class ") + target;
            }
            List<String> parameters = strings(Json.get(data, "parameters"));
            if (data == null && (kind == 2 || kind == 3)) parameters = legacyParameters(detail);
            Item item = new Item(name, detail, type, category, kind, parameters,
                strings(Json.get(data, "typeParameters")), Boolean.TRUE.equals(Json.get(data, "static")));
            if (seen.add(name + "\n" + detail)) out.add(item);
        }
        return List.copyOf(out);
    }
    private static List<String> strings(Object value) { return Json.list(value).stream().map(Json::string).toList(); }
    private static List<String> legacyParameters(String signature) {
        int begin = signature.indexOf('('), end = signature.lastIndexOf(')');
        if (begin < 0 || end <= begin + 1) return List.of();
        String arguments = signature.substring(begin + 1, end);
        List<String> out = new ArrayList<>(); int nesting = 0, start = 0;
        for (int i = 0; i < arguments.length(); i++) {
            char c = arguments.charAt(i);
            if (c == '<' || c == '[' || c == '(') nesting++;
            if (c == '>' || c == ']' || c == ')') nesting--;
            if (c == ',' && nesting == 0) { out.add(arguments.substring(start, i).trim()); start = i + 1; }
        }
        out.add(arguments.substring(start).trim()); return List.copyOf(out);
    }
    /** Full qualified prefix after import, or -1 outside an import line. */
    public static int importPrefixStart(CharSequence text, int caret) {
        caret = Math.max(0, Math.min(caret, text.length()));
        int line=caret;
        while(line>0 && text.charAt(line-1)!='\n' && text.charAt(line-1)!='\r') line--;
        int p=line; while(p<caret && (text.charAt(p)==' ' || text.charAt(p)=='\t')) p++;
        String word="import";
        if(p+word.length()>=caret) return -1;
        for(int n=0;n<word.length();n++) if(text.charAt(p+n)!=word.charAt(n)) return -1;
        p+=word.length();
        if(text.charAt(p)!=' ' && text.charAt(p)!='\t') return -1;
        while(p<caret && (text.charAt(p)==' ' || text.charAt(p)=='\t')) p++;
        int start=p;
        for(;p<caret;p++) {char c=text.charAt(p);if(!Character.isLetterOrDigit(c)&&c!='_'&&c!='.')return -1;}
        return start;
    }
    public static int prefixStart(CharSequence text, int caret) {
        int imported=importPrefixStart(text,caret);
        if(imported>=0)return imported;
        int attribute=attributePrefixStart(text,caret);
        if(attribute>=0)return attribute;
        int at = Math.max(0, Math.min(caret, text.length()));
        while (at > 0 && (Character.isLetterOrDigit(text.charAt(at - 1)) || text.charAt(at - 1) == '_')) at--;
        return at;
    }
    public static int attributePrefixStart(CharSequence text,int caret) {
        int at=Math.max(0,Math.min(caret,text.length()));
        while(at>0&&(Character.isLetterOrDigit(text.charAt(at-1))||text.charAt(at-1)=='_'||text.charAt(at-1)=='.'))at--;
        int p=at;while(p>0&&Character.isWhitespace(text.charAt(p-1)))p--;
        if(p>0&&(text.charAt(p-1)=='['||text.charAt(p-1)==','))return at;
        return -1;
    }
    /** Caret is relative to end of the inserted name; existing calls are never duplicated. */
    public static Insertion insertion(Item item, CharSequence following) {
        if (item.attribute()) {
            int next=0;while(next<following.length()&&Character.isWhitespace(following.charAt(next)))next++;
            if(next<following.length()&&following.charAt(next)=='(')return new Insertion("",next+1,true);
            boolean closed=next<following.length()&&(following.charAt(next)==']'||following.charAt(next)==',');
            if(!item.parameters().isEmpty())return new Insertion(closed?"()":"()]",1,true);
            return new Insertion(closed?"":"]",closed?0:1,false);
        }
        if (!item.callable()) return new Insertion("", 0, false);
        int next = 0; while (next < following.length() && Character.isWhitespace(following.charAt(next))) next++;
        if (next < following.length() && following.charAt(next) == '(')
            return new Insertion("", next + 1, !item.parameters().isEmpty());
        if (next < following.length() && following.charAt(next) == '<') return new Insertion("", next + 1, false);
        if (!item.typeParameters().isEmpty()) return new Insertion("<>()", 1, false);
        return new Insertion("()", item.parameters().isEmpty() ? 2 : 1, !item.parameters().isEmpty());
    }
    /** Suppress popups in literal text/comments, but permit code inside $"... {value.} ...". */
    public static boolean isCode(String text, int offset) {
        if (offset < 0 || offset > text.length()) return false;
        for (int p = 0; p < text.length() && p < offset;) {
            var token = BoltScanner.next(text, p, text.length());
            if (token.end() >= offset) return codeIn(text, token, offset, 0);
            p = token.end();
        }
        return true;
    }
    private static boolean codeIn(String text, BoltScanner.Token t, int at, int depth) {
        switch (t.kind()) {
            case LINE_COMMENT, BLOCK_COMMENT: return false;
            case CHARACTER: return at == t.end() && t.end() > t.start() + 1 && text.charAt(t.end()-1) == '\'';
            case STRING:
                if (text.startsWith("$\"", t.start()) && depth < 64) {
                    for (int p = t.start() + 2; p < t.end();) {
                        char c = text.charAt(p++);
                        if (c == '\\' && p < t.end()) { p++; continue; }
                        if ((c == '{' || c == '}') && p < t.end() && text.charAt(p) == c) { p++; continue; }
                        if (c != '{') continue;
                        if (at == p) return true;
                        int braces = 1;
                        while (p < t.end() && braces > 0) {
                            var inner = BoltScanner.next(text, p, t.end());
                            if (inner.start() < at && at <= inner.end()) return codeIn(text, inner, at, depth + 1);
                            String word = text.substring(inner.start(), inner.end());
                            if (word.equals("{")) braces++; else if (word.equals("}")) braces--;
                            p = inner.end();
                        }
                    }
                }
                return at == t.end() && t.end() > t.start()+1 && text.charAt(t.end()-1) == '"';
            default: return true;
        }
    }
}
