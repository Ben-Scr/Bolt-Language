package dev.bolt.idea.core;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** Runs compiler-checked native thunks; every local binding keeps its target address. */
public final class DebugExpression {
    private final DapClient dap;
    private final Map<String,Object> configuration;
    private volatile int frame;
    public DebugExpression(DapClient dap,Path input,Path compiler)throws IOException {
        this.dap=dap;
        String os=System.getProperty("os.name");String library=os.startsWith("Windows")?"BoltRuntime.dll":os.contains("Mac")?"libBoltRuntime.dylib":"libBoltRuntime.so";
        Map<String,String> sources=new LinkedHashMap<>();
        if(Files.isRegularFile(input)&&input.toString().endsWith(".bolt"))sources.put(input.toAbsolutePath().normalize().toString().replace('\\','/'),Files.readString(input));
        else {Path root=Files.isDirectory(input)?input:input.getParent();Path sourceRoot=Files.isDirectory(root.resolve("src"))?root.resolve("src"):root;
            try(var files=Files.walk(sourceRoot,6)){long bytes=0;for(Path file:files.filter(Files::isRegularFile).filter(p->p.toString().endsWith(".bolt")).sorted().toList()){
                String text=Files.readString(file);bytes+=text.length();if(bytes>2*1024*1024)throw new IOException("Debugger source snapshot exceeds 2 MiB");sources.put(file.toAbsolutePath().normalize().toString().replace('\\','/'),text);
            }}
        }
        configuration=Json.object("session",UUID.randomUUID().toString(),"input",input.toAbsolutePath().toString(),"runtime",compiler.getParent().resolve(library).toString(),"sources",sources,"llvm",Objects.requireNonNullElse(System.getenv("BOLT_LLVM_LIBRARY"),""));
    }
    public CompletableFuture<Object> evaluate(String expression,int frameId){
        frame=frameId;var args=new LinkedHashMap<>(configuration);args.put("expression",expression);
        return dap.request("evaluate",Json.object("expression","`bolt-evaluate "+Json.write(args),"context","repl","frameId",frameId),120).thenCompose(body->{
            Object result;
            try{result=Json.parse(Json.string(Json.get(body,"result")).strip());}catch(Exception e){return CompletableFuture.failedFuture(new IOException(Json.string(Json.get(body,"result")),e));}
            return resolve(result,frameId);
        });
    }
    private CompletableFuture<Object> resolve(Object captured,int frameId){
        String persistent=Json.string(Json.get(captured,"persistent"));
        CompletableFuture<Object> value=persistent.isEmpty()?CompletableFuture.completedFuture(Json.object("result",Json.get(captured,"value"),"variablesReference",0)):
            dap.request("evaluate",Json.object("expression",persistent,"context","watch","frameId",frameId));
        return value.thenCompose(nativeValue->{
            var response=Json.object("result",Json.get(nativeValue,"result"),"value",Json.get(nativeValue,"result"),"type",Json.get(captured,"type"),"name",Json.get(captured,"name"),"variablesReference",Json.get(nativeValue,"variablesReference"),"memoryReference",Json.get(captured,"address"));
            List<CompletableFuture<Object>> children=new ArrayList<>();for(Object child:Json.list(Json.get(captured,"children")))children.add(resolve(child,frameId));
            return CompletableFuture.allOf(children.toArray(CompletableFuture[]::new)).thenApply(unused->{if(!children.isEmpty())response.put("children",children.stream().map(CompletableFuture::join).toList());return response;});
        });
    }
    public CompletableFuture<Object> release(int frameId){if(frame==0)return CompletableFuture.completedFuture(null);var args=new LinkedHashMap<>(configuration);args.remove("sources");args.put("release",true);return dap.request("evaluate",Json.object("expression","`bolt-evaluate "+Json.write(args),"context","repl","frameId",frameId>0?frameId:frame),90).thenApply(value->{frame=0;return value;});}
}
