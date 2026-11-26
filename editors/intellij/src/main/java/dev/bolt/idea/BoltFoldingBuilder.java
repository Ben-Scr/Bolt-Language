package dev.bolt.idea;

import com.intellij.lang.ASTNode;
import com.intellij.lang.folding.FoldingBuilderEx;
import com.intellij.lang.folding.FoldingDescriptor;
import com.intellij.openapi.editor.Document;
import com.intellij.openapi.progress.ProgressManager;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.util.TextRange;
import com.intellij.psi.PsiElement;
import dev.bolt.idea.core.BoltFolding;
import java.util.ArrayList;

/** Brace-block folding also works while indexing and without a running compiler. */
public final class BoltFoldingBuilder extends FoldingBuilderEx implements DumbAware {
    @Override public FoldingDescriptor[] buildFoldRegions(PsiElement root, Document document, boolean quick) {
        var descriptors = new ArrayList<FoldingDescriptor>();
        var node = root.getNode();
        int base = root.getTextRange().getStartOffset();
        for (var range : BoltFolding.ranges(root.getText(), ProgressManager::checkCanceled)) {
            ProgressManager.checkCanceled();
            // Anchor each fold to its own opening token, so nested blocks can be
            // expanded independently even though Bolt intentionally has shallow PSI.
            var opening = node.findLeafElementAt(range.start());
            if (opening != null) descriptors.add(new FoldingDescriptor(opening,
                new TextRange(base + range.start(), base + range.end())));
        }
        return descriptors.toArray(FoldingDescriptor.EMPTY_ARRAY);
    }

    @Override public String getPlaceholderText(ASTNode node) { return "{ … }"; }
    @Override public boolean isCollapsedByDefault(ASTNode node) { return false; }
}
