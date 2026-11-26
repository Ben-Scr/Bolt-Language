package dev.bolt.idea;

import com.intellij.openapi.project.Project;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.openapi.vfs.VirtualFile;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.psi.*;
import com.intellij.psi.util.PsiTreeUtil;
import com.intellij.testFramework.LightVirtualFile;
import com.intellij.openapi.util.Key;
import dev.bolt.idea.core.*;
import java.net.URI;
import java.nio.file.Path;
import java.util.*;

/** Read-only lookup of exact compiler source locations. No external process on the EDT. */
public final class BoltNavigation {
    private static final Key<Map<String,LightVirtualFile>> VIRTUALS=Key.create("bolt.virtual.sources");
    private static final Key<Resolved> ORIGIN=Key.create("bolt.navigation.origin");
    static final Key<VirtualSource> VIRTUAL_SOURCE=Key.create("bolt.virtual.source");
    record VirtualSource(String uri,Path context) { }
    static Path sourceContext(Resolved resolved) {
        var vf=resolved.file().getVirtualFile();if(vf==null)return null;
        var source=vf.getUserData(VIRTUAL_SOURCE);
        return source!=null?source.context():vf.isInLocalFileSystem()?Path.of(vf.getPath()):null;
    }
    public record Resolved(BoltSymbols.Hit hit,BoltSymbols snapshot,PsiFile file,String source,long generation) { }
    public static BoltIdentifier identifier(PsiElement leaf){return leaf instanceof BoltIdentifier n?n:PsiTreeUtil.getParentOfType(leaf,BoltIdentifier.class,false);}
    private static boolean current(Resolved resolved) {
        if(!resolved.file().isValid()||resolved.generation()!=BoltProjectService.getInstance(resolved.file().getProject()).generation())return false;
        var document=PsiDocumentManager.getInstance(resolved.file().getProject()).getDocument(resolved.file());
        return document!=null&&document.getText().equals(resolved.source());
    }
    public static Resolved resolve(PsiElement element) {
        if(element instanceof BoltDocElement e) {
            var origin=e.resolved;if(!current(origin))return null;
            var selected=origin.snapshot().selectOverload(origin.hit().symbol(),e.overload);if(selected==null)return null;
            var file=targetFile(element.getProject(),Json.string(Json.get(selected.definition(),"uri")),origin.snapshot().virtualDocuments(),sourceContext(origin));if(file==null)return null;
            String source=file.getText();var positions=new LspPositions(source);var range=Json.get(selected.definition(),"range");
            int start=positions.offset(Json.get(range,"start")),end=positions.offset(Json.get(range,"end"));if(start<0||end<=start)return null;
            return new Resolved(new BoltSymbols.Hit(start,end,selected),origin.snapshot(),file,source,origin.generation());
        }
        var name=identifier(element);if(name==null)return null;
        var file=name.getContainingFile();var vf=file.getVirtualFile();var service=BoltProjectService.getInstance(file.getProject());
        if(vf!=null) {
            var doc=PsiDocumentManager.getInstance(file.getProject()).getDocument(file);
            if(doc!=null){var data=service.symbols(vf,doc.getText());if(data!=null){var hit=data.at(name.getTextOffset());if(hit!=null)return new Resolved(hit,data,file,doc.getText(),service.generation());}}
        }
        // IDEA invokes Find Usages on the resolved declaration, possibly in a
        // just-opened or read-only embedded source. Retain its real origin query.
        var origin=name.getUserData(ORIGIN);
        if(origin!=null&&current(origin))return origin;
        return null;
    }
    public static PsiFile targetFile(Project project,String uri,Map<String,String> sources) {
        return targetFile(project,uri,sources,null);
    }
    public static PsiFile targetFile(Project project,String uri,Map<String,String> sources,Path context) {
        try {
            VirtualFile file;
            if(uri.startsWith("file:"))file=LocalFileSystem.getInstance().findFileByNioFile(Path.of(URI.create(uri)));
            else if(uri.startsWith("bolt-stdlib:///")) {
                String text=sources.get(uri);if(text==null)return null;
                String cacheKey=uri+"\n"+context;
                synchronized(project){var cache=project.getUserData(VIRTUALS);if(cache==null){cache=new HashMap<>();project.putUserData(VIRTUALS,cache);}var old=cache.get(cacheKey);
                    if(old==null||!old.getContent().toString().equals(text)){old=new LightVirtualFile(uri.substring(uri.lastIndexOf('/')+1),BoltFileType.INSTANCE,text);old.setWritable(false);if(context!=null)old.putUserData(VIRTUAL_SOURCE,new VirtualSource(uri,context));cache.put(cacheKey,old);}file=old;}
            }else return null;
            return file==null?null:PsiManager.getInstance(project).findFile(file);
        }catch(RuntimeException e){return null;}
    }
    public static PsiElement target(Project project,Object location,Map<String,String> sources) {
        return target(project,location,sources,null);
    }
    public static PsiElement target(Project project,Object location,Map<String,String> sources,Path context) {
        var file=targetFile(project,Json.string(Json.get(location,"uri")),sources,context);if(file==null)return null;
        int offset=new LspPositions(file.getText()).offset(Json.get(Json.get(location,"range"),"start"));if(offset<0||offset>=file.getTextLength())return null;
        var leaf=file.findElementAt(offset);var name=identifier(leaf);return name==null?leaf:name;
    }
    public static PsiElement[] declarations(PsiElement element){
        var r=resolve(element);if(r==null)return new PsiElement[0];List<PsiElement> result=new ArrayList<>();
        for(var location:r.snapshot().definitions(r.hit().symbol())){var target=target(element.getProject(),location,r.snapshot().virtualDocuments(),sourceContext(r));if(target!=null){target.putUserData(ORIGIN,r);result.add(target);}}
        return result.toArray(PsiElement[]::new);
    }
    public static PsiElement declaration(PsiElement element){var targets=declarations(element);return targets.length==1?targets[0]:null;}
    private BoltNavigation(){}
}
