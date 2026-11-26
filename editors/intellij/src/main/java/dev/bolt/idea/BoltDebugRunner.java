package dev.bolt.idea;

import com.intellij.execution.*;
import com.intellij.execution.configurations.*;
import com.intellij.execution.runners.*;
import com.intellij.execution.ui.RunContentDescriptor;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.progress.*;
import com.intellij.xdebugger.*;
import dev.bolt.idea.core.*;
import org.jetbrains.concurrency.*;
import org.jetbrains.annotations.NotNull;
import java.util.*;

public final class BoltDebugRunner extends AsyncProgramRunner<RunnerSettings> {
    @Override public @NotNull String getRunnerId(){return "BoltLLVMdebugger";}
    @Override public boolean canRun(@NotNull String executor,@NotNull RunProfile profile){return executor.equals("Debug")&&profile instanceof BoltRunConfiguration c&&!c.buildOnly;}
    @Override protected @NotNull Promise<RunContentDescriptor> execute(@NotNull ExecutionEnvironment environment,@NotNull RunProfileState state)throws ExecutionException {
        var config=(BoltRunConfiguration)environment.getRunProfile();
        if(!BoltTrust.isTrusted(environment.getProject()))throw new ExecutionException("Trust this project before debugging Bolt.");
        FileDocumentManager.getInstance().saveAllDocuments();
        var result=new AsyncPromise<RunContentDescriptor>();
        ProgressManager.getInstance().run(new Task.Backgroundable(environment.getProject(),"Build Bolt for LLVM debugging",true){
            @Override public void run(@NotNull ProgressIndicator indicator){
                StringBuilder log=new StringBuilder();
                try{
                    var compiler=CompilerLocator.locate(BoltSettings.getInstance().snapshot().executable());
                    var args=config.arguments.isEmpty()?List.<String>of():Arrays.asList(config.arguments.split("\\R",-1));
                    var options=config.debugOptions();
                    var launch=options.ownsProcess()?DebugLaunch.prepare(compiler,config.input(),config.debugAdapter,config.directory,config.overrideArguments,args,config.parseEnvironment(),text->{synchronized(log){log.append(text);if(log.length()>1024*1024)log.delete(0,log.length()-1024*1024);}indicator.setText2(text.strip());},()->indicator.isCanceled()||environment.getProject().isDisposed()):new DebugLaunch(DebugAdapterLocator.locate(config.debugAdapter,compiler),config.input(),config.input().getParent(),List.of(),config.parseEnvironment());
                    if(!java.nio.file.Files.isRegularFile(launch.executable()))throw new ExecutionException("Debug symbol executable does not exist: "+launch.executable());
                    var sources=new DebugSources(compiler,config.input());
                    indicator.checkCanceled();
                    ApplicationManager.getApplication().invokeLater(()->{
                        if(environment.getProject().isDisposed()){sources.close();result.setError("Project closed");return;}
                        try{
                            if(!BoltTrust.isTrusted(environment.getProject()))throw new ExecutionException("Bolt debugging is disabled in Safe Mode.");
                            var session=XDebuggerManager.getInstance(environment.getProject()).startSession(environment,new XDebugProcessStarter(){
                                @Override public @NotNull XDebugProcess start(@NotNull XDebugSession session)throws ExecutionException{return new BoltDebugProcess(session,launch,options,sources,log.toString());}
                            });
                            result.setResult(session.getRunContentDescriptor());
                        }catch(Exception e){sources.close();result.setError(e);}
                    });
                }catch(Exception e){result.setError(new ExecutionException(e.getMessage()+"\n"+log,e));}
            }
        });
        return result;
    }
}
