package dev.bolt.idea;
import com.intellij.execution.actions.*;
import com.intellij.execution.configurations.ConfigurationFactory;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.util.Ref;
import com.intellij.psi.PsiElement;
import java.nio.file.Path;

public class BoltRunProducer extends LazyRunConfigurationProducer<BoltRunConfiguration> implements DumbAware {
    protected boolean build(){return false;}
    @Override public ConfigurationFactory getConfigurationFactory(){return BoltRunType.factory(build());}
    private BoltProjectModel.Snapshot model(ConfigurationContext c){
        PsiElement e=c.getPsiLocation();if(e==null)return null;
        var file=e.getContainingFile();var vf=file==null?null:file.getVirtualFile();if(vf==null)return null;
        if(!vf.getName().endsWith(".bolt")&&!vf.getName().equals("bolt.proj"))return null;
        var service=BoltProjectModel.getInstance(c.getProject());Path path=Path.of(vf.getPath());
        var snapshot=service.forFile(path);if(snapshot==null)service.request(path);return snapshot;
    }
    @Override protected boolean setupConfigurationFromContext(BoltRunConfiguration config,ConfigurationContext context,Ref<PsiElement> source){
        var snapshot=model(context);if(snapshot==null||snapshot.info().executable()==build())return false;
        config.projectFile=snapshot.info().file().toString();config.setName(snapshot.info().name()+(build()?" — Build":" — Run"));return true;
    }
    @Override public boolean isConfigurationFromContext(BoltRunConfiguration config,ConfigurationContext context){
        var snapshot=model(context);return snapshot!=null&&config.buildOnly==build()&&config.input().equals(snapshot.info().file());
    }
}
