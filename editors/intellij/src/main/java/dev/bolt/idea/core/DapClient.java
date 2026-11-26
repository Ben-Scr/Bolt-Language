package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.function.BiConsumer;

/** LLDB-DAP stdio transport. Debuggee output comes only from DAP output events. */
public final class DapClient implements AutoCloseable {
    private final Process process;
    private final Map<Long,CompletableFuture<Object>> pending=new ConcurrentHashMap<>();
    private final AtomicLong sequence=new AtomicLong();
    private final AtomicBoolean closed=new AtomicBoolean();
    private final BiConsumer<String,Object> events;
    private final java.util.function.BiFunction<String,Object,Object> reverse;
    private final ExecutorService dispatch=Executors.newSingleThreadExecutor(r->daemon("Bolt debugger events",r,false));
    private final StringBuilder errors=new StringBuilder();
    public DapClient(Path adapter,Path cwd,BiConsumer<String,Object> events)throws IOException {
        this(adapter,cwd,events,null);
    }
    public DapClient(Path adapter,Path cwd,BiConsumer<String,Object> events,java.util.function.BiFunction<String,Object,Object> reverse)throws IOException {
        this.events=events;this.reverse=reverse;
        process=new ProcessBuilder(adapter.toString()).directory(cwd.toFile()).start();
        daemon("Bolt debugger protocol",this::readLoop,true);
        daemon("Bolt debugger stderr",()->{
            try(var reader=new InputStreamReader(process.getErrorStream(),StandardCharsets.UTF_8)){
                char[] b=new char[1024];int n;
                while((n=reader.read(b))>=0)synchronized(errors){errors.append(b,0,n);if(errors.length()>8192)errors.delete(0,errors.length()-8192);}
            }catch(IOException ignored) { }
        },true);
    }
    private static Thread daemon(String name,Runnable r,boolean start){Thread t=new Thread(r,name);t.setDaemon(true);if(start)t.start();return t;}
    public CompletableFuture<Object> request(String command,Object arguments){
        return request(command,arguments,30);
    }
    public CompletableFuture<Object> request(String command,Object arguments,long timeoutSeconds){
        long id=sequence.incrementAndGet();var reply=new CompletableFuture<Object>();
        if(closed.get()){reply.completeExceptionally(new IOException("Debugger is closed"));return reply;}
        if(pending.size()>=128){reply.completeExceptionally(new IOException("Too many debugger requests"));return reply;}
        pending.put(id,reply);
        try{send(Json.object("seq",id,"type","request","command",command,"arguments",arguments));}
        catch(IOException e){pending.remove(id);reply.completeExceptionally(e);}
        reply.orTimeout(timeoutSeconds,TimeUnit.SECONDS).whenComplete((v,e)->pending.remove(id));
        return reply;
    }
    private synchronized void send(Object packet)throws IOException{
        byte[] bytes=Json.write(packet).getBytes(StandardCharsets.UTF_8);
        var out=process.getOutputStream();out.write(("Content-Length: "+bytes.length+"\r\n\r\n").getBytes(StandardCharsets.US_ASCII));out.write(bytes);out.flush();
    }
    static Object read(InputStream in)throws IOException{
        int length=-1,total=0;var line=new ByteArrayOutputStream();
        while(true){
            int c=in.read();if(c<0)throw new EOFException("Debugger protocol ended");
            if(++total>8192)throw new IOException("Debugger header too large");
            if(c=='\n'){
                String text=line.toString(StandardCharsets.US_ASCII).strip();line.reset();
                if(text.isEmpty())break;
                int colon=text.indexOf(':');
                if(colon>0&&text.substring(0,colon).equalsIgnoreCase("Content-Length")){
                    if(length!=-1)throw new IOException("Duplicate debugger length");
                    try{length=Integer.parseInt(text.substring(colon+1).strip());}catch(NumberFormatException e){throw new IOException("Invalid debugger length",e);}
                }
            }else line.write(c);
        }
        if(length<0||length>8*1024*1024)throw new IOException("Invalid debugger frame size");
        byte[] data=in.readNBytes(length);if(data.length!=length)throw new EOFException("Truncated debugger frame");
        try{return Json.parse(new String(data,StandardCharsets.UTF_8));}catch(IllegalArgumentException e){throw new IOException("Invalid debugger JSON",e);}
    }
    private void event(String name,Object body){if(!dispatch.isShutdown())try{dispatch.execute(()->events.accept(name,body));}catch(RejectedExecutionException ignored) { }}
    private void readLoop(){
        try{
            while(!closed.get()){
                Object packet=read(process.getInputStream());String type=Json.string(Json.get(packet,"type"));
                if(type.equals("response")){
                    var reply=pending.remove(Json.integer(Json.get(packet,"request_seq"),-1));
                    if(reply!=null){if(Boolean.TRUE.equals(Json.get(packet,"success")))reply.complete(Json.get(packet,"body"));else reply.completeExceptionally(new IOException(Json.string(Json.get(packet,"message"))));}
                }else if(type.equals("event"))event(Json.string(Json.get(packet,"event")),Json.get(packet,"body"));
                else if(type.equals("request")) {
                    // Never block the protocol reader: terminal launch can itself await adapter output.
                    dispatch.execute(()->{
                        String command=Json.string(Json.get(packet,"command"));
                        var response=Json.object("seq",sequence.incrementAndGet(),"type","response","request_seq",Json.get(packet,"seq"),"command",command);
                        try {if(reverse==null)throw new IOException("Unsupported adapter request: "+command);response.put("body",reverse.apply(command,Json.get(packet,"arguments")));response.put("success",true);}
                        catch(Exception e){response.put("success",false);response.put("message",Objects.toString(e.getMessage(),e.toString()));}
                        try{send(response);}catch(IOException e){failPending(e);}
                    });
                }
            }
        }catch(Exception e){
            if(!closed.get()){String tail;synchronized(errors){tail=errors.toString();}event("adapterError",Json.object("message",e.getMessage()+(tail.isBlank()?"":"\n"+tail)));}
        }finally{failPending(new IOException("Debugger adapter disconnected"));}
    }
    private void failPending(IOException error){pending.values().forEach(f->f.completeExceptionally(error));pending.clear();}
    @Override public void close(){
        if(!closed.compareAndSet(false,true))return;
        failPending(new IOException("Debugger stopped"));
        process.descendants().forEach(ProcessHandle::destroy);process.destroy();
        try{if(!process.waitFor(2,TimeUnit.SECONDS)){process.descendants().forEach(ProcessHandle::destroyForcibly);process.destroyForcibly();}}
        catch(InterruptedException e){Thread.currentThread().interrupt();process.destroyForcibly();}
        dispatch.shutdown();
    }
}
