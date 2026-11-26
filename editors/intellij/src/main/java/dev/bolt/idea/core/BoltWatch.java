package dev.bolt.idea.core;

import java.io.IOException;
import java.util.*;
import java.util.concurrent.*;
import java.util.regex.*;

/** Compiler-backed Bolt watches, with a memory-only fast path for stored values. */
public final class BoltWatch {
    private final DapClient dap;
    private final DebugExpression compiler;
    private static final Pattern ROOT=Pattern.compile("^([A-Za-z_][A-Za-z_0-9]*)(.*)$");
    private static final Pattern MEMBER=Pattern.compile("^\\.([A-Za-z_][A-Za-z_0-9]*)(.*)$");
    private static final Pattern INDEX=Pattern.compile("^\\[([0-9]+)\\](.*)$");
    private static final Pattern ENTRY=Pattern.compile("^\\.EntryAt\\(([0-9]+)\\)(.*)$");
    private static final Pattern ITEM=Pattern.compile("^\\.ItemAt\\(([0-9]+)\\)(.*)$");
    public BoltWatch(DapClient dap){this(dap,null);}
    public BoltWatch(DapClient dap,DebugExpression compiler){this.dap=dap;this.compiler=compiler;}
    private Object request(String name,Object args)throws Exception{return dap.request(name,args).get(30,TimeUnit.SECONDS);}
    private Object child(Object value,String name)throws Exception{
        long reference=Json.integer(Json.get(value,"variablesReference"),0);if(reference==0)throw new IOException("Value has no fields/elements");
        for(Object variable:Json.list(Json.get(request("variables",Json.object("variablesReference",reference)),"variables")))if(Json.string(Json.get(variable,"name")).equals(name))return variable;
        throw new IOException("No stored field or displayed element '"+name+"'. Bolt getters/methods require a compiler evaluation bridge; collection display is limited to 128 elements.");
    }
    private Object dictionaryValue(Object value,String key)throws Exception{
        for(Object entry:Json.list(Json.get(request("variables",Json.object("variablesReference",Json.get(value,"variablesReference"))),"variables"))){
            if(!Json.string(Json.get(entry,"name")).startsWith("["))continue;
            Object stored=child(entry,"Key");String type=Json.string(Json.get(stored,"type"));
            if(!Set.of("byte","short","ushort","int","uint","long","ulong").contains(type))throw new IOException("Dictionary key evaluation currently requires a stored integer key");
            if(Json.string(Json.get(stored,"value")).equals(key))return child(entry,"Value");
        }
        throw new IOException("Dictionary key was not found in the first 128 displayed entries");
    }
    private static Object evaluated(Object variable){return Json.object("result",Json.get(variable,"value"),"type",Json.get(variable,"type"),"variablesReference",Json.get(variable,"variablesReference"),"memoryReference",Json.get(variable,"memoryReference"));}
    public CompletableFuture<Object> evaluate(String expression,int frame){
        if(compiler!=null&&!expression.strip().startsWith("native:")){
            String text=expression.strip();
            if(text.matches("[A-Za-z_][A-Za-z_0-9]*(?:\\.[A-Za-z_][A-Za-z_0-9]*|\\[[0-9]+\\])*")){
                return CompletableFuture.supplyAsync(()->{
                    try{
                        Matcher root=ROOT.matcher(text);root.matches();String name=root.group(1);
                        boolean local=false;for(Object scope:Json.list(Json.get(request("scopes",Json.object("frameId",frame)),"scopes")))
                            if(!Boolean.TRUE.equals(Json.get(scope,"expensive")))for(Object value:Json.list(Json.get(request("variables",Json.object("variablesReference",Json.get(scope,"variablesReference"))),"variables")))if(Json.string(Json.get(value,"name")).equals(name)&&!name.startsWith("__bolt_"))local=true;
                        if(!local)throw new IOException("Compiler expression required");
                        return null;
                    }catch(Exception e){throw new CompletionException(e);}
                }).thenCompose(unused->new BoltWatch(dap).evaluate(text,frame)).exceptionallyCompose(error->compiler.evaluate(expression,frame));
            }
            return compiler.evaluate(expression,frame);
        }
        return CompletableFuture.supplyAsync(()->{
        try{
            String text=expression.strip();
            if(text.startsWith("native:"))return request("evaluate",Json.object("expression",text.substring(7).strip(),"frameId",frame,"context","watch"));
            Matcher root=ROOT.matcher(text);
            if(!root.matches()||root.group(2).isEmpty())return request("evaluate",Json.object("expression",text,"frameId",frame,"context","watch"));
            String suffix=root.group(2);
            // Scalar arithmetic and dereferencing retain the existing native expression support.
            if(!suffix.startsWith(".")&&!suffix.startsWith("[")){
                if(text.contains("("))throw new IOException("Bolt method/operator execution is not available; use native: only for explicit LLDB expressions");
                return request("evaluate",Json.object("expression",text,"frameId",frame,"context","watch"));
            }
            Object result=request("evaluate",Json.object("expression",root.group(1),"frameId",frame,"context","watch"));
            Object value=Json.object("value",Json.get(result,"result"),"type",Json.get(result,"type"),"variablesReference",Json.get(result,"variablesReference"),"memoryReference",Json.get(result,"memoryReference"));
            int steps=0;
            while(!suffix.isEmpty()){
                if(++steps>32)throw new IOException("Watch value path is too deep");
                Matcher item=ITEM.matcher(suffix),entry=ENTRY.matcher(suffix),index=INDEX.matcher(suffix),member=MEMBER.matcher(suffix);
                String type=Json.string(Json.get(value,"type"));
                if(item.matches()){
                    if(!(type.startsWith("System.Collections.Generic.List<")||type.startsWith("System.Collections.Generic.Queue<")||type.startsWith("System.Collections.Generic.HashSet<")))throw new IOException("ItemAt is only supported for List, Queue and HashSet");
                    value=child(value,"["+Integer.parseInt(item.group(1))+"]");suffix=item.group(2);
                }else if(entry.matches()){
                    if(!type.startsWith("System.Collections.Generic.Dictionary<"))throw new IOException("EntryAt is only supported for Dictionary");
                    value=child(value,"["+Integer.parseInt(entry.group(1))+"]");suffix=entry.group(2);
                }else if(index.matches()){
                    if(type.equals("string"))value=new DebugMemory(dap).stringAccess(value,Integer.parseInt(index.group(1)));
                    else if(type.startsWith("System.Collections.Generic.Dictionary<"))value=dictionaryValue(value,index.group(1));
                    else if(type.endsWith("]")||type.startsWith("System.Collections.Generic.List<"))value=child(value,"["+Integer.parseInt(index.group(1))+"]");
                    else throw new IOException("This Bolt type has no supported numeric indexer");
                    suffix=index.group(2);
                }
                else if(member.matches()){
                    String name=member.group(1);
                    if(name.equals("Count")&&type.startsWith("System.Collections.Generic."))name="Length";
                    if(name.equals("Length")&&type.equals("string"))value=new DebugMemory(dap).stringAccess(value,null);else value=child(value,name);
                    suffix=member.group(2);
                }else throw new IOException("Bolt getter/method/operator execution requires a compiler evaluation bridge");
            }
            return evaluated(value);
        }catch(Exception e){throw new CompletionException(e);}
    });}
}
