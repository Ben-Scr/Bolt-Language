package dev.bolt.idea;
import com.intellij.extapi.psi.ASTWrapperPsiElement;
import com.intellij.lang.ASTNode;
import com.intellij.psi.*;
import com.intellij.openapi.util.TextRange;
import com.intellij.util.IncorrectOperationException;

/** Identifier-shaped PSI is not automatically a resolved language symbol. */
public final class BoltIdentifier extends ASTWrapperPsiElement implements PsiNameIdentifierOwner {
    public BoltIdentifier(ASTNode node){super(node);}
    @Override public String getName(){return getText();}
    @Override public PsiElement getNameIdentifier(){return getFirstChild();}
    @Override public PsiElement setName(String name) throws IncorrectOperationException {throw new IncorrectOperationException("Use Bolt Rename to validate all symbol references together");}
    @Override public PsiReference getReference(){
        return new PsiReferenceBase<BoltIdentifier>(this,new TextRange(0,getTextLength()),false){
            @Override public PsiElement resolve(){return BoltNavigation.declaration(getElement());}
            @Override public Object[] getVariants(){return new Object[0];}
            @Override public PsiElement handleElementRename(String newName){throw new IncorrectOperationException("Use the Bolt semantic Rename action");}
            @Override public boolean isReferenceTo(PsiElement element){var target=resolve();return target!=null&&getElement().getManager().areElementsEquivalent(target,element);}
        };
    }
}
