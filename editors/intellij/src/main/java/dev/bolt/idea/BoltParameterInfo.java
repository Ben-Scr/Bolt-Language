package dev.bolt.idea;
import com.intellij.lang.parameterInfo.*;
import com.intellij.codeInsight.AutoPopupController;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.progress.ProgressManager;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.util.Key;
import com.intellij.psi.*;
import dev.bolt.idea.core.*;
import java.util.*;
import java.util.concurrent.*;

/** Standard IDEA parameter popup. All resolution happens in the shared compiler. */
public final class BoltParameterInfo implements ParameterInfoHandler<PsiElement,BoltSignatures.Signature>,DumbAware {
    private static final Key<BoltSignatures.Help> HELP=Key.create("bolt.signature.help");
    private static final Key<BoltSignatures.Signature> VISIBLE_SIGNATURE=Key.create("bolt.signature.visible");
    private static final Key<BoltSignatures.Signature> DISPLAY_SIGNATURE=Key.create("bolt.signature.display");
    private static final Key<Boolean> QUERY_PENDING=Key.create("bolt.signature.pending");
    private static final Key<Long> SELECTION_STAMP=Key.create("bolt.signature.selection.stamp");
    private BoltSignatures.Help query(ParameterInfoContext context){
        context.getEditor().putUserData(QUERY_PENDING,false);
        var file=context.getFile();if(!(file instanceof BoltPsiFile))return null;
        var input=BoltExternalAnnotator.collect(file);if(input==null||!input.trusted())return null;
        int offset=context.getOffset();var future=BoltProjectService.getInstance(file.getProject()).query(input,"textDocument/signatureHelp",offset,Map.of());
        int anchor=context instanceof UpdateParameterInfoContext update&&update.getParameterOwner()!=null?update.getParameterOwner().getTextOffset():-1;
        try {
            long deadline=System.nanoTime()+250_000_000L;
            while(!future.isDone()){
                ProgressManager.checkCanceled();
                if(ApplicationManager.getApplication().isDispatchThread()||System.nanoTime()>deadline){
                    context.getEditor().putUserData(QUERY_PENDING,true);
                    future.whenComplete((value,error)->{if(error==null)ApplicationManager.getApplication().invokeLater(()->{
                        var editor=context.getEditor();if(!file.getProject().isDisposed()&&!editor.isDisposed()&&editor.getDocument().getModificationStamp()==input.stamp()&&editor.getCaretModel().getOffset()==offset) {
                            var controller=anchor<0?null:com.intellij.codeInsight.hint.ParameterInfoControllerBase.findControllerAtOffset(editor,anchor);
                            if(controller!=null)controller.updateComponent();
                            else if(value!=null)AutoPopupController.getInstance(file.getProject()).autoPopupParameterInfo(editor,null);
                        }
                    });});return null;
                }
                try{future.get(20,TimeUnit.MILLISECONDS);}catch(TimeoutException ignored){}
            }
            return BoltSignatures.decode(future.get(),input.text());
        }catch(CancellationException e){return null;}catch(InterruptedException e){Thread.currentThread().interrupt();return null;}catch(ExecutionException e){return null;}
    }
    @Override public PsiElement findElementForParameterInfo(CreateParameterInfoContext context){
        var help=query(context);if(help==null)return null;var opening=context.getFile().findElementAt(help.opening());if(opening==null)return null;
        opening.putUserData(HELP,help);opening.putUserData(VISIBLE_SIGNATURE,help.signatures().get(help.selected()));
        context.setItemsToShow(help.signatures().toArray());return opening;
    }
    @Override public void showParameterInfo(PsiElement element,CreateParameterInfoContext context){context.showHint(element,element.getTextOffset(),this);}
    @Override public PsiElement findElementForUpdatingParameterInfo(UpdateParameterInfoContext context){
        var help=query(context);if(help==null)return Boolean.TRUE.equals(context.getEditor().getUserData(QUERY_PENDING))?context.getParameterOwner():null;var opening=context.getFile().findElementAt(help.opening());if(opening==null)return null;
        if(context.getParameterOwner()!=null&&context.getParameterOwner().getTextOffset()!=help.opening())return null;
        opening.putUserData(HELP,help);return opening;
    }
    @Override public void updateParameterInfo(PsiElement owner,UpdateParameterInfoContext context){
        var help=owner.getUserData(HELP);if(help==null){context.removeHint();return;}context.setParameterOwner(owner);context.setCurrentParameter(help.argument());
        // Keep the object identities that IDEA's overload switcher tracks.
        Object[] objects=context.getObjectsToView();for(int i=0;i<objects.length;i++)context.setUIComponentEnabled(i,true);
        long stamp=context.getEditor().getDocument().getModificationStamp();
        if((context.getHighlightedParameter()==null||!Objects.equals(owner.getUserData(SELECTION_STAMP),stamp))&&help.selected()<objects.length)
            context.setHighlightedParameter(objects[help.selected()]);
        owner.putUserData(SELECTION_STAMP,stamp);
        if(context.getHighlightedParameter() instanceof BoltSignatures.Signature signature) {
            owner.putUserData(VISIBLE_SIGNATURE,signature);
            int index=Arrays.asList(objects).indexOf(signature);
            owner.putUserData(DISPLAY_SIGNATURE,index>=0&&index<help.signatures().size()?help.signatures().get(index):signature);
        }
    }
    @Override public void updateUI(BoltSignatures.Signature signature,ParameterInfoUIContext context){
        var owner=context.getParameterOwner();
        var visible=owner==null?null:owner.getUserData(VISIBLE_SIGNATURE);
        context.setUIComponentVisible(visible==null||visible.equals(signature));
        if(visible!=null&&!visible.equals(signature))return;
        // IDEA retains descriptor identities for keyboard switching. Render the
        // current compiler signature even when generic inference changed its text.
        var display=owner==null?null:owner.getUserData(DISPLAY_SIGNATURE);if(display!=null)signature=display;
        int parameter=context.getCurrentParameterIndex(),start=-1,end=-1;
        if(parameter>=signature.parameters().size()&&!signature.parameters().isEmpty()) {
            var last=signature.parameters().getLast();
            if(last.start()>=0&&last.end()<=signature.label().length()&&signature.label().substring(last.start(),last.end()).contains("..."))parameter=signature.parameters().size()-1;
        }
        if(parameter>=0&&parameter<signature.parameters().size()){var p=signature.parameters().get(parameter);start=p.start();end=p.end();}
        context.setupUIComponentPresentation(signature.label(),start,end,false,false,false,context.getDefaultParameterColor());
    }
    @Override public boolean supportsOverloadSwitching(){return true;}
    @Override public boolean isWhitespaceSensitive(){return true;}
    @Override public void processFoundElementForUpdatingParameterInfo(PsiElement owner,UpdateParameterInfoContext context){if(owner==null)context.removeHint();}
}
