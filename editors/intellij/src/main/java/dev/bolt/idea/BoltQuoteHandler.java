package dev.bolt.idea;
import com.intellij.codeInsight.editorActions.SimpleTokenSetQuoteHandler;
import dev.bolt.idea.core.BoltScanner;
public final class BoltQuoteHandler extends SimpleTokenSetQuoteHandler {
    public BoltQuoteHandler(){super(BoltTokenTypes.type(BoltScanner.Kind.STRING),BoltTokenTypes.type(BoltScanner.Kind.CHARACTER));}
}
