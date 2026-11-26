package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

/** Uses the production Java build planner and transport against a real LLDB adapter. */
public final class DebugTests {
    private static void check(boolean value,String reason){if(!value)throw new AssertionError(reason);}
    private static Object request(DapClient dap,String command,Object args)throws Exception{return dap.request(command,args).get(35,TimeUnit.SECONDS);}
    private static Object event(BlockingQueue<Map.Entry<String,Object>> queue,String name)throws Exception {
        long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(30);
        while(true){var entry=queue.poll(Math.max(1,end-System.nanoTime()),TimeUnit.NANOSECONDS);if(entry==null)throw new AssertionError("Missing debugger event: "+name);if(entry.getKey().equals("adapterError"))throw new AssertionError(entry.getValue());if(entry.getKey().equals(name))return entry.getValue();}
    }
    private static void framing()throws Exception {
        byte[] body=Json.write(Json.object("type","event","event","output","body",Json.object("output","Grüße 😀"))).getBytes(StandardCharsets.UTF_8);
        var bytes=new ByteArrayOutputStream();bytes.write(("Content-Length: "+body.length+"\r\n\r\n").getBytes(StandardCharsets.US_ASCII));bytes.write(body);
        check(Json.string(Json.get(DapClient.read(new ByteArrayInputStream(bytes.toByteArray())),"event")).equals("output"),"UTF-8 frame uses byte length");
        for(String bad:List.of("Content-Length: -1\r\n\r\n","Content-Length: 9000000\r\n\r\n","Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}","Content-Length: 2\r\n\r\n{")){
            try{DapClient.read(new ByteArrayInputStream(bad.getBytes(StandardCharsets.US_ASCII)));throw new AssertionError("Accepted malformed frame");}catch(IOException expected) { }
        }
    }
    private static void storage(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Storage.bolt");
        Files.writeString(source,"import System.Collections.Generic\npublic static class Program {\npublic: int Main(){\nstring text=\"Grüße 😀\";\nint[] numbers=[3,7,11];\nList<int> list; list.Add(13); list.Add(17);\nQueue<int> queue; queue.Enqueue(19); queue.Enqueue(23);\nreturn numbers[0]+list[0]+queue.Peek();\n}\n}\n");
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb"));var launched=dap.request("launch",launch.dapArguments());event(events,"initialized");
            request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",8))));request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);
            Object stop=event(events,"stopped"),thread=Json.get(stop,"threadId");Object frame=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",thread)),"stackFrames")).getFirst();
            var memory=new DebugMemory(dap);Map<String,Object> locals=new HashMap<>();
            for(Object scope:Json.list(Json.get(request(dap,"scopes",Json.object("frameId",Json.get(frame,"id"))),"scopes")))for(Object variable:Json.list(Json.get(request(dap,"variables",Json.object("variablesReference",Json.get(scope,"variablesReference"))),"variables")))locals.put(Json.string(Json.get(variable,"name")),variable);
            check(memory.snapshot(locals.get("text")).summary().equals(Json.write("Grüße 😀")),"real UTF-8 string snapshot");
            var array=memory.snapshot(locals.get("numbers"));check(array.children().size()==3&&Json.string(Json.get(array.children().get(2),"value")).equals("11"),"real primitive array elements");
            var list=memory.snapshot(locals.get("list"));check(list.children().size()==2&&Json.string(Json.get(list.children().get(1),"value")).equals("17"),"real generic List storage");
            var queue=memory.snapshot(locals.get("queue"));check(queue.children().size()==2&&Json.string(Json.get(queue.children().get(0),"value")).equals("19"),"real generic Queue order");
            try(var sources=new DebugSources(compiler,source)){check(sources.resolve("<bolt-stdlib>/System/NativeString.bolt").contains("class NativeString"),"actual compiler embedded source lookup");}
            request(dap,"disconnect",Json.object("terminateDebuggee",true));
        }
    }
    private static void formatters(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Formatters.bolt");Files.writeString(source,"import System.Collections.Generic\npublic static class Program {public: int Main(){\nstring text=\"Grüße 😀\";\nstring[] words=[\"one\",\"two\"];\nList<int> list; list.Add(13); list.Add(17);\nQueue<int> queue; queue.Enqueue(19); queue.Enqueue(23); queue.Dequeue(); queue.Enqueue(29);\nDictionary<int,string> dictionary;dictionary.Add(31,\"thirty-one\");\nHashSet<int> set;set.Add(37); int[] emptyArray; string empty=\"\"; int[] many=new int[140]; for(int i=0;i<140;i++)many[i]=i; string longText=\"\";for(int i=0;i<4100;i++)longText+=\"x\";\nreturn 0;\n}}\n");
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var formatters=new DebugFormatters();var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb"));var launched=dap.request("launch",formatters.arguments(DebugOptions.launch(),launch));event(events,"initialized");
            request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",9))));request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);Object stop=event(events,"stopped"),thread=Json.get(stop,"threadId");
            Object frame=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",thread)),"stackFrames")).getFirst();Map<String,Object> locals=new HashMap<>();
            for(Object scope:Json.list(Json.get(request(dap,"scopes",Json.object("frameId",Json.get(frame,"id"))),"scopes")))for(Object variable:Json.list(Json.get(request(dap,"variables",Json.object("variablesReference",Json.get(scope,"variablesReference"))),"variables")))locals.put(Json.string(Json.get(variable,"name")),variable);
            check(Json.string(Json.get(locals.get("text"),"value")).contains("Grüße 😀"),"LLDB UTF-8 summary");
            check(Json.string(Json.get(locals.get("empty"),"value")).equals("\"\""),"default empty string summary");
            check(Json.string(Json.get(locals.get("emptyArray"),"value")).contains("Length = 0"),"default empty array summary");
            check(Json.string(Json.get(locals.get("longText"),"value")).contains("4100 UTF-8 bytes"),"long text is explicitly truncated");
            var bounded=Json.list(Json.get(request(dap,"variables",Json.object("variablesReference",Json.get(locals.get("many"),"variablesReference"))),"variables"));
            check(bounded.stream().filter(v->Json.string(Json.get(v,"name")).matches("\\[[0-9]+\\]")).count()==128,"array expansion bounded to 128 actual elements");
            check(bounded.stream().anyMatch(v->Json.string(Json.get(v,"name")).equals("[127]")&&Json.string(Json.get(v,"value")).equals("127")),"bounded array includes final permitted element");
            for(String name:List.of("words","list","queue","dictionary","set")){
                var variable=locals.get(name);check(Json.string(Json.get(variable,"value")).contains("Length ="),"native formatter length for "+name);
                var children=Json.list(Json.get(request(dap,"variables",Json.object("variablesReference",Json.get(variable,"variablesReference"))),"variables"));check(!children.isEmpty(),"native formatter elements for "+name);
                if(name.equals("queue"))check(Json.string(Json.get(children.getFirst(),"value")).equals("23"),"queue displays logical front after transfer");
                if(name.equals("words"))check(Json.string(Json.get(children.get(1),"value")).contains("two"),"nested string formatter");
                if(name.equals("dictionary")){var entry=Json.list(Json.get(request(dap,"variables",Json.object("variablesReference",Json.get(children.getFirst(),"variablesReference"))),"variables"));check(entry.stream().anyMatch(v->Json.string(Json.get(v,"name")).equals("Value")&&Json.string(Json.get(v,"value")).contains("thirty-one")),"typed generic dictionary entry fields");}
            }
            var watches=new BoltWatch(dap);int frameId=(int)Json.integer(Json.get(frame,"id"),0);
            check(Json.string(Json.get(watches.evaluate("list.Length",frameId).get(30,TimeUnit.SECONDS),"result")).equals("2"),"Bolt collection Length watch");
            check(Json.string(Json.get(watches.evaluate("queue.ItemAt(0)",frameId).get(30,TimeUnit.SECONDS),"result")).equals("23"),"Bolt built-in ItemAt watch");
            check(Json.string(Json.get(watches.evaluate("dictionary.EntryAt(0).Value",frameId).get(30,TimeUnit.SECONDS),"result")).contains("thirty-one"),"Bolt generic entry field watch");
            check(Json.string(Json.get(watches.evaluate("dictionary[31]",frameId).get(30,TimeUnit.SECONDS),"result")).contains("thirty-one"),"dictionary watch uses key semantics");
            check(Json.string(Json.get(watches.evaluate("set.ItemAt(0)",frameId).get(30,TimeUnit.SECONDS),"result")).equals("37"),"HashSet ItemAt returns key");
            check(Json.string(Json.get(watches.evaluate("text.Length",frameId).get(30,TimeUnit.SECONDS),"result")).equals("7"),"String Length uses Unicode scalars");
            check(Json.string(Json.get(watches.evaluate("text[6]",frameId).get(30,TimeUnit.SECONDS),"result")).equals("128512"),"String index uses Unicode scalars");
            request(dap,"disconnect",Json.object("terminateDebuggee",true));
        }
    }
    private static void semantic(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Semantic.bolt");Files.writeString(source,"""
            public class Box {public:
            static int Calls=0;
            int Value;
            Box(int value){Value=value;}
            int Computed {get {Calls++;return Value+1;}}
            int Add(int delta){Value+=delta;return Value;}
            int Inspect(int delta){int probe=Value;
            return probe+delta; // instance watch
            }
            static Box operator +(Box a,Box b){return Box(a.Value+b.Value);}
            }
            public class GenericBox<T> {public:T Value;GenericBox(T value){Value=value;}}
            public class Tracked {public:
            static int Drops=0;int Value;Tracked(int value){Value=value;}~Tracked(){Drops++;}
            }
            public static class Program {public:
            T Identity<T>(T value){return value;}
            int Failing(){throw System.Exception("watch failure");}
            string Owned(){return "owned 😀";}
            int Main(){
            int count=40;
            const int fixedValue=9;
            Box box=Box(5);Identity<int>(0);Box.Calls=7;
            try {throw System.Exception("prior catch");}catch(System.Exception original){
            int marker=0;
            if(count+box.Value+Box.Calls+marker!=56)return 99;
            return box.Inspect(3)==11?0:97;
            }
            return 98;
            }}
            """);
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var formatters=new DebugFormatters();var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb"));var launched=dap.request("launch",formatters.arguments(DebugOptions.launch(),launch));event(events,"initialized");
            Object checked=request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",Files.readString(source).lines().toList().indexOf("int marker=0;")+1))));System.out.println("semantic breakpoint "+Json.write(checked));request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);Object stop=event(events,"stopped"),thread=Json.get(stop,"threadId");
            Object frame=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",thread)),"stackFrames")).getFirst();int frameId=(int)Json.integer(Json.get(frame,"id"),0);
            var engine=new DebugExpression(dap,source,compiler);var watch=new BoltWatch(dap,engine);
            Object value=watch.evaluate("count + 2",frameId).get(90,TimeUnit.SECONDS);System.out.println("SEMANTIC "+Json.write(value));check(Json.string(Json.get(value,"result")).equals("42"),"compiler-checked Bolt arithmetic against target local");
            check(Json.string(Json.get(watch.evaluate("count + 2",frameId).get(90,TimeUnit.SECONDS),"result")).equals("42"),"cached thunk refresh reads current target bindings");
            Object property=watch.evaluate("box.Computed + Identity<int>(count) + Identity<long>(2L)",frameId).get(90,TimeUnit.SECONDS);System.out.println("semantic property "+Json.write(property));check(Json.string(Json.get(property,"result")).equals("48"),"user getter and existing/new generic method specializations");
            check(Json.string(Json.get(watch.evaluate("Box.Calls",frameId).get(90,TimeUnit.SECONDS),"result")).equals("8"),"getter changes existing target static exactly once");System.out.println("Semantic getter/generic/static checks passed");
            check(Json.string(Json.get(watch.evaluate("box.Add(3)",frameId).get(90,TimeUnit.SECONDS),"result")).equals("8"),"instance method changes actual stopped local");
            check(Json.string(Json.get(watch.evaluate("(box + Box(4)).Value",frameId).get(90,TimeUnit.SECONDS),"result")).equals("12"),"Bolt operator overload");
            check(Json.string(Json.get(watch.evaluate("Owned()",frameId).get(90,TimeUnit.SECONDS),"result")).contains("owned 😀"),"owning native string result");System.out.println("Semantic instance/operator/string checks passed");
            try{watch.evaluate("fixedValue = 10",frameId).get(90,TimeUnit.SECONDS);throw new AssertionError("Const assignment accepted");}catch(ExecutionException expected){check(expected.toString().contains("const"),"const mutation fails compiler typing");}
            try{watch.evaluate("Failing()",frameId).get(90,TimeUnit.SECONDS);throw new AssertionError("Throwing expression accepted");}catch(ExecutionException expected){check(expected.toString().contains("watch failure"),"watch exception reports actual Bolt message");}
            check(Json.string(Json.get(watch.evaluate("count + 2",frameId).get(90,TimeUnit.SECONDS),"result")).equals("42"),"failed watch does not poison context");
            Object owning=watch.evaluate("Tracked(17)",frameId).get(90,TimeUnit.SECONDS);check(Json.string(Json.get(owning,"type")).equals("Tracked"),"owning class result retains correct type");
            Object beforeDrop=watch.evaluate("Tracked.Drops",frameId).get(90,TimeUnit.SECONDS);
            engine.release(frameId).get(90,TimeUnit.SECONDS);
            Object dropped=watch.evaluate("Tracked.Drops",frameId).get(90,TimeUnit.SECONDS);check(Integer.parseInt(Json.string(Json.get(dropped,"result")))-Integer.parseInt(Json.string(Json.get(beforeDrop,"result")))==1,"session release drops owning class result once");System.out.println("Semantic const/error/ownership checks passed");
            Object generic=watch.evaluate("GenericBox<long>(23L)",frameId).get(90,TimeUnit.SECONDS);check(Json.string(Json.get(generic,"type")).equals("GenericBox<long>"),"new generic result type");
            check(Json.list(Json.get(generic,"children")).stream().anyMatch(v->Json.string(Json.get(v,"name")).equals("Value")&&Json.string(Json.get(v,"value")).equals("23")),"new generic fields decoded with compiler layout");
            engine.release(frameId).get(90,TimeUnit.SECONDS);
            int instanceLine=Files.readString(source).lines().toList().indexOf("return probe+delta; // instance watch")+1;
            request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",instanceLine))));
            request(dap,"continue",Json.object("threadId",thread));Object instanceStop=event(events,"stopped");Object instanceFrame=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",Json.get(instanceStop,"threadId"))),"stackFrames")).getFirst();int instanceId=(int)Json.integer(Json.get(instanceFrame,"id"),0);
            check(Json.string(Json.get(watch.evaluate("Value + probe + delta",instanceId).get(90,TimeUnit.SECONDS),"result")).equals("19"),"this/parameter/local bindings in selected instance frame");
            engine.release(instanceId).get(90,TimeUnit.SECONDS);
            request(dap,"continue",Json.object("threadId",thread));check(Json.integer(Json.get(event(events,"exited"),"exitCode"),-1)==0,"prior caught exception and target state preserved after watches");
            request(dap,"disconnect",Json.object("terminateDebuggee",true));System.out.println("Native Bolt semantic watch checks passed");
        }
    }
    private static void bindings(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Bindings.bolt");Files.writeString(source,"""
            enum Mode {One,Two}
            public static class Program {public:int Main(){
            long wide=4294967297L;ulong huge=18446744073709551614ul;
            uint unsigned=4u;short signed=(short)-2;ushort small=(ushort)3;
            string text="initial";int[] values=[3,4];Mode mode=Mode.One;
            int marker=0;
            return wide==4294967297L&&text=="replacement"&&values[0]==11&&mode==Mode.Two?0:99;
            }}
            """);
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var formatters=new DebugFormatters();var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb"));var launched=dap.request("launch",formatters.arguments(DebugOptions.launch(),launch));event(events,"initialized");
            request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",6))));request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);Object stop=event(events,"stopped"),thread=Json.get(stop,"threadId");
            Object frame=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",thread)),"stackFrames")).getFirst();int frameId=(int)Json.integer(Json.get(frame,"id"),0);
            var engine=new DebugExpression(dap,source,compiler);var watch=new BoltWatch(dap,engine);
            Object numeric=watch.evaluate("wide + (long)unsigned + signed + small",frameId).get(120,TimeUnit.SECONDS);check(Json.string(Json.get(numeric,"result")).equals("4294967302"),"CodeView fundamental aliases retain Bolt widths in locals");
            check(Json.string(Json.get(watch.evaluate("text = \"replacement\"",frameId).get(120,TimeUnit.SECONDS),"result")).contains("replacement"),"owning string assignment uses compiler hooks");
            check(Json.string(Json.get(watch.evaluate("values[0] = 11",frameId).get(120,TimeUnit.SECONDS),"result")).equals("11"),"actual target array assignment");
            check(Json.string(Json.get(watch.evaluate("mode = Mode.Two",frameId).get(120,TimeUnit.SECONDS),"result")).contains("Two"),"enum assignment keeps declared Bolt type");
            String original=Files.readString(source);try{Files.writeString(source,original+"// changed source\n");var mismatch=new DebugExpression(dap,source,compiler);try{mismatch.evaluate("wide + 2L",frameId).get(120,TimeUnit.SECONDS);throw new AssertionError("Mismatched source accepted");}catch(ExecutionException expected){check(expected.toString().contains("source snapshot differs"),"source mismatch rejected before invocation");}}finally{Files.writeString(source,original);}
            engine.release(frameId).get(90,TimeUnit.SECONDS);request(dap,"continue",Json.object("threadId",thread));check(Json.integer(Json.get(event(events,"exited"),"exitCode"),-1)==0,"compiler assignment survives release and continuation");request(dap,"disconnect",Json.object("terminateDebuggee",true));
            System.out.println("Native width/owning/enum/array/source validation checks passed");
        }
    }
    private static void terminal(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Input.bolt");Files.writeString(source,"public static class Program { public: int Main(){string input=System.Console.ReadLine(); System.Console.WriteLine(input);return input==\"hello debugger\"?0:91;} }\n");
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();StringBuffer output=new StringBuffer();
        try(var terminal=new DebugTerminal((category,text)->output.append(text));var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)),terminal::launch)){
            request(dap,"initialize",Json.object("adapterID","lldb","supportsRunInTerminalRequest",true));
            var options=new DebugOptions("Launch",0,"",true,false,Map.of());var launched=dap.request("launch",options.arguments(launch));event(events,"initialized");request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);
            terminal.input().write("hello debugger\n".getBytes(StandardCharsets.UTF_8));
            check(Json.integer(Json.get(event(events,"exited"),"exitCode"),-1)==0,"interactive native console reads input");event(events,"terminated");
            long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);while(!output.toString().contains("hello debugger")&&System.nanoTime()<end)Thread.sleep(10);check(output.toString().contains("hello debugger"),"terminal forwards actual UTF-8 output");
            request(dap,"disconnect",Json.object("terminateDebuggee",true));
        }
    }
    private static void attach(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Attach.bolt");Files.writeString(source,"public static class Program {public:int Main(){int count=0;while(true){count=(count+1)%1000;}return count;}}\n");
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var target=new ProcessBuilder(launch.executable().toString()).directory(root.toFile()).start();
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb"));var options=new DebugOptions("Attach",target.pid(),"",false,false,Map.of());
            var attached=dap.request(options.command(),options.arguments(launch));event(events,"initialized");request(dap,"configurationDone",Json.object());attached.get(35,TimeUnit.SECONDS);event(events,"stopped");
            request(dap,"disconnect",Json.object("terminateDebuggee",options.ownsProcess()));check(target.isAlive(),"Attach disconnect preserves existing target");
        }finally{target.destroyForcibly();target.waitFor(5,TimeUnit.SECONDS);}
    }
    private static void remote(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Remote.bolt");Files.writeString(source,"public static class Program {public:int Main(){int count=42;return count;}}\n");
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        int port;try(var socket=new java.net.ServerSocket(0)){port=socket.getLocalPort();}
        Path serverExecutable=adapter.resolveSibling(System.getProperty("os.name").startsWith("Windows")?"lldb-server.exe":"lldb-server");
        Path log=root.resolve("remote-server.log");
        var server=new ProcessBuilder(serverExecutable.toString(),"gdbserver","127.0.0.1:"+port,launch.executable().toString()).directory(root.toFile()).redirectErrorStream(true).redirectOutput(log.toFile()).start();
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            Thread.sleep(200);if(!server.isAlive())throw new IOException("Remote server rejected launch: "+Files.readString(log));
            request(dap,"initialize",Json.object("adapterID","lldb"));var options=new DebugOptions("Remote",0,"127.0.0.1:"+port,false,false,Map.of());
            var attached=dap.request(options.command(),options.arguments(launch));event(events,"initialized");request(dap,"configurationDone",Json.object());attached.get(35,TimeUnit.SECONDS);Object stop=event(events,"stopped");
            var frames=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",Json.get(stop,"threadId"))),"stackFrames"));check(!frames.isEmpty(),"actual gdb-remote stack frames");
            request(dap,"disconnect",Json.object("terminateDebuggee",false));System.out.println("Native remote connection passed");
        }finally{server.descendants().forEach(ProcessHandle::destroyForcibly);server.destroyForcibly();server.waitFor(5,TimeUnit.SECONDS);}
    }
    private static void exception(Path compiler,Path adapter,Path root)throws Exception {
        Path source=root.resolve("Throw.bolt");Files.writeString(source,"public static class Program {public:int Main(){try {throw System.Exception(\"debug throw\");}catch(System.Exception e){return 0;}return 1;}}\n");
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of(),Map.of(),System.out::print,()->false);
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        try(var dap=new DapClient(adapter,root,(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb"));var launched=dap.request("launch",launch.dapArguments());event(events,"initialized");
            Object result=request(dap,"setFunctionBreakpoints",Json.object("breakpoints",List.of(Json.object("name","bolt_raise"))));check(Boolean.TRUE.equals(Json.get(Json.list(Json.get(result,"breakpoints")).getFirst(),"verified")),"actual Bolt throw symbol resolves");
            request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);Object stop=event(events,"stopped");
            var frames=Json.list(Json.get(request(dap,"stackTrace",Json.object("threadId",Json.get(stop,"threadId"))),"stackFrames"));check(frames.stream().anyMatch(f->Json.string(Json.get(f,"name")).contains("bolt_raise")),"Bolt exception breakpoint stops in raise hook");
            request(dap,"setFunctionBreakpoints",Json.object("breakpoints",List.of()));request(dap,"continue",Json.object("threadId",Json.get(stop,"threadId")));check(Json.integer(Json.get(event(events,"exited"),"exitCode"),-1)==0,"catch executes after exception breakpoint");request(dap,"disconnect",Json.object("terminateDebuggee",true));
        }
    }
    public static void main(String[] args)throws Exception {
        framing();
        var defaults=DebugOptions.launch();check(defaults.ownsProcess()&&defaults.command().equals("launch"),"launch owns target");
        var attach=new DebugOptions("Attach",123,"",false,false,Map.of());check(!attach.ownsProcess()&&attach.command().equals("attach"),"attach detaches target");
        var remote=new DebugOptions("Remote",0,"localhost:1234",false,false,DebugOptions.parseSourceMap("/remote=C:/local\n/second=C:/second"));check(remote.sourceMap().size()==2,"multiple source mappings");
        check(DebugSources.uri("C:/build/<bolt-stdlib>/System/NativeExecution.bolt").equals("bolt-stdlib:///System/NativeExecution.bolt"),"embedded path mapping");
        for(String endpoint:List.of("host:0","host:65536","host:12;quit","host:12\nquit")){try{new DebugOptions("Remote",0,endpoint,false,false,Map.of());throw new AssertionError("Accepted invalid remote endpoint");}catch(IllegalArgumentException expected){}}

        if(args.length==0){System.out.println("Debugger framing checks passed");return;}
        Path compiler=Path.of(args[0]).toAbsolutePath(),adapter=Path.of(args[1]).toAbsolutePath(),root=Path.of(args[2]).toAbsolutePath();Files.createDirectories(root);
        if(args.length>3&&args[3].equals("remote")){remote(compiler,adapter,root);return;}
        if(args.length>3&&args[3].equals("bindings")){bindings(compiler,adapter,root);return;}
        if(args.length>3&&args[3].equals("semantic")){semantic(compiler,adapter,root);return;}
        Path source=root.resolve("Main.bolt");
        Files.writeString(source,"public static class Program {\npublic:\n int Main(string[] args) {\n  int count = 38 + args.Length;\n  count += 2;\n  if (System.Environment.GetVariable(\"BOLT_DEBUG_TEST\") != \"works\") return 99;\n  return count;\n }\n}\n");
        var events=new LinkedBlockingQueue<Map.Entry<String,Object>>();
        var launch=DebugLaunch.prepare(compiler,source,adapter.toString(),root.toString(),true,List.of("argument with spaces",""),Map.of("BOLT_DEBUG_TEST","works"),System.out::print,()->false);
        check(launch.arguments().equals(List.of("argument with spaces","")),"debug arguments preserved");
        check(launch.environment().get("BOLT_DEBUG_TEST").equals("works"),"debug environment preserved");
        try(var dap=new DapClient(launch.adapter(),launch.cwd(),(name,body)->events.add(Map.entry(name,body==null?Map.of():body)))){
            request(dap,"initialize",Json.object("adapterID","lldb","linesStartAt1",true,"columnsStartAt1",true,"pathFormat","path"));
            var launched=dap.request("launch",launch.dapArguments());event(events,"initialized");
            var bp=request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",5))));
            check(Boolean.TRUE.equals(Json.get(Json.list(Json.get(bp,"breakpoints")).getFirst(),"verified")),"real breakpoint verified");
            request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);
            Object stop=event(events,"stopped");Object thread=Json.get(stop,"threadId");
            Object stack=request(dap,"stackTrace",Json.object("threadId",thread));var frame=Json.list(Json.get(stack,"stackFrames")).getFirst();
            Object value=request(dap,"evaluate",Json.object("expression","count","context","watch","frameId",Json.get(frame,"id")));
            check(Json.string(Json.get(value,"result")).equals("40"),"real Java transport reads typed local");
            String storage=Json.string(Json.get(value,"memoryReference"));
            check(!storage.isBlank(),"LLDB supplies the local's storage address");
            Object address=request(dap,"evaluate",Json.object("expression","&count","context","watch","frameId",Json.get(frame,"id")));
            check(Long.parseUnsignedLong(storage.replaceFirst("^0[xX]",""),16)==Long.parseUnsignedLong(Json.string(Json.get(address,"result")).replaceFirst("^0[xX]",""),16),"displayed storage address equals the actual address-of expression");
            var scopes=Json.list(Json.get(request(dap,"scopes",Json.object("frameId",Json.get(frame,"id"))),"scopes"));
            boolean assigned=false;
            for(Object scope:scopes){Object parent=Json.get(scope,"variablesReference");var locals=Json.list(Json.get(request(dap,"variables",Json.object("variablesReference",parent)),"variables"));
                for(Object local:locals)if(Json.string(Json.get(local,"name")).equals("count")){
                    Object changed=request(dap,"setVariable",Json.object("variablesReference",parent,"name","count","value","41"));check(Json.string(Json.get(changed,"value")).equals("41"),"native scalar assignment");
                    request(dap,"setVariable",Json.object("variablesReference",parent,"name","count","value","40"));assigned=true;
                }
            }
            check(assigned,"local variable is assignable through actual parent scope");
            request(dap,"next",Json.object("threadId",thread));event(events,"stopped");
            stack=request(dap,"stackTrace",Json.object("threadId",thread));frame=Json.list(Json.get(stack,"stackFrames")).getFirst();
            value=request(dap,"evaluate",Json.object("expression","count","context","watch","frameId",Json.get(frame,"id")));
            check(Json.string(Json.get(value,"result")).equals("42"),"step over changes local");
            request(dap,"setBreakpoints",Json.object("source",Json.object("path",source.toString()),"breakpoints",List.of(Json.object("line",7,"logMessage","BOLT_LOG count={count}"))));
            request(dap,"continue",Json.object("threadId",thread));check(Json.string(Json.get(event(events,"output"),"output")).contains("BOLT_LOG count=42"),"real logpoint evaluates local without stopping");check(Json.integer(Json.get(event(events,"exited"),"exitCode"),0)==42,"native exit status");event(events,"terminated");
            request(dap,"disconnect",Json.object("terminateDebuggee",true));
        }
        storage(compiler,adapter,root);
        formatters(compiler,adapter,root);
        if(args.length>3&&args[3].equals("terminal"))terminal(compiler,adapter,root);
        attach(compiler,adapter,root);
        exception(compiler,adapter,root);
        // A compiler failure cannot launch an older artifact.
        Files.writeString(source,"not valid Bolt;");
        try{DebugLaunch.prepare(compiler,source,adapter.toString(),"",false,List.of(),Map.of(),unused->{},()->false);throw new AssertionError("Invalid build accepted");}catch(IOException expected) { }
        Path project=root.resolve("manifest");Files.createDirectories(project.resolve("src"));Files.createDirectories(project.resolve("working"));
        Files.writeString(project.resolve("bolt.proj"),"format = 1\n[project]\nname = \"DebugProject\"\n");
        Files.writeString(project.resolve("bolt.conf"),"format = 1\n[run]\nworking_directory = \"working\"\narguments = [\"configured argument\", \"\"]\n");
        Files.writeString(project.resolve("src/Main.bolt"),"public static class Program {public:int Main(){return 0;}}\n");
        var model=DebugLaunch.prepare(compiler,project,adapter.toString(),"",false,List.of(),Map.of(),System.out::print,()->false);
        check(model.arguments().equals(List.of("configured argument","")),"project run arguments retained");
        check(model.cwd().equals(project.resolve("working")),"project working directory retained");
        check(model.executable().getParent().getFileName().toString().equals("Debug"),"project Debug artifact selected");
        Files.writeString(project.resolve("bolt.conf"),"format = 1\n[profiles.Debug]\noptimization = \"aggressive\"\n");
        try{DebugLaunch.prepare(compiler,project,adapter.toString(),"",false,List.of(),Map.of(),unused->{},()->false);throw new AssertionError("Optimized debug build accepted");}catch(IOException expected) { }
        Files.writeString(source,"public static class Program {public:int Main(){int count=0;while(true){count=(count+1)%1000;}return count;}}\n");
        var running=DebugLaunch.prepare(compiler,source,adapter.toString(),"",true,List.of(),Map.of(),System.out::print,()->false);
        events.clear();var processId=new AtomicLong();
        try(var dap=new DapClient(adapter,root,(name,body)->{if(name.equals("process"))processId.set(Json.integer(Json.get(body,"systemProcessId"),0));events.add(Map.entry(name,body==null?Map.of():body));})){
            request(dap,"initialize",Json.object("adapterID","lldb"));var launched=dap.request("launch",running.dapArguments());event(events,"initialized");request(dap,"configurationDone",Json.object());launched.get(35,TimeUnit.SECONDS);
            var threads=Json.list(Json.get(request(dap,"threads",Json.object()),"threads"));check(!threads.isEmpty(),"running threads enumerated");
            request(dap,"pause",Json.object("threadId",Json.get(threads.getFirst(),"id")));event(events,"stopped");
            request(dap,"disconnect",Json.object("terminateDebuggee",true));
        }
        if(processId.get()>0){long end=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);while(ProcessHandle.of(processId.get()).map(ProcessHandle::isAlive).orElse(false)&&System.nanoTime()<end)Thread.sleep(20);check(!ProcessHandle.of(processId.get()).map(ProcessHandle::isAlive).orElse(false),"Stop terminates the debuggee");}
        System.out.println("Production Java debugger checks passed: framing, build, args/env, locals/addresses, mutation, logpoints, memory decoding, typed generic formatters, built-in watches, embedded sources, attach/detach, exceptions, stepping, exit and project model/build rejection");
    }
}
