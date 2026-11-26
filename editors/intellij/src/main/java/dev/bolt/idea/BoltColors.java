package dev.bolt.idea;
import com.intellij.openapi.editor.DefaultLanguageHighlighterColors;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.editor.HighlighterColors;
import com.intellij.openapi.editor.colors.CodeInsightColors;
import dev.bolt.idea.core.BoltScanner;
import java.util.*;
/** Theme-aware IntelliJ color categories with user-overridable defaults. */
public final class BoltColors {
    public static final TextAttributesKey KEYWORD = key("KEYWORD", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey CONTROL = key("CONTROL_FLOW", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey TYPE = key("PRIMITIVE_TYPE", DefaultLanguageHighlighterColors.KEYWORD);
    public static final TextAttributesKey TYPE_PARAMETER = key("TYPE_PARAMETER", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey CONCEPT = key("CONCEPT", DefaultLanguageHighlighterColors.INTERFACE_NAME);
    public static final TextAttributesKey CLASS = key("CLASS", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey INTERFACE = key("INTERFACE", DefaultLanguageHighlighterColors.INTERFACE_NAME);
    public static final TextAttributesKey STRUCT = key("STRUCT", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey ENUM = key("ENUM", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey METHOD = key("METHOD", DefaultLanguageHighlighterColors.FUNCTION_CALL);
    public static final TextAttributesKey FIELD = key("FIELD", DefaultLanguageHighlighterColors.INSTANCE_FIELD);
    public static final TextAttributesKey PROPERTY = key("PROPERTY", DefaultLanguageHighlighterColors.INSTANCE_FIELD);
    public static final TextAttributesKey CHARACTER = key("CHARACTER", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey PREPROCESSOR = key("PREPROCESSOR", DefaultLanguageHighlighterColors.METADATA);
    public static final TextAttributesKey INACTIVE_CODE = key("INACTIVE_CODE", CodeInsightColors.NOT_USED_ELEMENT_ATTRIBUTES);
    public static final TextAttributesKey ATTRIBUTE = key("ATTRIBUTE", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey CONSTANT = key("CONSTANT", DefaultLanguageHighlighterColors.CONSTANT);
    public static final TextAttributesKey PARAMETER = key("PARAMETER", DefaultLanguageHighlighterColors.PARAMETER);
    public static final TextAttributesKey VARIABLE = key("LOCAL_VARIABLE", DefaultLanguageHighlighterColors.LOCAL_VARIABLE);
    public static final TextAttributesKey NAMESPACE = key("NAMESPACE", DefaultLanguageHighlighterColors.CLASS_NAME);
    public static final TextAttributesKey IDENTIFIER = key("IDENTIFIER", DefaultLanguageHighlighterColors.IDENTIFIER);
    public static final TextAttributesKey STRING = key("STRING", DefaultLanguageHighlighterColors.STRING);
    public static final TextAttributesKey NUMBER = key("NUMBER", DefaultLanguageHighlighterColors.NUMBER);
    public static final TextAttributesKey LINE_COMMENT = key("LINE_COMMENT", DefaultLanguageHighlighterColors.LINE_COMMENT);
    public static final TextAttributesKey BLOCK_COMMENT = key("BLOCK_COMMENT", DefaultLanguageHighlighterColors.BLOCK_COMMENT);
    public static final TextAttributesKey OPERATOR = key("OPERATOR", DefaultLanguageHighlighterColors.OPERATION_SIGN);
    public static final TextAttributesKey BRACES = key("BRACES", DefaultLanguageHighlighterColors.BRACES);
    public static final TextAttributesKey PARENTHESES = key("PARENTHESES", DefaultLanguageHighlighterColors.PARENTHESES);
    public static final TextAttributesKey BRACKETS = key("BRACKETS", DefaultLanguageHighlighterColors.BRACKETS);
    public static final TextAttributesKey BAD = key("BAD_CHARACTER", HighlighterColors.BAD_CHARACTER);
    public static final Map<String, TextAttributesKey> SEMANTIC = Map.ofEntries(
        Map.entry("concept", CONCEPT), Map.entry("typeParameter", TYPE_PARAMETER), Map.entry("class", CLASS), Map.entry("struct", STRUCT),
        Map.entry("interface", INTERFACE), Map.entry("enum", ENUM), Map.entry("method", METHOD), Map.entry("function", METHOD),
        Map.entry("event", FIELD), Map.entry("property", PROPERTY), Map.entry("field", FIELD), Map.entry("enumMember", CONSTANT), Map.entry("parameter", PARAMETER),
        Map.entry("variable", VARIABLE), Map.entry("namespace", NAMESPACE), Map.entry("identifier", IDENTIFIER),
        Map.entry("type", CLASS), Map.entry("keyword", KEYWORD), Map.entry("string", STRING),
        Map.entry("character", CHARACTER), Map.entry("number", NUMBER), Map.entry("operator", OPERATOR),
        Map.entry("macro", PREPROCESSOR), Map.entry("comment", LINE_COMMENT), Map.entry("inactiveCode", INACTIVE_CODE), Map.entry("decorator", ATTRIBUTE));
    /** Select only a registered language key: colors remain independently editable by the user. */
    public static TextAttributesKey semantic(String kind, String word) {
        return semantic(kind, word, 0, word.length());
    }
    public static TextAttributesKey semantic(String kind, String text, int start, int end) {
        // The Bolt LSP currently reports preprocessor-excluded source as "comment".
        // Real comments retain comment coloring; excluded source gets a separate dim gray.
        if (kind.equals("comment")) {
            while (start < end && Character.isWhitespace(text.charAt(start))) start++;
            boolean comment = start + 1 < end && text.charAt(start) == '/' &&
                (text.charAt(start + 1) == '/' || text.charAt(start + 1) == '*');
            return comment ? LINE_COMMENT : INACTIVE_CODE;
        }
        if (kind.equals("type") || kind.equals("keyword")) {
            BoltScanner.Kind word = BoltScanner.wordKind(text, start, end);
            if (word == BoltScanner.Kind.TYPE) return TYPE;
            if (kind.equals("keyword")) return word == BoltScanner.Kind.CONTROL ? CONTROL : KEYWORD;
        }
        return SEMANTIC.get(kind);
    }
    private static TextAttributesKey key(String name, TextAttributesKey fallback) { return TextAttributesKey.createTextAttributesKey("BOLT_" + name, fallback); }
    private BoltColors() { }
}
