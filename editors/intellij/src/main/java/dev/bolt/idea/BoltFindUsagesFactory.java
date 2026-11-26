package dev.bolt.idea;
import com.intellij.find.findUsages.*;
import com.intellij.psi.PsiElement;
import com.intellij.openapi.application.ReadAction;
import com.intellij.openapi.progress.ProgressManager;
import com.intellij.usageView.UsageInfo;
import com.intellij.util.Processor;
import dev.bolt.idea.core.*;
import java.util.Map;
import java.util.concurrent.*;
public final class BoltFindUsagesFactory extends FindUsagesHandlerFactory {
    @Override public boolean canFindUsages(PsiElement element){var r=BoltNavigation.resolve(element);return r!=null&&r.snapshot().definitions(r.hit().symbol()).size()==1;}
    @Override public FindUsagesHandler createFindUsagesHandler(PsiElement element,boolean forHighlightUsages){
        if(!canFindUsages(element))return null;
        return new FindUsagesHandler(element){
            @Override public boolean processElementUsages(PsiElement target,Processor<? super UsageInfo> processor,FindUsagesOptions options){
                var resolved=ReadAction.compute(()->BoltNavigation.resolve(target));if(resolved==null)return false;
                var input=ReadAction.compute(()->BoltExternalAnnotator.collect(resolved.file()));if(input==null||!input.text().equals(resolved.source()))return false;
                var future=BoltProjectService.getInstance(target.getProject()).query(input,"textDocument/references",resolved.hit().start(),Json.object("context",Json.object("includeDeclaration",false)));
                Object result;
                try{while(true){ProgressManager.checkCanceled();try{result=future.get(25,TimeUnit.MILLISECONDS);break;}catch(TimeoutException ignored){}}}
                catch(InterruptedException e){Thread.currentThread().interrupt();return false;}catch(ExecutionException|CancellationException e){return false;}
                for(var location:Json.list(result)){
                    ProgressManager.checkCanceled();var usage=ReadAction.compute(()->{
                        var file=BoltNavigation.targetFile(target.getProject(),Json.string(Json.get(location,"uri")),Map.of());if(file==null)return null;
                        var range=Json.get(location,"range");var positions=new LspPositions(file.getText());int start=positions.offset(Json.get(range,"start")),end=positions.offset(Json.get(range,"end"));
                        if(start<0||end<start)return null;
                        com.intellij.psi.search.SearchScope search=options.searchScope;
                        if(search instanceof com.intellij.psi.search.GlobalSearchScope global&&!global.contains(file.getVirtualFile()))return null;
                        if(search instanceof com.intellij.psi.search.LocalSearchScope local&&java.util.Arrays.stream(local.getScope()).noneMatch(e->e.getContainingFile()==file&&e.getTextRange().containsRange(start,end)))return null;
                        return new UsageInfo(file,start,end,false);
                    });if(usage!=null&&!processor.process(usage))return false;
                }return true;
            }
        };
    }
}
