package dev.bolt.idea;

import com.intellij.execution.*;
import com.intellij.execution.configurations.*;
import com.intellij.execution.process.*;
import com.intellij.execution.runners.ExecutionEnvironment;
import com.intellij.openapi.fileEditor.FileDocumentManager;
import com.intellij.openapi.options.SettingsEditor;
import com.intellij.openapi.project.Project;
import com.intellij.openapi.util.InvalidDataException;
import dev.bolt.idea.core.*;
import org.jdom.Element;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class BoltRunConfiguration extends RunConfigurationBase<RunConfigurationOptions> {
    public String projectFile="", profile="", arguments="", directory="", environment="";
    public boolean overrideArguments=false;
    public String debugAdapter="",debugMode="Launch",debugPid="",debugRemote="",debugSourceMap="";
    public boolean debugTerminal=false,debugExceptions=false;
    DebugOptions debugOptions(){long pid=0;try{if(!debugPid.isBlank())pid=Long.parseLong(debugPid);}catch(NumberFormatException e){throw new IllegalArgumentException("Invalid process ID");}return new DebugOptions(debugMode,pid,debugRemote,debugTerminal,debugExceptions,DebugOptions.parseSourceMap(debugSourceMap));}
    public final boolean buildOnly;
    public BoltRunConfiguration(Project p,ConfigurationFactory f,String name,boolean build){super(p,f,name);buildOnly=build;}
    Path input(){Path base=Path.of(Objects.requireNonNullElse(getProject().getBasePath(),".")).toAbsolutePath();Path p=projectFile.isBlank()?base:Path.of(projectFile);return (p.isAbsolute()?p:base.resolve(p)).normalize();}
    @Override public SettingsEditor<BoltRunConfiguration> getConfigurationEditor(){return new BoltRunEditor();}
    @Override public void checkConfiguration() throws RuntimeConfigurationException {
        if(!BoltTrust.isTrusted(getProject()))throw new RuntimeConfigurationError("Trust this project before running Bolt.");
        try{CompilerLocator.locate(BoltSettings.getInstance().snapshot().executable());if(!Files.exists(input()))throw new IllegalArgumentException("Project/source path does not exist.");parseEnvironment();debugOptions();}
        catch(Exception e){throw new RuntimeConfigurationError(e.getMessage());}
        if(!profile.isBlank()&&!profile.equals("Debug")&&!profile.equals("Release"))throw new RuntimeConfigurationError("Profile must be Debug, Release or project default.");
    }
    Map<String,String> parseEnvironment(){
        Map<String,String> result=new LinkedHashMap<>();
        for(String row:environment.split("\\R"))if(!row.isBlank()){
            int equals=row.indexOf('=');if(equals<=0||row.indexOf('\0')>=0)throw new IllegalArgumentException("Environment entries must be NAME=value, one per line.");
            String name=row.substring(0,equals).trim();if(name.isEmpty()||name.contains(" "))throw new IllegalArgumentException("Invalid environment variable name");result.put(name,row.substring(equals+1));
        }return result;
    }
    @Override public RunProfileState getState(Executor executor,ExecutionEnvironment execution) throws ExecutionException {
        if(!executor.getId().equals("Run")&&(!executor.getId().equals("Debug")||buildOnly))throw new ExecutionException("Bolt libraries support Build; executable projects support Run and Debug.");
        if(!BoltTrust.isTrusted(getProject()))throw new ExecutionException("Bolt execution is disabled in Safe Mode.");
        return new CommandLineState(execution){
            {addConsoleFilters(new BoltConsoleFilter(getProject()));}
            @Override protected ProcessHandler startProcess() throws ExecutionException {
                try{
                    Path compiler=CompilerLocator.locate(BoltSettings.getInstance().snapshot().executable());
                    List<String> args=new ArrayList<>(List.of(compiler.toString(),buildOnly?"compile":"run",input().toString()));
                    if(!profile.isBlank()){args.add("--profile");args.add(profile);}
                    if(!buildOnly&&!directory.isBlank()){args.add("--working-directory");args.add(directory);}
                    if(!buildOnly&&overrideArguments){args.add("--");if(!arguments.isEmpty())args.addAll(Arrays.asList(arguments.split("\\R",-1)));}
                    GeneralCommandLine command=new GeneralCommandLine(args)
                        .withCharset(StandardCharsets.UTF_8)
                        .withParentEnvironmentType(GeneralCommandLine.ParentEnvironmentType.CONSOLE);
                    command.withEnvironment(parseEnvironment());
                    command.withEnvironment("BOLT_IDE_CONSOLE","1");
                    Path cwd=Files.isDirectory(input())?input():input().getParent();command.withWorkDirectory(cwd.toFile());
                    // No 'make before launch': bolt run owns the single build pipeline.
                    var handler=new BoltProcessHandler(command);
                    handler.setShouldDestroyProcessRecursively(true);
                    ProcessTerminatedListener.attach(handler);return handler;
                }catch(Exception e){if(e instanceof ExecutionException x)throw x;throw new ExecutionException(e.getMessage(),e);}
            }
        };
    }
    @Override public void readExternal(Element e) throws InvalidDataException {super.readExternal(e);projectFile=e.getAttributeValue("boltProject","");profile=e.getAttributeValue("boltProfile","");arguments=e.getAttributeValue("boltArguments","");directory=e.getAttributeValue("boltWorkingDirectory","");environment=e.getAttributeValue("boltEnvironment","");overrideArguments=Boolean.parseBoolean(e.getAttributeValue("boltOverrideArguments","false"));debugAdapter=e.getAttributeValue("boltDebugAdapter","");debugMode=e.getAttributeValue("boltDebugMode","Launch");debugPid=e.getAttributeValue("boltDebugPid","");debugRemote=e.getAttributeValue("boltDebugRemote","");debugSourceMap=e.getAttributeValue("boltDebugSourceMap","");debugTerminal=Boolean.parseBoolean(e.getAttributeValue("boltDebugTerminal","false"));debugExceptions=Boolean.parseBoolean(e.getAttributeValue("boltDebugExceptions","false"));}
    @Override public void writeExternal(Element e) {super.writeExternal(e);e.setAttribute("boltProject",projectFile);e.setAttribute("boltProfile",profile);e.setAttribute("boltArguments",arguments);e.setAttribute("boltWorkingDirectory",directory);e.setAttribute("boltEnvironment",environment);e.setAttribute("boltOverrideArguments",Boolean.toString(overrideArguments));e.setAttribute("boltDebugAdapter",debugAdapter);e.setAttribute("boltDebugMode",debugMode);e.setAttribute("boltDebugPid",debugPid);e.setAttribute("boltDebugRemote",debugRemote);e.setAttribute("boltDebugSourceMap",debugSourceMap);e.setAttribute("boltDebugTerminal",Boolean.toString(debugTerminal));e.setAttribute("boltDebugExceptions",Boolean.toString(debugExceptions));}
}
