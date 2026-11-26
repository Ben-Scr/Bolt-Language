package dev.bolt.idea;

import com.intellij.codeInsight.daemon.DaemonCodeAnalyzer;
import com.intellij.execution.RunManager;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.components.Service;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.module.ModuleManager;
import com.intellij.openapi.roots.ModuleRootModificationUtil;
import com.intellij.openapi.vfs.VfsUtilCore;
import dev.bolt.idea.core.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.*;

/** Cached CLI model. Never launches a project-selected SDK or parses TOML in Java. */
@Service(Service.Level.PROJECT)
public final class BoltProjectModel {
    public record Snapshot(BoltProjectInfo info,Map<Path,String> sourceText) { }
    private final Project project;
    private final Map<Path,Snapshot> models=new ConcurrentHashMap<>();
    private final Set<Path> pending=ConcurrentHashMap.newKeySet();
    private final Set<Path> dirty=ConcurrentHashMap.newKeySet();
    private final Map<String,List<Path>> registeredRoots=new HashMap<>();
    private volatile String lastError="";
    public BoltProjectModel(Project project){this.project=project;}
    public static BoltProjectModel getInstance(Project project){return project.getService(BoltProjectModel.class);}
    public String lastError(){return lastError;}
    public Snapshot forFile(Path file){
        file=file.toAbsolutePath().normalize();
        for(Snapshot s:models.values())if(s.info().file().equals(file)||s.info().sources().contains(file))return s;
        return null;
    }
    public void refresh(){
        if(project.getBasePath()!=null)request(Path.of(project.getBasePath()));
        for(Path path:List.copyOf(models.keySet()))request(path);
    }
    public void request(Path path){
        if(project.isDisposed()||!BoltTrust.isTrusted(project))return;
        Path key=path.toAbsolutePath().normalize();
        if(Files.isRegularFile(key)&&key.toString().endsWith(".bolt"))key=key.getParent();
        final Path input=key;
        if(!pending.add(input)){dirty.add(input);return;}
        ApplicationManager.getApplication().executeOnPooledThread(()->{
            try{
                if(project.isDisposed()||!BoltTrust.isTrusted(project))return;
                Path compiler=CompilerLocator.locate(BoltSettings.getInstance().snapshot().executable());
                BoltProjectInfo info=BoltProjectInfo.query(compiler,input);
                Map<Path,String> current=new HashMap<>();
                for(Path source:info.sources())if(Files.isRegularFile(source)&&Files.size(source)<=4*1024*1024)current.put(source,Files.readString(source,StandardCharsets.UTF_8));
                Snapshot snapshot=new Snapshot(info,Map.copyOf(current));models.put(input,snapshot);lastError="";
                ApplicationManager.getApplication().invokeLater(()->{
                    if(project.isDisposed()||!BoltTrust.isTrusted(project))return;
                    register(snapshot);DaemonCodeAnalyzer.getInstance(project).restart();
                });
            }catch(IOException|RuntimeException e){models.remove(input);lastError=Objects.toString(e.getMessage(),"Bolt project discovery failed");}
            finally{pending.remove(input);if(dirty.remove(input))request(input);}
        });
    }
    private void register(Snapshot snapshot){
        var info=snapshot.info();var manager=RunManager.getInstance(project);boolean buildOnly=!info.executable();
        var debugSettings=(com.intellij.execution.RunnerAndConfigurationSettings)null;
        var releaseSettings=(com.intellij.execution.RunnerAndConfigurationSettings)null;
        String legacyName=info.name()+(info.executable()?" — Run":" — Build");

        for(var setting:manager.getAllSettings()){
            if(!(setting.getConfiguration() instanceof BoltRunConfiguration c)||c.buildOnly!=buildOnly||!c.input().equals(info.file()))continue;
            if("Debug".equals(c.profile)){debugSettings=setting;continue;}
            if("Release".equals(c.profile)){releaseSettings=setting;continue;}

            // Migrate the old untouched auto-generated "<project> — Run/Build" configuration
            // to the new Debug default instead of leaving a duplicate behind.
            if(debugSettings==null&&c.profile.isBlank()&&setting.getName().equals(legacyName)&&
                c.arguments.isBlank()&&c.directory.isBlank()&&c.environment.isBlank()&&!c.overrideArguments){
                c.profile="Debug";setting.setName("Debug");debugSettings=setting;
            }
        }

        var factory=BoltRunType.factory(buildOnly);
        if(debugSettings==null){
            debugSettings=manager.createConfiguration("Debug",factory);
            var config=(BoltRunConfiguration)debugSettings.getConfiguration();
            config.projectFile=info.file().toString();config.profile="Debug";
            manager.addConfiguration(debugSettings);
        }
        if(releaseSettings==null){
            releaseSettings=manager.createConfiguration("Release",factory);
            var config=(BoltRunConfiguration)releaseSettings.getConfiguration();
            config.projectFile=info.file().toString();config.profile="Release";
            manager.addConfiguration(releaseSettings);
        }
        if(manager.getSelectedConfiguration()==null)manager.setSelectedConfiguration(debugSettings);
        // Configure only a Bolt-owned module; don't rewrite roots of Java or other modules.
        String id="Bolt-"+info.name()+"-"+Integer.toHexString(info.root().toString().hashCode());
        if(info.sourceRoots().equals(registeredRoots.get(id)))return;
        registeredRoots.put(id,info.sourceRoots());
        ApplicationManager.getApplication().runWriteAction(()->{
            if(project.isDisposed())return;
            var modules=ModuleManager.getInstance(project);var module=modules.findModuleByName(id);
            if(module==null){Path iml=info.root().resolve(".idea").resolve(id+".iml");
                try{Files.createDirectories(iml.getParent());}catch(IOException e){lastError=e.getMessage();return;}
                module=modules.newModule(iml.toString(),"EMPTY_MODULE");
            }
            ModuleRootModificationUtil.updateModel(module,model->{
                for(var e:model.getContentEntries())model.removeContentEntry(e);
                var content=model.addContentEntry(VfsUtilCore.pathToUrl(info.root().toString()));
                for(Path source:info.sourceRoots()){
                    if(source.startsWith(info.root()))content.addSourceFolder(VfsUtilCore.pathToUrl(source.toString()),false);
                    else model.addContentEntry(VfsUtilCore.pathToUrl(source.toString())).addSourceFolder(VfsUtilCore.pathToUrl(source.toString()),false);
                }
                for(String name:List.of("bin","obj",".bolt"))content.addExcludeFolder(VfsUtilCore.pathToUrl(info.root().resolve(name).toString()));
            });
        });
    }
}
