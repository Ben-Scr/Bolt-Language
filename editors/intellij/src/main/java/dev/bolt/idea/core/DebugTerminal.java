package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.*;
import java.util.function.BiConsumer;

/** Integrated DAP terminal: literal argv, no shell, independent UTF-8 output streams. */
public final class DebugTerminal implements AutoCloseable {
    private Process process;
    private boolean closed;
    private final BiConsumer<String,String> output;
    public DebugTerminal(BiConsumer<String,String> output){this.output=output;}
    public synchronized Object launch(String command,Object arguments) {
        if(!command.equals("runInTerminal"))throw new IllegalArgumentException("Unsupported adapter request: "+command);
        if(Boolean.TRUE.equals(Json.get(arguments,"argsCanBeInterpretedByShell")))throw new IllegalArgumentException("Shell terminal commands are not supported");
        if(closed||process!=null)throw new IllegalStateException("Debug terminal is unavailable");
        var argv=Json.list(Json.get(arguments,"args")).stream().map(Json::string).toList();
        if(argv.isEmpty())throw new IllegalArgumentException("Terminal argv is empty");
        var builder=new ProcessBuilder(argv);
        String cwd=Json.string(Json.get(arguments,"cwd"));if(!cwd.isBlank())builder.directory(Path.of(cwd).toFile());
        Object env=Json.get(arguments,"env");if(env instanceof Map<?,?> map)for(var entry:map.entrySet()){
            String key=entry.getKey().toString();if(entry.getValue()==null)builder.environment().remove(key);else builder.environment().put(key,entry.getValue().toString());
        }
        try{process=builder.start();pump(process.getInputStream(),"stdout");pump(process.getErrorStream(),"stderr");return Json.object("processId",process.pid());}
        catch(IOException e){throw new UncheckedIOException(e);}
    }
    private void pump(InputStream stream,String category){Thread t=new Thread(()->{
        try(var reader=new InputStreamReader(stream,StandardCharsets.UTF_8)){char[] buffer=new char[2048];int count;while((count=reader.read(buffer))>=0)output.accept(category,new String(buffer,0,count));}
        catch(IOException e){synchronized(this){if(!closed)output.accept("stderr",e.getMessage());}}
    },"Bolt terminal "+category);t.setDaemon(true);t.start();}
    public OutputStream input(){return new OutputStream(){
        @Override public void write(int value)throws IOException{write(new byte[]{(byte)value});}
        @Override public void write(byte[] bytes,int offset,int length)throws IOException{synchronized(DebugTerminal.this){if(process==null||closed)throw new IOException("Debug terminal has not started");process.getOutputStream().write(bytes,offset,length);process.getOutputStream().flush();}}
    };}
    @Override public synchronized void close(){closed=true;if(process!=null){process.descendants().forEach(ProcessHandle::destroyForcibly);process.destroyForcibly();}}
}
