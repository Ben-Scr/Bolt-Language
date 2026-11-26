package dev.bolt.idea;
import com.intellij.codeInsight.navigation.actions.GotoDeclarationHandler;
import com.intellij.openapi.editor.Editor;
import com.intellij.psi.PsiElement;
public final class BoltGotoDeclaration implements GotoDeclarationHandler {
    @Override public PsiElement[] getGotoDeclarationTargets(PsiElement sourceElement,int offset,Editor editor){if(sourceElement==null)return null;var targets=BoltNavigation.declarations(sourceElement);return targets.length==0?null:targets;}
}
