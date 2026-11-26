package dev.bolt.idea.core;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/** Local, error-tolerant folding; offsets use the editor's UTF-16 coordinates. */
public final class BoltFolding {
    public record Range(int start, int end) { }

    private BoltFolding() { }

    public static List<Range> ranges(CharSequence text) {
        return ranges(text, () -> { });
    }

    public static List<Range> ranges(CharSequence text, Runnable checkCanceled) {
        var openings = new ArrayDeque<Integer>();
        var ranges = new ArrayList<Range>();
        int lastNewline = -1, literalEnd = 0, length = text.length();
        for (int i = 0; i < length; i++) {
            if ((i & 4095) == 0) checkCanceled.run();
            char c = text.charAt(i);
            if (c == '\r' || c == '\n') lastNewline = i;
            else if (i >= literalEnd) {
                if (c == '{') openings.push(i);
                else if (c == '}' && !openings.isEmpty()) {
                    int opening = openings.pop();
                    if (opening < lastNewline) ranges.add(new Range(opening, i + 1));
                } else if (c == '"' || c == '\'' || startsLiteral(text, c, i + 1))
                    literalEnd = BoltScanner.next(text, i, length).end();
            }
        }
        ranges.sort(Comparator.comparingInt(Range::start));
        return List.copyOf(ranges);
    }
    private static boolean startsLiteral(CharSequence text, char prefix, int next) {
        if ((prefix != '$' && prefix != '/') || next == text.length()) return false;
        char c = text.charAt(next);
        return prefix == '$' ? c == '"' : c == '/' || c == '*';
    }
}
