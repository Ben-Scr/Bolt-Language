package dev.bolt.idea;

import com.intellij.openapi.actionSystem.ActionUpdateThread;
import com.intellij.openapi.actionSystem.AnActionEvent;
import com.intellij.openapi.actionSystem.CommonDataKeys;
import com.intellij.openapi.command.WriteCommandAction;
import com.intellij.openapi.project.DumbAwareAction;
import com.intellij.psi.PsiDocumentManager;
import dev.bolt.idea.core.BoltFormatting;

/** Formats the entire editor document in one undoable command, including unsaved edits. */
public final class BoltFormatFileAction extends DumbAwareAction {
    @Override public ActionUpdateThread getActionUpdateThread() { return ActionUpdateThread.BGT; }

    @Override public void update(AnActionEvent event) {
        var editor = event.getData(CommonDataKeys.EDITOR);
        var file = event.getData(CommonDataKeys.PSI_FILE);
        boolean available = event.getProject() != null && file instanceof BoltPsiFile && editor != null;
        event.getPresentation().setVisible(available);
        event.getPresentation().setEnabled(available && file.isWritable() &&
            !editor.isViewer() && editor.getDocument().isWritable());
    }

    @Override public void actionPerformed(AnActionEvent event) {
        var project = event.getProject();
        var editor = event.getData(CommonDataKeys.EDITOR);
        var file = event.getData(CommonDataKeys.PSI_FILE);
        if (project == null || !(file instanceof BoltPsiFile) || editor == null ||
            editor.isViewer() || !file.isWritable()) return;
        var document = editor.getDocument();
        if (!document.isWritable() || PsiDocumentManager.getInstance(project).getDocument(file) != document) return;

        var edits = BoltFormatting.edits(document.getText());
        if (edits.isEmpty()) return;
        WriteCommandAction.runWriteCommandAction(project, "Format Bolt File", null, () -> {
            // Descending offsets keep every pending edit valid and let the editor
            // track caret/selection positions instead of replacing the whole file.
            for (int i = edits.size() - 1; i >= 0; i--) {
                var edit = edits.get(i);
                document.replaceString(edit.start(), edit.end(), edit.replacement());
            }
            PsiDocumentManager.getInstance(project).commitDocument(document);
        }, file);
    }
}
