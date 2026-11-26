package dev.bolt.idea.core;

import java.util.*;
import java.util.function.Consumer;
import java.util.regex.Pattern;

/** Decode compiler-owned records; never infer that ordinary status means an error. */
public final class BoltConsole {
    public record Output(String text,String severity) { }
    public static final String PREFIX="\u001eBOLT1 ";
    private static final Pattern WARNING=Pattern.compile("(?i)^.*(?:warning(?:\\s+[A-Z]+\\d+)?\\s*:|warning\\s+BLT\\d+).*$");
    private static final Pattern ERROR=Pattern.compile("(?i)^.*(?:error(?:\\s+[A-Z]+\\d+)?\\s*:|fatal error|error\\s+BLT\\d+).*$");
    private final StringBuilder stderr=new StringBuilder(),stdout=new StringBuilder();
    private boolean application;
    private String diagnosticSeverity;
    public synchronized void accept(String chunk,Consumer<Output> output){accept(chunk,true,output);}
    public synchronized void accept(String chunk,boolean isErrorStream,Consumer<Output> output) {
        if(application&&!isErrorStream){output.accept(new Output(chunk,"info"));return;} // preserve application prompts without waiting for newline
        StringBuilder pending=isErrorStream?stderr:stdout;
        for(int i=0;i<chunk.length();i++) {
            char c=chunk.charAt(i);pending.append(c);
            if(c=='\n'){String line=pending.toString();pending.setLength(0);emit(line,isErrorStream,output);}
            else if(pending.length()>131072){output.accept(new Output(pending.toString(),isErrorStream?"error":"info"));pending.setLength(0);}
        }
    }
    public synchronized void finish(Consumer<Output> output){flush(stdout,false,output);flush(stderr,true,output);}
    private void flush(StringBuilder builder,boolean err,Consumer<Output> output){if(!builder.isEmpty()){String line=builder.toString();builder.setLength(0);emit(line,err,output);}}
    private void emit(String line,boolean err,Consumer<Output> output) {
        if(err&&line.startsWith(PREFIX))try {
            Object record=Json.parse(line.substring(PREFIX.length()).stripTrailing());
            if("run".equals(Json.get(record,"phase"))){flush(stdout,false,output);application=true;return;}
            String severity=Json.string(Json.get(record,"severity"));Object message=Json.get(record,"message");
            if(Set.of("info","warning","error").contains(severity)&&message instanceof String text){diagnosticSeverity=null;output.accept(new Output(text+"\n",severity));return;}
        }catch(RuntimeException ignored) { /* Malformed records stay visible. */ }
        String severity=err?"error":"info";
        if(!application){
            if(WARNING.matcher(line.stripTrailing()).matches())diagnosticSeverity=severity="warning";
            else if(ERROR.matcher(line.stripTrailing()).matches())diagnosticSeverity=severity="error";
            else if(diagnosticSeverity!=null&&!line.isBlank()&&(Character.isWhitespace(line.charAt(0))||line.startsWith("note:")))severity=diagnosticSeverity;
            else diagnosticSeverity=null;
        }
        output.accept(new Output(line,severity));
    }
}
