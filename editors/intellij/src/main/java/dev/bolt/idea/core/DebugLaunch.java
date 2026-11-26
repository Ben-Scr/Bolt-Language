package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.*;

/** Build first, then launch the exact LLVM artifact without a second compilation. */
public record DebugLaunch(Path adapter,Path executable,Path cwd,List<String> arguments,Map<String,String> environment) {
    public static DebugLaunch prepare(Path compiler,Path input,String adapterPath,String directory,
                                      boolean override,List<String> args,Map<String,String> env,
                                      Consumer<String> output,BooleanSupplier cancelled)throws IOException {
        Path root=Files.isDirectory(input)?input:input.getParent();
        Path adapter=DebugAdapterLocator.locate(adapterPath,compiler);
        Path manifest=Files.isRegularFile(input)&&input.getFileName().toString().equals("bolt.proj")?input:null;
        for(Path p=root;manifest==null&&p!=null;p=p.getParent())if(Files.isRegularFile(p.resolve("bolt.proj")))manifest=p.resolve("bolt.proj");
        Path artifact;List<String> configuredArgs=List.of();Path configuredCwd=root;
        Map<String,String> debugEnvironment=new LinkedHashMap<>(env);
        if(manifest!=null){
            StringBuilder model=new StringBuilder();
            run(List.of(compiler.toString(),"project","info",manifest.toString(),"--profile","Debug","--format","json"),root,env,model::append,cancelled);
            Object info=Json.parse(model.toString());
            if(!Json.string(Json.get(info,"kind")).equals("executable"))throw new IOException("Only executable Bolt projects can be debugged.");
            Object profile=Json.get(Json.get(info,"profiles"),"Debug");
            if(!Json.string(Json.get(profile,"optimization")).equals("none"))throw new IOException("Set profiles.Debug.optimization = 'none' in bolt.conf for reliable debugging.");
            artifact=Path.of(Json.string(Json.get(info,"artifact")));root=Path.of(Json.string(Json.get(info,"root")));input=manifest;
            Object run=Json.get(info,"run");configuredCwd=Path.of(Json.string(Json.get(run,"working_directory")));
            configuredArgs=Json.list(Json.get(run,"arguments")).stream().map(Json::string).toList();
            List<String> runtimePaths=Json.list(Json.get(Json.get(info,"native"),"runtime_dirs")).stream().map(Json::string).toList();
            if(!runtimePaths.isEmpty()){
                String key=System.getProperty("os.name").startsWith("Windows")?"PATH":System.getProperty("os.name").contains("Mac")?"DYLD_LIBRARY_PATH":"LD_LIBRARY_PATH";
                String inherited=debugEnvironment.getOrDefault(key,Objects.requireNonNullElse(System.getenv(key),""));
                debugEnvironment.put(key,String.join(java.io.File.pathSeparator,runtimePaths)+(inherited.isEmpty()?"":java.io.File.pathSeparator+inherited));
            }
        }else {
            String stem=Files.isDirectory(input)?input.getFileName().toString():input.getFileName().toString().replaceFirst("\\.bolt$","");
            artifact=root.resolve("bin/Debug").resolve(stem+(System.getProperty("os.name").startsWith("Windows")?".exe":""));
        }
        run(List.of(compiler.toString(),"compile",input.toString(),"--debug","--backend","llvm","--debug-info","--require-native","-o",artifact.toString()),root,env,output,cancelled);
        if(!Files.isRegularFile(artifact))throw new IOException("Compiler did not produce the debug executable: "+artifact);
        Path cwd=directory.isBlank()?configuredCwd:root.resolve(directory).normalize();
        if(!Files.isDirectory(cwd))throw new IOException("Debug working directory does not exist: "+cwd);
        return new DebugLaunch(adapter,artifact.toAbsolutePath(),cwd.toAbsolutePath(),override?List.copyOf(args):configuredArgs,Map.copyOf(debugEnvironment));
    }
    static void run(List<String> command,Path cwd,Map<String,String> env,Consumer<String> output,BooleanSupplier cancelled)throws IOException {
        ProcessBuilder builder=new ProcessBuilder(command).directory(cwd.toFile()).redirectErrorStream(true);builder.environment().putAll(env);
        Process process=builder.start();var reader=Executors.newSingleThreadExecutor(r->{Thread t=new Thread(r,"Bolt debug build output");t.setDaemon(true);return t;});
        Future<?> read=reader.submit(()->{try(var r=new InputStreamReader(process.getInputStream(),StandardCharsets.UTF_8)){char[] b=new char[2048];int n;while((n=r.read(b))>=0)output.accept(new String(b,0,n));}catch(IOException e){throw new UncheckedIOException(e);}});
        try{
            long end=System.nanoTime()+TimeUnit.MINUTES.toNanos(10);
            while(!process.waitFor(100,TimeUnit.MILLISECONDS)){if(cancelled.getAsBoolean())throw new IOException("Debug build cancelled");if(System.nanoTime()>end)throw new IOException("Debug build timed out");}
            read.get(5,TimeUnit.SECONDS);
            if(process.exitValue()!=0)throw new IOException("Bolt debug build failed (exit "+process.exitValue()+"). See build output.");
        }catch(InterruptedException e){Thread.currentThread().interrupt();throw new IOException("Debug build interrupted",e);}
        catch(ExecutionException|TimeoutException e){throw new IOException("Cannot read debug build output",e);}
        finally{process.descendants().forEach(ProcessHandle::destroyForcibly);process.destroyForcibly();reader.shutdownNow();}
    }
    public Map<String,Object> dapArguments(){return Json.object("program",executable.toString(),"cwd",cwd.toString(),"args",arguments,"env",environment,"stopOnEntry",false,"disableASLR",false);}
}
