package dev.bolt.idea;
import com.intellij.execution.lineMarker.RunLineMarkerContributor;
import com.intellij.execution.lineMarker.ExecutorAction;
import com.intellij.icons.AllIcons;
import com.intellij.openapi.project.DumbAware;
import com.intellij.psi.PsiElement;
import java.nio.file.Path;
import java.nio.charset.StandardCharsets;

/** Only marks an entry resolved by the compiler, never a method-shaped token. */
public final class BoltRunMarker extends RunLineMarkerContributor implements DumbAware {
    @Override public Info getInfo(PsiElement element){
        if(element.getFirstChild()!=null||!element.getText().equals("Main"))return null;
        var file=element.getContainingFile();if(file==null||file.getVirtualFile()==null)return null;
        Path path=Path.of(file.getVirtualFile().getPath()).toAbsolutePath().normalize();
        var model=BoltProjectModel.getInstance(element.getProject());var snapshot=model.forFile(path);
        if(snapshot==null){model.request(path);return null;}
        if(!snapshot.info().executable())return null;
        String text=file.getText(),disk=snapshot.sourceText().get(path);
        if(disk==null||!text.equals(disk.replace("\r\n","\n")))return null;
        for(var entry:snapshot.info().entries())if(entry.file().equals(path)&&dev.bolt.idea.core.BoltProjectInfo.editorOffset(entry,disk)==element.getTextOffset())
            return new Info(AllIcons.RunConfigurations.TestState.Run,ExecutorAction.getActions(0),e->"Build and run Bolt project "+snapshot.info().name());
        return null;
    }
}
