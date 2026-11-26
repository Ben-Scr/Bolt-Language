package dev.bolt.idea;

import com.intellij.codeInsight.AutoPopupController;
import com.intellij.codeInsight.completion.*;
import com.intellij.codeInsight.lookup.*;
import com.intellij.icons.AllIcons;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.progress.ProgressManager;
import com.intellij.openapi.project.DumbAware;
import com.intellij.psi.PsiFile;
import dev.bolt.idea.core.BoltCompletion;
import javax.swing.Icon;
import java.util.List;
import java.util.concurrent.*;

/** Typed members are supplied by the same native model that diagnoses the code. */
public final class BoltCompletionContributor extends CompletionContributor implements DumbAware {
    @Override public void fillCompletionVariants(CompletionParameters parameters, CompletionResultSet result) {
        PsiFile file = parameters.getOriginalFile();
        if (!(file instanceof BoltPsiFile) || !BoltSettings.getInstance().snapshot().completion()) return;
        Editor editor = parameters.getEditor();
        int offset = parameters.getOffset();
        var input = BoltExternalAnnotator.collect(file);
        if (input == null || !input.trusted() || offset > input.text().length() || !BoltCompletion.isCode(input.text(), offset)) return;
        int start = BoltCompletion.prefixStart(input.text(), offset);
        CompletionResultSet filtered = result.withPrefixMatcher(input.text().substring(start, offset));
        CompletableFuture<List<BoltCompletion.Item>> future = BoltProjectService.getInstance(file.getProject()).complete(input, offset);
        List<BoltCompletion.Item> items;
        try {
            // The task doing I/O never holds an IDE read action. This background
            // completion task waits briefly, yielding to write-action cancellation.
            long deadline = System.nanoTime() + (parameters.isAutoPopup() ? 250_000_000L : 2_000_000_000L);
            while (true) {
                ProgressManager.checkCanceled();
                if (future.isDone()) { items = future.get(); break; }
                if (ApplicationManager.getApplication().isDispatchThread() || System.nanoTime() >= deadline) {
                    rerunWhenReady(future, input, editor, offset);
                    result.addLookupAdvertisement("Bolt: loading members from the language server…");
                    result.stopHere(); return;
                }
                try { items = future.get(20, TimeUnit.MILLISECONDS); break; }
                catch (TimeoutException ignored) { /* Retry with cancellation. */ }
            }
        } catch (CancellationException ignored) { return; }
        catch (InterruptedException error) { Thread.currentThread().interrupt(); return; }
        catch (ExecutionException error) {
            filtered.addLookupAdvertisement("Bolt completion unavailable. Check Settings > Tools > Bolt.");
            result.stopHere(); return;
        }
        if (BoltProjectService.getInstance(file.getProject()).generation() != input.generation()) return;
        for (var item : items) {
            ProgressManager.checkCanceled();
            var lookup = LookupElementBuilder.create(item, item.name())
                .withTypeText(item.type(), true).withTailText(item.tail(), true).withIcon(icon(item))
                .withInsertHandler((context, element) -> {
                    if (!item.callable() && !item.attribute()) return;
                    int end = context.getTailOffset();
                    var document = context.getDocument();
                    var insert = BoltCompletion.insertion(item, document.getCharsSequence().subSequence(end, document.getTextLength()));
                    char completionChar = context.getCompletionChar();
                    if (completionChar == '(' || completionChar == '<' || item.attribute() && completionChar == ']') context.setAddCompletionChar(false);
                    if (!insert.suffix().isEmpty()) document.insertString(end, insert.suffix());
                    context.getEditor().getCaretModel().moveToOffset(end + insert.caret());
                    context.setTailOffset(end + insert.suffix().length());
                });
            filtered.addElement(AutoCompletionPolicy.NEVER_AUTOCOMPLETE.applyPolicy(lookup));
        }
        // Avoid unrelated word-list entries and inaccessible members from other providers.
        result.stopHere();
    }
    private static void rerunWhenReady(CompletableFuture<?> future, BoltExternalAnnotator.Input input, Editor editor, int offset) {
        future.whenComplete((ignored, error) -> {
            if (error != null || input.project().isDisposed()) return;
            ApplicationManager.getApplication().invokeLater(() -> {
                if (input.project().isDisposed() || editor.isDisposed() || editor.getCaretModel().getOffset() != offset) return;
                if (editor.getDocument().getModificationStamp() != input.stamp()) return;
                if (BoltProjectService.getInstance(input.project()).generation() != input.generation()) return;
                AutoPopupController.getInstance(input.project()).scheduleAutoPopup(editor);
            });
        });
    }
    private static Icon icon(BoltCompletion.Item item) {
        return switch (item.kind()) {
            case 2, 3, 4 -> AllIcons.Nodes.Method;
            case 7 -> AllIcons.Nodes.Class;
            case 8 -> AllIcons.Nodes.Interface;
            case 13 -> AllIcons.Nodes.Enum;
            case 9 -> AllIcons.Nodes.Module;
            default -> AllIcons.Nodes.Field;
        };
    }
}
