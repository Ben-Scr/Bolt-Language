package dev.bolt.idea;
import com.intellij.codeInsight.editorActions.enter.EnterHandlerDelegate;
import com.intellij.openapi.actionSystem.DataContext;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.editor.actionSystem.EditorActionHandler;
import com.intellij.openapi.util.Ref;
import com.intellij.psi.PsiFile;
import com.intellij.psi.codeStyle.CodeStyleSettingsManager;
import dev.bolt.idea.core.BoltEditing;
public final class BoltEnterHandler implements EnterHandlerDelegate {
    @Override public Result preprocessEnter(PsiFile file,Editor editor,Ref<Integer> caretOffset,Ref<Integer> caretAdvance,DataContext context,EditorActionHandler originalHandler){
        if(!(file instanceof BoltPsiFile)||editor.getCaretModel().getCaretCount()!=1)return Result.Continue;
        var options=CodeStyleSettingsManager.getSettings(file.getProject()).getIndentOptions(BoltFileType.INSTANCE);
        String indent=options.USE_TAB_CHARACTER?"\t":" ".repeat(Math.max(1,options.INDENT_SIZE));
        var edit=BoltEditing.blockEnter(editor.getDocument().getCharsSequence(),caretOffset.get(),indent);if(edit==null)return Result.Continue;
        editor.getDocument().replaceString(edit.start(),edit.end(),edit.text());editor.getCaretModel().moveToOffset(edit.caret());caretOffset.set(edit.caret());caretAdvance.set(0);return Result.Stop;
    }
}
