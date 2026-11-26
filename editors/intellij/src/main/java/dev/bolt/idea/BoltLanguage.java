package dev.bolt.idea;
import com.intellij.lang.Language;
public final class BoltLanguage extends Language {
    public static final BoltLanguage INSTANCE = new BoltLanguage();
    private BoltLanguage() { super("Bolt"); }
}
