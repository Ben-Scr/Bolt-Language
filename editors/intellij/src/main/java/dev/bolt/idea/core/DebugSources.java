package dev.bolt.idea.core;

import java.io.IOException;
import java.nio.file.*;
import java.util.*;

/** Resolves compiler-embedded source through the same compiler's virtualDocument API. */
public final class DebugSources implements AutoCloseable {
    private final BoltSession session=new BoltSession();
    private final Path compiler,context,workspace,input;
    private final Map<String,String> cache=new HashMap<>();
    public DebugSources(Path compiler,Path input)throws IOException{
        this.compiler=compiler;this.input=input;session.setProfile("Debug");
        Path root=Files.isDirectory(input)?input:input.getParent();workspace=root;
        if(input.toString().endsWith(".bolt"))context=input;
        else {
            Path sources=Files.isDirectory(root.resolve("src"))?root.resolve("src"):root;
            try(var files=Files.walk(sources,4)){context=files.filter(Files::isRegularFile).filter(p->p.toString().endsWith(".bolt")).sorted().findFirst().orElse(null);}
        }
    }
    public Path compiler(){return compiler;}
    public Path input(){return input;}
    public static String uri(String path){
        String normalized=path.replace('\\','/');int start=normalized.indexOf("<bolt-stdlib>/");
        if(start>=0)return "bolt-stdlib:///"+normalized.substring(start+14);
        return normalized.startsWith("bolt-stdlib:///")?normalized:null;
    }
    public synchronized String resolve(String path)throws IOException{
        String uri=uri(path);if(uri==null||context==null)return null;
        String existing=cache.get(uri);if(existing!=null)return existing;
        String source=Files.readString(context);
        Object result=session.query(compiler,workspace,Map.of(context,source),context,0,"bolt/virtualDocument",0,Json.object("uri",uri),()->{});
        Object text=Json.get(result,"text");if(!(text instanceof String value))throw new IOException("Embedded debugger source is unavailable: "+uri);
        cache.put(uri,value);return value;
    }
    @Override public synchronized void close(){session.close();}
}
