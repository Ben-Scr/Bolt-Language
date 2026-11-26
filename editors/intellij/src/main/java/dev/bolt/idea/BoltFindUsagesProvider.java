package dev.bolt.idea;
import com.intellij.lang.findUsages.FindUsagesProvider;
import com.intellij.lang.cacheBuilder.WordsScanner;
import com.intellij.lang.cacheBuilder.DefaultWordsScanner;
import com.intellij.psi.*;
import com.intellij.psi.tree.TokenSet;
import dev.bolt.idea.core.BoltScanner;
public final class BoltFindUsagesProvider implements FindUsagesProvider {
    @Override public WordsScanner getWordsScanner(){return new DefaultWordsScanner(new BoltLexer(),TokenSet.create(BoltTokenTypes.type(BoltScanner.Kind.IDENTIFIER)),BoltTokenTypes.COMMENTS,BoltTokenTypes.STRINGS);}
    @Override public boolean canFindUsagesFor(PsiElement element){return BoltNavigation.resolve(element)!=null;}
    @Override public String getHelpId(PsiElement element){return null;}
    @Override public String getType(PsiElement element){var value=BoltNavigation.resolve(element);return value==null?"symbol":value.hit().symbol().category();}
    @Override public String getDescriptiveName(PsiElement element){var value=BoltNavigation.resolve(element);return value==null?element.getText():value.hit().symbol().label();}
    @Override public String getNodeText(PsiElement element,boolean full){return getDescriptiveName(element);}
}
