package dev.bolt.idea;
import com.intellij.execution.configurations.*;
import com.intellij.openapi.project.DumbAware;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.IconLoader;
import com.intellij.openapi.util.NotNullLazyValue;

/** Executable configurations support Run and LLVM/LLDB Debug; libraries support Build. */
public final class BoltRunType extends ConfigurationTypeBase implements DumbAware {
    public BoltRunType(){super("BoltExecution","Bolt","Build and run Bolt projects",NotNullLazyValue.createValue(()->IconLoader.getIcon("/icons/bolt-logo.png",BoltRunType.class)));addFactory(new Factory(this,false));addFactory(new Factory(this,true));}
    public static Factory factory(boolean build){
        for(ConfigurationType t:ConfigurationType.CONFIGURATION_TYPE_EP.getExtensionList())if(t instanceof BoltRunType)
            for(ConfigurationFactory f:t.getConfigurationFactories())if(f instanceof Factory b&&b.build==build)return b;
        throw new IllegalStateException("Bolt run configurations are not registered");
    }
    public static final class Factory extends ConfigurationFactory {
        final boolean build;
        Factory(ConfigurationType t,boolean build){super(t);this.build=build;}
        @Override public String getId(){return build?"BoltBuild":"BoltRun";}
        @Override public String getName(){return build?"Bolt Build":"Bolt Run";}
        @Override public boolean isEditableInDumbMode(){return true;}
        @Override public RunConfiguration createTemplateConfiguration(Project p){return new BoltRunConfiguration(p,this,getName(),build);}
    }
}
