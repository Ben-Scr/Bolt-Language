package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

/** The native CLI is the only TOML/project resolver. No duplicate IDE build model. */
public record BoltProjectInfo(Path file, Path root, String name, String kind,
                              String defaultProfile, List<Path> sources,
                              List<Path> sourceRoots, List<Entry> entries, String sourceHash) {
    public record Entry(Path file, int line, long byteOffset, String sourceHash) { }
    public static BoltProjectInfo parse(String json) throws IOException {
        try {
            Object model = Json.parse(json);
            if (Json.integer(Json.get(model,"schema_version"),0)!=1) throw new IOException("Unsupported Bolt project-info schema; update compiler and plugin together.");
            Path root=Path.of(Json.string(Json.get(model,"root")));
            Path file=Path.of(Json.string(Json.get(model,"project_file")));
            if(!root.isAbsolute()||!file.isAbsolute())throw new IOException("Bolt project model returned relative paths.");
            List<Path> sources=paths(Json.get(model,"sources")), roots=paths(Json.get(model,"source_roots"));
            List<Entry> entries=new ArrayList<>();
            for(Object e:Json.list(Json.get(model,"entrypoints")))entries.add(new Entry(Path.of(Json.string(Json.get(e,"file"))), (int)Json.integer(Json.get(e,"line"),0),Json.integer(Json.get(e,"offset"),-1),Json.string(Json.get(e,"source_hash"))));
            return new BoltProjectInfo(file,root,Json.string(Json.get(model,"name")),Json.string(Json.get(model,"kind")),Json.string(Json.get(model,"default_profile")),sources,roots,List.copyOf(entries),Json.string(Json.get(model,"source_hash")));
        }catch(IllegalArgumentException e){throw new IOException("Invalid Bolt project information: "+e.getMessage(),e);}
    }
    private static List<Path> paths(Object value){return Json.list(value).stream().map(v->Path.of(Json.string(v))).toList();}
    public static BoltProjectInfo query(Path compiler,Path input) throws IOException {
        Process process=new ProcessBuilder(compiler.toString(),"project","info",input.toAbsolutePath().toString(),"--format","json").start();
        ExecutorService readers=Executors.newFixedThreadPool(2,r->{Thread t=new Thread(r,"Bolt project output");t.setDaemon(true);return t;});
        Future<String> stdout=readers.submit(()->read(process.getInputStream()));
        Future<String> stderr=readers.submit(()->read(process.getErrorStream()));
        try{
            if(!process.waitFor(30,TimeUnit.SECONDS))throw new IOException("Bolt project inspection timed out.");
            String text=stdout.get(5,TimeUnit.SECONDS),error=stderr.get(5,TimeUnit.SECONDS);
            if(process.exitValue()!=0)throw new IOException(error.isBlank()?"Bolt project inspection failed.":error.strip());
            return parse(text);
        }catch(InterruptedException e){Thread.currentThread().interrupt();throw new IOException("Project inspection cancelled",e);}
        catch(ExecutionException|TimeoutException e){throw new IOException("Cannot read Bolt project output",e);}
        finally{process.descendants().forEach(ProcessHandle::destroy);process.destroyForcibly();readers.shutdownNow();}
    }
    private static String read(InputStream stream)throws IOException{
        byte[] bytes=stream.readNBytes(4*1024*1024+1);if(bytes.length>4*1024*1024)throw new IOException("Bolt project output exceeded 4 MiB");return new String(bytes,StandardCharsets.UTF_8);
    }
    public static int editorOffset(Entry entry,String source){
        if(source==null||!entry.sourceHash().equals(sourceHash(source)))return -1;
        byte[] utf8=source.getBytes(StandardCharsets.UTF_8);
        if(entry.byteOffset()<0||entry.byteOffset()>utf8.length)return -1;
        return new String(utf8,0,(int)entry.byteOffset(),StandardCharsets.UTF_8).replace("\r\n","\n").length();
    }
    public static String sourceHash(String source){long hash=0xcbf29ce484222325L;for(byte b:source.getBytes(StandardCharsets.UTF_8)){hash^=(b&255);hash*=0x100000001b3L;}return Long.toUnsignedString(hash);}
    public boolean executable(){return kind.equals("executable");}
}
