package dev.bolt.idea;
import com.intellij.lexer.LexerBase;
import com.intellij.psi.tree.IElementType;
import dev.bolt.idea.core.BoltScanner;
/** Multiline comments/literals are single tokens, so every restart boundary has state zero. */
public final class BoltLexer extends LexerBase {
    private CharSequence buffer = "";
    private int limit;
    private BoltScanner.Token current;
    @Override public void start(CharSequence text, int startOffset, int endOffset, int initialState) {
        buffer = text; limit = endOffset; current = BoltScanner.next(text, startOffset, limit);
    }
    @Override public int getState() { return 0; }
    @Override public IElementType getTokenType() { if(current==null)return null;
        if(current.kind()==BoltScanner.Kind.BRACE||current.kind()==BoltScanner.Kind.PAREN||current.kind()==BoltScanner.Kind.BRACKET)return BoltTokenTypes.delimiter(buffer.charAt(current.start()));
        return BoltTokenTypes.type(current.kind()); }
    @Override public int getTokenStart() { return current == null ? limit : current.start(); }
    @Override public int getTokenEnd() { return current == null ? limit : current.end(); }
    @Override public void advance() { if (current != null) current = BoltScanner.next(buffer, current.end(), limit); }
    @Override public CharSequence getBufferSequence() { return buffer; }
    @Override public int getBufferEnd() { return limit; }
}
