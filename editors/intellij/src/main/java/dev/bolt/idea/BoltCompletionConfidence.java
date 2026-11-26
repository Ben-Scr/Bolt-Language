package dev.bolt.idea;

import com.intellij.codeInsight.completion.CompletionConfidence;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiFile;
import com.intellij.util.ThreeState;
import dev.bolt.idea.core.BoltCompletion;

/** Allow interpolation expressions even though their parent PSI token is a string. */
public final class BoltCompletionConfidence extends CompletionConfidence {
    @Override public ThreeState shouldSkipAutopopup(PsiElement context, PsiFile file, int offset) {
        if (!(file instanceof BoltPsiFile)) return ThreeState.UNSURE;
        return BoltCompletion.isCode(file.getText(), offset) ? ThreeState.NO : ThreeState.YES;
    }
}
