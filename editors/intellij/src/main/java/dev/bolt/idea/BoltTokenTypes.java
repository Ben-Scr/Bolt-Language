package dev.bolt.idea;
import com.intellij.psi.TokenType;
import com.intellij.psi.tree.*;
import dev.bolt.idea.core.BoltScanner.Kind;
import java.util.*;
public final class BoltTokenTypes {
    public static final IElementType ID_NODE = new IElementType("BOLT_RESOLVABLE_NAME", BoltLanguage.INSTANCE);
    public static final IElementType LBRACE = new IElementType("BOLT_LBRACE", BoltLanguage.INSTANCE), RBRACE = new IElementType("BOLT_RBRACE", BoltLanguage.INSTANCE);
    public static final IElementType LPAREN = new IElementType("BOLT_LPAREN", BoltLanguage.INSTANCE), RPAREN = new IElementType("BOLT_RPAREN", BoltLanguage.INSTANCE);
    public static final IElementType LBRACKET = new IElementType("BOLT_LBRACKET", BoltLanguage.INSTANCE), RBRACKET = new IElementType("BOLT_RBRACKET", BoltLanguage.INSTANCE);
    public static IElementType delimiter(char c) { return switch(c){case '{'->LBRACE;case '}'->RBRACE;case '('->LPAREN;case ')'->RPAREN;case '['->LBRACKET;case ']'->RBRACKET;default->null;}; }
    public static final IFileElementType FILE = new IFileElementType(BoltLanguage.INSTANCE);
    private static final Map<Kind, IElementType> TYPES = new EnumMap<>(Kind.class);
    static {
        for (Kind k : Kind.values()) TYPES.put(k, new IElementType("BOLT_" + k.name(), BoltLanguage.INSTANCE));
        TYPES.put(Kind.SPACE, TokenType.WHITE_SPACE); TYPES.put(Kind.BAD, TokenType.BAD_CHARACTER);
    }
    public static IElementType type(Kind kind) { return TYPES.get(kind); }
    public static final TokenSet COMMENTS = TokenSet.create(type(Kind.LINE_COMMENT), type(Kind.BLOCK_COMMENT));
    public static final TokenSet STRINGS = TokenSet.create(type(Kind.STRING), type(Kind.CHARACTER));
    private BoltTokenTypes() { }
}
