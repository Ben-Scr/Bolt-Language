package dev.bolt.idea;
import com.intellij.lang.BracePair;
import com.intellij.lang.PairedBraceMatcher;
import com.intellij.psi.PsiFile;
import com.intellij.psi.tree.IElementType;
/** Use the IDE's nesting-aware pairing/skip logic. Angles are NOT generic braces. */
public final class BoltBraceMatcher implements PairedBraceMatcher {
    private static final BracePair[] PAIRS={new BracePair(BoltTokenTypes.LBRACE,BoltTokenTypes.RBRACE,true),new BracePair(BoltTokenTypes.LPAREN,BoltTokenTypes.RPAREN,false),new BracePair(BoltTokenTypes.LBRACKET,BoltTokenTypes.RBRACKET,false)};
    @Override public BracePair[] getPairs(){return PAIRS;}
    @Override public boolean isPairedBracesAllowedBeforeType(IElementType left,IElementType next){return next==null||next==com.intellij.psi.TokenType.WHITE_SPACE||next==BoltTokenTypes.RBRACE||next==BoltTokenTypes.RPAREN||next==BoltTokenTypes.RBRACKET||next==BoltTokenTypes.type(dev.bolt.idea.core.BoltScanner.Kind.COMMA)||next==BoltTokenTypes.type(dev.bolt.idea.core.BoltScanner.Kind.SEMICOLON);}
    @Override public int getCodeConstructStart(PsiFile file,int openingBraceOffset){return openingBraceOffset;}
}
