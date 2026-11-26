package dev.bolt.idea.core;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Set;

/** Tolerant, whitespace-only formatting. Literal and comment contents belong to the lexer. */
public final class BoltFormatting {
    public record Edit(int start, int end, String replacement) { }

    private static final Set<String> ACCESS = Set.of("public", "private", "protected", "internal");
    private static final Set<String> HEADERS = Set.of("if", "elif", "for", "while", "switch", "catch");
    private static final int MAX_DEPTH = 128;

    private BoltFormatting() { }

    /** Changes are ordered by their original offsets and never include a non-whitespace token. */
    public static List<Edit> edits(String source) {
        List<BoltScanner.Token> tokens = new ArrayList<>();
        for (BoltScanner.Token token : BoltScanner.scan(source))
            if (token.kind() != BoltScanner.Kind.SPACE) tokens.add(token);
        if (tokens.isEmpty()) return source.isEmpty() ? List.of() : List.of(new Edit(0, source.length(), ""));

        Analysis analysis = new Analysis(source, tokens);
        String newline = newline(source);
        List<Edit> result = new ArrayList<>();
        Layout layout = new Layout();
        List<Branch> branches = new ArrayList<>();
        boolean pendingBreak = false;
        int extra = 0;
        for (int i = 0; i < tokens.size(); i++) {
            BoltScanner.Token token = tokens.get(i);
            String word = analysis.words[i];
            extra += analysis.extra[i];
            String gap = source.substring(i == 0 ? 0 : tokens.get(i - 1).end(), token.start());
            boolean hasNewline = lineCount(gap) > 0;
            boolean comment = isComment(token);
            boolean closing = word.equals("}");
            int indent;
            if (closing) indent = layout.closeBrace() + extra;
            else {
                if (word.equals(")") || word.equals("]")) layout.closeDelimiter(word.charAt(0));
                indent = layout.indent() + extra;
            }
            if (analysis.label[i]) indent = layout.bodyIndent() - 1 + extra;
            if (token.kind() == BoltScanner.Kind.PREPROCESSOR) indent = 0;

            boolean attachedComment = comment && i > 0 && !hasNewline
                    && tokens.get(i - 1).kind() != BoltScanner.Kind.PREPROCESSOR
                    && tokens.get(i - 1).kind() != BoltScanner.Kind.LINE_COMMENT;
            boolean bracePunctuation = i > 0 && analysis.words[i - 1].equals("}")
                    && Set.of(";", ",", ")", "]", ".", "->", "?.").contains(word);
            boolean breakBefore = i > 0 && (word.equals("{") || closing || analysis.label[i]
                    || token.kind() == BoltScanner.Kind.PREPROCESSOR || analysis.bodyStart[i]
                    || pendingBreak);
            if (attachedComment || bracePunctuation) breakBefore = false;
            String replacement;
            if (hasNewline || breakBefore) {
                replacement = newline.repeat(Math.max(1, lineCount(gap))) + spaces(indent);
            } else if (i == 0) replacement = spaces(indent);
            else if (analysis.labelColon[i] || bracePunctuation) replacement = "";
            else replacement = gap;
            addEdit(result, i == 0 ? 0 : tokens.get(i - 1).end(), token.start(), gap, replacement);

            // A trailing block comment must not consume the statement's pending line break.
            if (!comment || hasNewline) pendingBreak = false;
            if (word.equals("{")) {
                layout.openBrace(Math.max(0, indent - extra));
                pendingBreak = true;
            } else if (closing || analysis.labelColon[i]) pendingBreak = true;
            else if (word.equals(";") && layout.delimiters.isEmpty()) pendingBreak = true;
            else if (token.kind() == BoltScanner.Kind.LINE_COMMENT) pendingBreak = true;
            else if (token.kind() == BoltScanner.Kind.PREPROCESSOR) {
                pendingBreak = true;
                String directive = directive(word);
                if (Set.of("if", "ifdef", "ifndef").contains(directive)) {
                    branches.add(new Branch(layout.copy()));
                } else if ((directive.equals("else") || directive.equals("elif")) && !branches.isEmpty()) {
                    Branch branch = branches.get(branches.size() - 1);
                    branch.end(layout);
                    layout = branch.start.copy();
                } else if (directive.equals("endif") && !branches.isEmpty()) {
                    Branch branch = branches.remove(branches.size() - 1);
                    branch.end(layout);
                    layout = branch.compatible ? branch.firstEnd.copy() : branch.start.copy();
                }
            } else if (word.equals("(") || word.equals("[")) layout.delimiters.add(word.charAt(0));
        }
        int end = tokens.get(tokens.size() - 1).end();
        String tail = source.substring(end);
        String ending = newline.repeat(Math.max(1, lineCount(tail)));
        BoltScanner.Token last = tokens.get(tokens.size() - 1);
        if (tail.isEmpty() && (last.kind() == BoltScanner.Kind.BLOCK_COMMENT || last.kind() == BoltScanner.Kind.STRING)) {
            // An unfinished block comment/interpolation may consume an appended newline.
            String candidate = source + ending;
            if (BoltScanner.next(candidate, last.start(), candidate.length()).end() > end) ending = tail;
        }
        addEdit(result, end, source.length(), tail, ending);
        // Partial editor buffers can change lexical meaning merely by gaining a
        // newline (for example an inline # or an unfinished escaped character).
        // Reject the entire plan if any non-whitespace token would change.
        if (!result.isEmpty() && !sameTokens(source, tokens, apply(source, result))) return List.of();
        return List.copyOf(result);
    }

    public static String format(String source) {
        return apply(source, edits(source));
    }

    private static String apply(String source, List<Edit> edits) {
        if (edits.isEmpty()) return source;
        StringBuilder formatted = new StringBuilder(source.length());
        int at = 0;
        for (Edit edit : edits) {
            formatted.append(source, at, edit.start()).append(edit.replacement());
            at = edit.end();
        }
        return formatted.append(source, at, source.length()).toString();
    }

    private static boolean sameTokens(String source, List<BoltScanner.Token> tokens, String formatted) {
        int index = 0;
        for (int offset = 0; offset < formatted.length();) {
            var actual = BoltScanner.next(formatted, offset, formatted.length());
            offset = actual.end();
            if (actual.kind() == BoltScanner.Kind.SPACE) continue;
            if (index >= tokens.size()) return false;
            var expected = tokens.get(index++);
            int length = expected.end() - expected.start();
            if (actual.kind() != expected.kind() || actual.end() - actual.start() != length ||
                !source.regionMatches(expected.start(), formatted, actual.start(), length)) return false;
        }
        return index == tokens.size();
    }

    private static void addEdit(List<Edit> result, int start, int end, String before, String after) {
        if (!before.equals(after)) result.add(new Edit(start, end, after));
    }

    private static boolean isComment(BoltScanner.Token token) {
        return token.kind() == BoltScanner.Kind.LINE_COMMENT || token.kind() == BoltScanner.Kind.BLOCK_COMMENT;
    }

    private static String spaces(int level) { return "    ".repeat(Math.max(0, Math.min(MAX_DEPTH, level))); }

    private static int lineCount(String text) {
        int count = 0;
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) == '\n') count++;
            else if (text.charAt(i) == '\r') {
                count++;
                if (i + 1 < text.length() && text.charAt(i + 1) == '\n') i++;
            }
        }
        return count;
    }

    private static String newline(String source) {
        for (int i = 0; i < source.length(); i++) {
            if (source.charAt(i) == '\n') return "\n";
            if (source.charAt(i) == '\r') return i + 1 < source.length() && source.charAt(i + 1) == '\n' ? "\r\n" : "\r";
        }
        return "\n";
    }

    private static String directive(String text) {
        int start = 1;
        while (start < text.length() && Character.isWhitespace(text.charAt(start))) start++;
        int end = start;
        while (end < text.length() && Character.isLetter(text.charAt(end))) end++;
        return text.substring(start, end);
    }

    private record Brace(int indent, List<Character> delimiters) { }

    private static final class Layout {
        final List<Brace> braces = new ArrayList<>();
        List<Character> delimiters = new ArrayList<>();

        int bodyIndent() { return braces.isEmpty() ? 0 : braces.get(braces.size() - 1).indent() + 1; }
        int indent() { return bodyIndent() + delimiters.size(); }
        void openBrace(int indent) {
            braces.add(new Brace(indent, delimiters));
            delimiters = new ArrayList<>();
        }
        int closeBrace() {
            if (braces.isEmpty()) return 0;
            Brace brace = braces.remove(braces.size() - 1);
            delimiters = new ArrayList<>(brace.delimiters());
            return brace.indent();
        }
        void closeDelimiter(char close) {
            char open = close == ')' ? '(' : '[';
            for (int i = delimiters.size() - 1; i >= 0; i--) {
                if (delimiters.get(i) == open) {
                    delimiters.subList(i, delimiters.size()).clear();
                    return;
                }
            }
        }
        Layout copy() {
            Layout result = new Layout();
            for (Brace brace : braces) result.braces.add(new Brace(brace.indent(), new ArrayList<>(brace.delimiters())));
            result.delimiters = new ArrayList<>(delimiters);
            return result;
        }
        boolean sameShape(Layout other) { return braces.equals(other.braces) && delimiters.equals(other.delimiters); }
    }

    private static final class Branch {
        final Layout start;
        Layout firstEnd;
        boolean compatible = true;
        Branch(Layout start) { this.start = start; }
        void end(Layout layout) {
            if (firstEnd == null) firstEnd = layout.copy();
            else compatible &= firstEnd.sameShape(layout);
        }
    }

    /** Small statement recognizer solely for unbraced bodies; no syntax is rewritten. */
    private static final class Analysis {
        final List<BoltScanner.Token> tokens;
        final String[] words;
        final int[] match;
        final int[] next;
        final int[] extra;
        final int[] ends;
        final boolean[] label;
        final boolean[] labelColon;
        final boolean[] bodyStart;

        Analysis(String source, List<BoltScanner.Token> tokens) {
            this.tokens = tokens;
            int size = tokens.size();
            words = new String[size];
            match = new int[size];
            next = new int[size + 1];
            extra = new int[size + 1];
            ends = new int[size];
            label = new boolean[size];
            labelColon = new boolean[size];
            bodyStart = new boolean[size];
            Arrays.fill(match, -1);
            Arrays.fill(ends, -1);
            List<Integer> stack = new ArrayList<>();
            for (int i = 0; i < size; i++) {
                BoltScanner.Token token = tokens.get(i);
                words[i] = source.substring(token.start(), token.end());
                String word = words[i];
                if (word.equals("(") || word.equals("[") || word.equals("{")) stack.add(i);
                else if (word.equals(")") || word.equals("]") || word.equals("}")) {
                    String open = word.equals(")") ? "(" : word.equals("]") ? "[" : "{";
                    if (!stack.isEmpty() && words[stack.get(stack.size() - 1)].equals(open)) {
                        int opening = stack.remove(stack.size() - 1);
                        match[opening] = i;
                    }
                }
            }
            next[size] = size;
            for (int i = size - 1; i >= 0; i--) next[i] = isComment(tokens.get(i)) ? next[i + 1] : i;
            for (int i = 0; i < size; i++) {
                if (ACCESS.contains(words[i]) && is(i + 1, ":") && !is(i + 2, ":")) {
                    label[i] = true;
                    labelColon[i + 1] = true;
                }
                int body = body(i);
                if (body < 0 || body >= size || is(body, "{") || is(body, ";")) continue;
                if (is(i, "else") && (is(body, "if") || is(body, "elif"))) continue;
                int end = statementEnd(body, 0);
                if (end <= body) continue;
                bodyStart[body] = true;
                // Include intervening standalone comments in the body's indentation.
                int start = body;
                while (start > i + 1 && isComment(tokens.get(start - 1))) start--;
                extra[start]++;
                extra[end]--;
            }
        }

        boolean is(int i, String word) { return i >= 0 && i < words.length && words[i].equals(word); }
        int body(int i) {
            if (HEADERS.contains(words[i])) {
                int paren = next[i + 1];
                return is(paren, "(") && match[paren] >= 0 ? next[match[paren] + 1] : -1;
            }
            if (is(i, "else") || is(i, "do")) return next[i + 1];
            return -1;
        }

        int statementEnd(int start, int depth) {
            if (start >= words.length || depth >= MAX_DEPTH) return start;
            if (ends[start] >= 0) return ends[start];
            int end = start;
            if (is(start, "{")) end = match[start] >= 0 ? match[start] + 1 : start;
            else {
                int body = body(start);
                if (body > start) {
                    end = statementEnd(body, depth + 1);
                    if (end > body && (is(start, "if") || is(start, "elif"))) {
                        int following = next[end];
                        if (is(following, "else") || is(following, "elif")) end = statementEnd(following, depth + 1);
                    }
                    if (end > body && is(start, "do")) {
                        int following = next[end];
                        if (is(following, "while")) {
                            int paren = next[following + 1];
                            if (is(paren, "(") && match[paren] >= 0) {
                                int semicolon = next[match[paren] + 1];
                                if (is(semicolon, ";")) end = semicolon + 1;
                            }
                        }
                    }
                } else {
                    for (int i = start; i < words.length; i++) {
                        if (is(i, "}") || tokens.get(i).kind() == BoltScanner.Kind.PREPROCESSOR) { end = i; break; }
                        if (is(i, ";")) { end = i + 1; break; }
                        if (match[i] >= 0) i = match[i];
                        end = i + 1;
                    }
                }
            }
            ends[start] = end;
            return end;
        }
    }
}
