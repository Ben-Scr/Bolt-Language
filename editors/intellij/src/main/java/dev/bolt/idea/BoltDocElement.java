package dev.bolt.idea;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.psi.impl.FakePsiElement;
import com.intellij.openapi.editor.Editor;
import java.lang.ref.WeakReference;
/** A documentation page retains its exact semantic snapshot and selected overload. */
final class BoltDocElement extends FakePsiElement {
    final BoltNavigation.Resolved resolved;final int overload;private final PsiElement parent;
    private final WeakReference<Editor> editor;
    BoltDocElement(PsiElement parent,BoltNavigation.Resolved resolved,int overload){this(parent,resolved,overload,null);}
    BoltDocElement(PsiElement parent,BoltNavigation.Resolved resolved,int overload,Editor editor){this.parent=parent;this.resolved=resolved;this.overload=overload;this.editor=new WeakReference<>(editor);}
    Editor editor(){var value=editor.get();return value==null||value.isDisposed()?null:value;}
    @Override public PsiElement getParent(){return parent;}
    @Override public BoltLanguage getLanguage(){return BoltLanguage.INSTANCE;}
    @Override public PsiFile getContainingFile(){return null;}
    @Override public String getName(){return resolved.hit().symbol().name();}
    // Source navigation is exposed explicitly through clickable semantic links in
    // the signature. Keeping this synthetic documentation element non-navigatable
    // avoids IDEA's redundant module/edit-source footer.
    @Override public PsiElement getNavigationElement(){return this;}
    @Override public boolean canNavigate(){return false;}
    @Override public boolean canNavigateToSource(){return false;}
    @Override public void navigate(boolean requestFocus){}
    @Override public boolean isValid(){return parent!=null&&parent.isValid();}
}
