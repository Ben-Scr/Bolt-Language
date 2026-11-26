package dev.bolt.idea.core;

import java.util.*;

/** LSP positions are zero-based UTF-16, exactly like IntelliJ Document offsets. */
public final class LspPositions {
    private final String text;
    private final int[] lines;
    public LspPositions(String text) {
        this.text = text;
        int count = 1; for (int i = 0; i < text.length(); i++) if (text.charAt(i) == '\n') count++;
        lines = new int[count]; int line = 1;
        for (int i = 0; i < text.length(); i++) if (text.charAt(i) == '\n') lines[line++] = i + 1;
    }
    public int offset(int line, int character) {
        if (line < 0 || line >= lines.length || character < 0) return -1;
        int end = line + 1 == lines.length ? text.length() : lines[line + 1] - 1;
        if (end > lines[line] && text.charAt(end - 1) == '\r') end--;
        if (character > end - lines[line]) return -1;
        int at = lines[line] + character;
        if (at > 0 && at < text.length() && Character.isHighSurrogate(text.charAt(at - 1)) && Character.isLowSurrogate(text.charAt(at))) return -1;
        return at;
    }
    public int offset(Object position) {
        return offset((int) Json.integer(Json.get(position, "line"), -1), (int) Json.integer(Json.get(position, "character"), -1));
    }
    public Map<String, Object> position(int offset) {
        int at = Math.max(0, Math.min(text.length(), offset));
        int line = Arrays.binarySearch(lines, at); if (line < 0) line = -line - 2;
        return Json.object("line", line, "character", at - lines[line]);
    }
}
