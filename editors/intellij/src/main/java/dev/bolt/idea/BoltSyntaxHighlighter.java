package dev.bolt.idea;
import com.intellij.lexer.Lexer;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import com.intellij.openapi.fileTypes.SyntaxHighlighterBase;
import com.intellij.psi.tree.IElementType;
import dev.bolt.idea.core.BoltScanner.Kind;
import java.util.*;
public final class BoltSyntaxHighlighter extends SyntaxHighlighterBase {
    private static final TextAttributesKey[] EMPTY = new TextAttributesKey[0];
    private static final Map<IElementType, TextAttributesKey[]> COLORS = new IdentityHashMap<>();
    static {
        for(var t:List.of(BoltTokenTypes.LBRACE,BoltTokenTypes.RBRACE))COLORS.put(t,pack(BoltColors.BRACES));
        for(var t:List.of(BoltTokenTypes.LPAREN,BoltTokenTypes.RPAREN))COLORS.put(t,pack(BoltColors.PARENTHESES));
        for(var t:List.of(BoltTokenTypes.LBRACKET,BoltTokenTypes.RBRACKET))COLORS.put(t,pack(BoltColors.BRACKETS));
        for (Kind kind : Kind.values()) {
            TextAttributesKey color = switch (kind) {
                case KEYWORD -> BoltColors.KEYWORD; case CONTROL -> BoltColors.CONTROL;
                case TYPE -> BoltColors.TYPE; case NUMBER -> BoltColors.NUMBER;
                case STRING -> BoltColors.STRING; case CHARACTER -> BoltColors.CHARACTER; case LINE_COMMENT -> BoltColors.LINE_COMMENT;
                case BLOCK_COMMENT -> BoltColors.BLOCK_COMMENT; case BRACE -> BoltColors.BRACES;
                case PAREN -> BoltColors.PARENTHESES; case BRACKET -> BoltColors.BRACKETS;
                case PREPROCESSOR -> BoltColors.PREPROCESSOR; case OPERATOR -> BoltColors.OPERATOR; case BAD -> BoltColors.BAD;
                default -> null;
            };
            if (color != null) COLORS.put(BoltTokenTypes.type(kind), pack(color));
        }
    }
    @Override public Lexer getHighlightingLexer() { return new BoltLexer(); }
    @Override public TextAttributesKey[] getTokenHighlights(IElementType type) { return COLORS.getOrDefault(type, EMPTY); }
}
