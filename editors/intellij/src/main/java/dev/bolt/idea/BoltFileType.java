package dev.bolt.idea;
import com.intellij.openapi.fileTypes.LanguageFileType;
import com.intellij.openapi.util.IconLoader;
import javax.swing.Icon;
public final class BoltFileType extends LanguageFileType {
    public static final BoltFileType INSTANCE = new BoltFileType();
    private BoltFileType() { super(BoltLanguage.INSTANCE); }
    @Override public String getName() { return "Bolt"; }
    @Override public String getDescription() { return "Bolt source file"; }
    @Override public String getDefaultExtension() { return "bolt"; }
    @Override public Icon getIcon() { return IconLoader.getIcon("/icons/bolt.png", BoltFileType.class); }
}
