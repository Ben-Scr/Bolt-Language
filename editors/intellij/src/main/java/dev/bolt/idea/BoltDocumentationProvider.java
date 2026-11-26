package dev.bolt.idea;
import com.intellij.lang.documentation.AbstractDocumentationProvider;
import com.intellij.lang.documentation.ExternalDocumentationHandler;
import com.intellij.openapi.editor.Editor;
import com.intellij.openapi.editor.colors.EditorColorsManager;
import com.intellij.openapi.editor.colors.EditorColorsScheme;
import com.intellij.openapi.fileEditor.FileEditorManager;
import com.intellij.openapi.fileEditor.OpenFileDescriptor;
import com.intellij.psi.*;
import dev.bolt.idea.core.BoltSymbols;
import dev.bolt.idea.core.Json;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.Base64;

/** Legacy documented provider works on supported IDEA versions, including non-commercial editions. */
@SuppressWarnings("deprecation")
public final class BoltDocumentationProvider extends AbstractDocumentationProvider implements ExternalDocumentationHandler {
    private static final String NAVIGATION="bolt-nav://";
    private static BoltDocElement page(PsiElement element,PsiElement original){
        if(element instanceof BoltDocElement d)return d;
        var r=original==null?null:BoltNavigation.resolve(original);if(r==null)r=element==null?null:BoltNavigation.resolve(element);
        return r==null?null:new BoltDocElement(original==null?element:original,r,Math.max(0,r.hit().symbol().overloads().indexOf(r.hit().symbol().label())));
    }
    private static EditorColorsScheme colorsScheme(Editor editor){
        if(editor!=null) {
            try { return editor.getColorsScheme(); }
            catch(UnsupportedOperationException ignored) { }
        }
        return EditorColorsManager.getInstance().getGlobalScheme();
    }
    @Override public PsiElement getCustomDocumentationElement(Editor editor,PsiFile file,PsiElement context,int offset){
        if(!(file instanceof BoltPsiFile))return null;var p=page(context,context);
        return p==null?null:new BoltDocElement(p.getParent(),p.resolved,p.overload,editor);
    }
    @Override public String generateDoc(PsiElement element,PsiElement originalElement){
        var p=page(element,originalElement);if(p==null)return null;
        var symbol=p.resolved.hit().symbol();var snapshot=p.resolved.snapshot();
        var definition=snapshot.definition(symbol,p.overload);
        var editor=p.editor();if(editor==null)editor=FileEditorManager.getInstance(p.getProject()).getSelectedTextEditor();
        var scheme=colorsScheme(editor);
        // Local declarations can contribute extra type colors, while source links always
        // come from the compiler snapshot and therefore work for embedded stdlib files too.
        BoltSymbols declaration=null;
        var file=BoltNavigation.targetFile(p.getProject(),Json.string(Json.get(definition,"uri")),snapshot.virtualDocuments());
        if(file!=null&&file.getVirtualFile()!=null&&file.getVirtualFile().isInLocalFileSystem())
            declaration=BoltProjectService.getInstance(p.getProject()).symbols(Path.of(file.getVirtualFile().getPath()),file.getText());
        final var types=declaration;
        var ownerDefinition=snapshot.ownerDefinition(symbol);var ownerKind=snapshot.ownerKind(symbol);
        return BoltSymbols.documentation(symbol,p.overload,definition,label->BoltDocumentationHtml.signature(label,symbol,
            name->{String kind=types==null?"":types.typeKind(name);return kind.isEmpty()?snapshot.typeKind(name):kind;},
            name->snapshot.typeDefinition(symbol,name),ownerDefinition,ownerKind,scheme));
    }
    @Override public String getQuickNavigateInfo(PsiElement element,PsiElement originalElement){return generateDoc(element,originalElement);}
    @Override public PsiElement getDocumentationElementForLink(PsiManager manager,String link,PsiElement context){
        if(!link.startsWith("bolt-overload/"))return null;var p=page(context,context);if(p==null)return null;
        try{return new BoltDocElement(p.getParent(),p.resolved,Integer.parseInt(link.substring("bolt-overload/".length())),p.editor());}catch(NumberFormatException e){return null;}
    }
    @Override public boolean handleExternalLink(PsiManager manager,String link,PsiElement context) {
        if(!link.startsWith(NAVIGATION))return false;
        var p=page(context,context);if(p==null)return false;
        try {
            String encoded=link.substring(NAVIGATION.length());
            String json=new String(Base64.getUrlDecoder().decode(encoded),StandardCharsets.UTF_8);
            Object location=Json.parse(json);
            var target=BoltNavigation.target(manager.getProject(),location,p.resolved.snapshot().virtualDocuments(),BoltNavigation.sourceContext(p.resolved));
            if(target==null||target.getContainingFile()==null||target.getContainingFile().getVirtualFile()==null)return false;
            new OpenFileDescriptor(manager.getProject(),target.getContainingFile().getVirtualFile(),target.getTextOffset()).navigate(true);
            return true;
        } catch(IllegalArgumentException ignored) { return false; }
    }
}
