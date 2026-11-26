package dev.bolt.idea.core;

import java.io.*;
import java.nio.file.*;
import java.util.*;

/** Session-owned LLDB formatter resource; extracted unchanged from the plugin. */
public final class DebugFormatters implements AutoCloseable {
    private final Path directory,file,evaluator;
    public DebugFormatters()throws IOException{
        directory=Files.createTempDirectory("bolt-debug-formatters-");file=directory.resolve("bolt_formatters.py");evaluator=directory.resolve("bolt_evaluator.py");
        try(var input=DebugFormatters.class.getResourceAsStream("/debugger/bolt_formatters.py")){
            if(input==null)throw new IOException("Bolt debugger formatter resource is missing");Files.copy(input,file);
            try(var engine=DebugFormatters.class.getResourceAsStream("/debugger/bolt_evaluator.py")){if(engine==null)throw new IOException("Bolt evaluator resource is missing");Files.copy(engine,evaluator);}
        }catch(IOException e){try{Files.deleteIfExists(evaluator);Files.deleteIfExists(file);Files.deleteIfExists(directory);}catch(IOException cleanup){e.addSuppressed(cleanup);}throw e;}
    }
    private static String path(Path file){return "\""+file.toString().replace('\\','/').replace("\"","\\\"")+"\"";}
    public List<String> commands(){return List.of("command script import "+path(file),"command script import "+path(evaluator),"command script add -f bolt_evaluator.evaluate bolt-evaluate");}
    public Map<String,Object> arguments(DebugOptions options,DebugLaunch launch){var args=options.arguments(launch);args.put("initCommands",commands());return args;}
    @Override public void close(){try(var files=Files.walk(directory)){for(Path path:files.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(path);}catch(IOException ignored){}}
}
