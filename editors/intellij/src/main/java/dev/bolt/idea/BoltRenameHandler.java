package dev.bolt.idea;
import com.intellij.refactoring.rename.RenameHandler;
import com.intellij.openapi.actionSystem.*;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.command.WriteCommandAction;
import com.intellij.openapi.ui.Messages;
import com.intellij.openapi.vfs.ReadonlyStatusHandler;
import com.intellij.psi.*;
import dev.bolt.idea.core.*;
import java.util.*;

/** One checked workspace transaction. No regex/search-replace fallback for unresolved names. */
public final class BoltRenameHandler implements RenameHandler {
    private static PsiElement element(DataContext context){var editor=CommonDataKeys.EDITOR.getData(context);var file=CommonDataKeys.PSI_FILE.getData(context);if(editor!=null&&file!=null){int offset=editor.getCaretModel().getOffset();var leaf=file.findElementAt(Math.max(0,Math.min(file.getTextLength()-1,offset)));if(BoltNavigation.identifier(leaf)==null&&offset>0)leaf=file.findElementAt(offset-1);return leaf;}return CommonDataKeys.PSI_ELEMENT.getData(context);}
    @Override public boolean isAvailableOnDataContext(DataContext context){var e=element(context);var r=e==null?null:BoltNavigation.resolve(e);return r!=null&&!r.hit().symbol().readOnly();}
    @Override public boolean isRenaming(DataContext context){return isAvailableOnDataContext(context);}
    @Override public void invoke(Project project,Editor editor,PsiFile file,DataContext context){perform(project,element(context));}
    @Override public void invoke(Project project,PsiElement[] elements,DataContext context){perform(project,elements.length==0?element(context):elements[0]);}
    private static void perform(Project project,PsiElement element){
        var resolved=element==null?null:BoltNavigation.resolve(element);if(resolved==null||resolved.hit().symbol().readOnly())return;
        var input=BoltExternalAnnotator.collect(resolved.file());if(input==null||!input.trusted()||!input.text().equals(resolved.source()))return;
        String name=Messages.showInputDialog(project,"New name for "+resolved.hit().symbol().name(),"Rename Bolt Symbol",Messages.getQuestionIcon(),resolved.hit().symbol().name(),null);
        if(name==null||name.equals(resolved.hit().symbol().name()))return;
        BoltProjectService.getInstance(project).query(input,"textDocument/rename",resolved.hit().start(),Json.object("newName",name)).whenComplete((result,error)->ApplicationManager.getApplication().invokeLater(()->{
            if(project.isDisposed())return;
            if(error!=null){Messages.showErrorDialog(project,Objects.toString(error.getCause()==null?error.getMessage():error.getCause().getMessage(),"Rename failed"),"Bolt Rename");return;}
            try{
                var changes=BoltEdits.decode(result);List<PsiFile> files=new ArrayList<>();List<com.intellij.openapi.editor.Document> documents=new ArrayList<>();
                for(var change:changes){var file=BoltNavigation.targetFile(project,change.uri(),Map.of());if(file==null)throw new IllegalStateException("File no longer exists: "+change.uri());var doc=PsiDocumentManager.getInstance(project).getDocument(file);if(doc==null)throw new IllegalStateException("No text document: "+change.uri());change.apply(doc.getText());files.add(file);documents.add(doc);}
                var virtuals=files.stream().map(PsiFile::getVirtualFile).toArray(com.intellij.openapi.vfs.VirtualFile[]::new);
                if(ReadonlyStatusHandler.getInstance(project).ensureFilesWritable(virtuals).hasReadonlyFiles())return;
                WriteCommandAction.runWriteCommandAction(project,"Rename Bolt Symbol",null,()->{
                    for(int i=0;i<changes.size();i++)changes.get(i).apply(documents.get(i).getText()); // atomic preflight inside write action
                    for(int i=0;i<changes.size();i++){var doc=documents.get(i);for(var edit:changes.get(i).edits())doc.replaceString(edit.start(),edit.end(),edit.replacement());}
                    PsiDocumentManager.getInstance(project).commitAllDocuments();
                },files.toArray(PsiFile[]::new));
            }catch(RuntimeException e){Messages.showErrorDialog(project,Objects.toString(e.getMessage(),"Rename could not be applied"),"Bolt Rename");}
        }));
    }
}
