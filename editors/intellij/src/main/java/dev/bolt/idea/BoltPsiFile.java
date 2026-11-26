package dev.bolt.idea;
import com.intellij.extapi.psi.PsiFileBase;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.psi.FileViewProvider;
public final class BoltPsiFile extends PsiFileBase {
    public BoltPsiFile(FileViewProvider provider) { super(provider, BoltLanguage.INSTANCE); }
    @Override public FileType getFileType() { return BoltFileType.INSTANCE; }
    @Override public String toString() { return "Bolt file"; }
}
