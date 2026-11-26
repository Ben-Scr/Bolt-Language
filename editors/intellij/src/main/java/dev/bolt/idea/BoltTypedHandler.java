package dev.bolt.idea;

import com.intellij.codeInsight.AutoPopupController;
import com.intellij.codeInsight.editorActions.TypedHandlerDelegate;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.project.Project;
import com.intellij.psi.PsiFile;

/** Import/dot/arrow completion is explicit, independent of word-completion heuristics. */
public final class BoltTypedHandler extends TypedHandlerDelegate {
    private static final com.intellij.openapi.util.Key<java.util.List<com.intellij.openapi.editor.RangeMarker>> ANGLES=com.intellij.openapi.util.Key.create("bolt.paired.generic.closers");
    private static final com.intellij.openapi.util.Key<Boolean> PAIR_GENERIC=com.intellij.openapi.util.Key.create("bolt.pair.generic.now");
    @Override public Result beforeCharTyped(char c,Project project,Editor editor,PsiFile file,com.intellij.openapi.fileTypes.FileType fileType){
        if(!(file instanceof BoltPsiFile)||editor.getCaretModel().getCaretCount()!=1)return Result.CONTINUE;
        int offset=editor.getCaretModel().getOffset();var document=editor.getDocument();
        if(c=='>'){
            var marks=document.getUserData(ANGLES);if(marks!=null)for(var it=marks.iterator();it.hasNext();){var mark=it.next();
                if(!mark.isValid()){it.remove();continue;}
                if(mark.getStartOffset()==offset&&mark.getEndOffset()==offset+1&&offset<document.getTextLength()&&document.getCharsSequence().charAt(offset)=='>'){
                    editor.getCaretModel().moveToOffset(offset+1);mark.dispose();it.remove();return Result.STOP;
                }
            }
        }
        if(c=='<'&&offset>0&&com.intellij.codeInsight.CodeInsightSettings.getInstance().AUTOINSERT_PAIR_BRACKET){
            var leaf=file.findElementAt(offset-1);var resolved=leaf==null?null:BoltNavigation.resolve(leaf);
            // The spelling is interpreted only after a real non-variable symbol resolved.
            boolean generic=false;if(resolved!=null){var symbol=resolved.hit().symbol();
                if(java.util.Set.of("class","struct","interface","method","concept").contains(symbol.category()))
                    generic=symbol.label().contains(symbol.name()+"<")||symbol.overloads().stream().anyMatch(label->label.contains(symbol.name()+"<"));
            }
            editor.putUserData(PAIR_GENERIC,generic);
        }
        return Result.CONTINUE;
    }
    private static void pair(Editor editor,int offset){
        var doc=editor.getDocument();doc.insertString(offset,">");var marker=doc.createRangeMarker(offset,offset+1);marker.setGreedyToLeft(false);marker.setGreedyToRight(false);
        var list=doc.getUserData(ANGLES);if(list==null){list=new java.util.ArrayList<>();doc.putUserData(ANGLES,list);}list.removeIf(m->!m.isValid());list.add(marker);editor.getCaretModel().moveToOffset(offset);
    }
    @Override public Result charTyped(char c,Project project,Editor editor,PsiFile file){
        if(!(file instanceof BoltPsiFile))return Result.CONTINUE;
        if(c=='('||c==',')AutoPopupController.getInstance(project).autoPopupParameterInfo(editor,null);
        int offset=editor.getCaretModel().getOffset();
        if(c!='<'||editor.getCaretModel().getCaretCount()!=1||!com.intellij.codeInsight.CodeInsightSettings.getInstance().AUTOINSERT_PAIR_BRACKET||!dev.bolt.idea.core.BoltEditing.genericInsertionSafe(editor.getDocument().getCharsSequence(),offset))return Result.CONTINUE;
        if(Boolean.TRUE.equals(editor.getUserData(PAIR_GENERIC))){editor.putUserData(PAIR_GENERIC,false);pair(editor,offset);return Result.STOP;}
        var input=BoltExternalAnnotator.collect(file);if(input==null||!input.trusted())return Result.CONTINUE;
        BoltProjectService.getInstance(project).query(input,"textDocument/boltGenericContext",offset,java.util.Map.of()).whenComplete((value,error)->{
            if(error!=null||!Boolean.TRUE.equals(dev.bolt.idea.core.Json.get(value,"generic")))return;
            com.intellij.openapi.application.ApplicationManager.getApplication().invokeLater(()->{
                if(project.isDisposed()||editor.isDisposed()||editor.getDocument().getModificationStamp()!=input.stamp()||editor.getCaretModel().getOffset()!=offset||BoltProjectService.getInstance(project).generation()!=input.generation())return;
                com.intellij.openapi.command.WriteCommandAction.runWriteCommandAction(project,"Pair Bolt generic brackets",null,()->pair(editor,offset));
            });
        });return Result.CONTINUE;
    }

    @Override public Result checkAutoPopup(char c, Project project, Editor editor, PsiFile file) {
        if (!(file instanceof BoltPsiFile) || !BoltSettings.getInstance().snapshot().completion()) return Result.CONTINUE;
        // Runs on the typing/UI path: inspect only the current line and neighboring character.
        // Literal/comment suppression is done later by BoltCompletionConfidence,
        // not by copying/scanning the whole file for every typed character.
        int offset = editor.getCaretModel().getOffset();
        CharSequence text = editor.getDocument().getCharsSequence();
        boolean importContext = dev.bolt.idea.core.BoltCompletion.importPrefixStart(text,offset)>=0;
        // checkAutoPopup is called before the character is inserted.
        if(c==' ') {
            int start=offset;while(start>0&&text.charAt(start-1)!='\n'&&text.charAt(start-1)!='\r')start--;
            importContext |= text.subSequence(start,offset).toString().trim().equals("import");
        }
        if (c == '[' || c == ',' || (importContext && (c==' ' || c=='.' || Character.isLetterOrDigit(c) || c=='_')) || c == '.' || c == '>' && offset > 0 && offset <= text.length() && text.charAt(offset - 1) == '-') {
            AutoPopupController.getInstance(project).scheduleAutoPopup(editor);
            return Result.STOP;
        }
        return Result.CONTINUE;
    }
}
