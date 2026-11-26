package dev.bolt.idea;

import com.intellij.execution.ExecutionException;
import com.intellij.execution.process.*;
import com.intellij.execution.ui.ConsoleViewContentType;
import com.intellij.openapi.application.ApplicationManager;
import com.intellij.openapi.fileTypes.FileType;
import com.intellij.openapi.vfs.LocalFileSystem;
import com.intellij.ui.ColoredTextContainer;
import com.intellij.ui.SimpleTextAttributes;
import com.intellij.xdebugger.*;
import com.intellij.xdebugger.breakpoints.*;
import com.intellij.xdebugger.evaluation.*;
import com.intellij.xdebugger.frame.*;
import com.intellij.xdebugger.impl.XSourcePositionImpl;
import dev.bolt.idea.core.*;
import org.jetbrains.annotations.NotNull;
import java.io.OutputStream;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

/** IntelliJ XDebugger frontend for LLVM source symbols and LLDB-DAP. */
public final class BoltDebugProcess extends XDebugProcess {
    private final DebugLaunch launch;
    private final DapClient dap;
    private final DebugOptions options;
    private final DebugSources sources;
    private final Map<String,com.intellij.openapi.vfs.VirtualFile> sourceFiles=new ConcurrentHashMap<>();
    private final DebugTerminal terminal;
    private final DebugFormatters formatters;
    private volatile Object capabilities;
    private DebugMemory memory;
    private DebugExpression expressionCompiler;
    private final String buildLog;
    private final CompletableFuture<Void> initialized=new CompletableFuture<>();
    private final ExecutorService commands=Executors.newSingleThreadExecutor(r->{Thread t=new Thread(r,"Bolt debugger commands");t.setDaemon(true);return t;});
    private final AtomicBoolean stopped=new AtomicBoolean(),started=new AtomicBoolean();
    private final AtomicLong generation=new AtomicLong();
    private final Map<String,List<XLineBreakpoint<BoltBreakpointType.Properties>>> breakpoints=new LinkedHashMap<>();
    private volatile boolean configured;
    private volatile int activeThread,exitCode;
    private volatile XSourcePosition temporary;
    private final DebugHandler handler=new DebugHandler();
    private final XDebuggerEditorsProvider editors=new XDebuggerEditorsProvider(){@Override public @NotNull FileType getFileType(){return BoltFileType.INSTANCE;}};
    public BoltDebugProcess(XDebugSession session,DebugLaunch launch,DebugOptions options,DebugSources sources,String buildLog)throws ExecutionException {
        super(session);this.launch=launch;this.options=options;this.sources=sources;this.buildLog=buildLog;
        terminal=new DebugTerminal((category,text)->event("output",Json.object("category",category,"output",text)));
        try{formatters=new DebugFormatters();dap=new DapClient(launch.adapter(),launch.cwd(),this::event,options.terminal()&&options.ownsProcess()?terminal::launch:null);memory=new DebugMemory(dap);expressionCompiler=new DebugExpression(dap,sources.input(),sources.compiler());}catch(Exception e){throw new ExecutionException("Cannot start LLDB-DAP: "+e.getMessage(),e);}
    }
    private void ui(Runnable task){ApplicationManager.getApplication().invokeLater(()->{if(!getSession().getProject().isDisposed())task.run();});}
    private static String error(Throwable e){while(e.getCause()!=null)e=e.getCause();return Objects.toString(e.getMessage(),e.toString());}
    private void failure(Throwable e){if(!stopped.get())ui(()->getSession().reportError(error(e)));}
    private Object request(String name,Object args)throws Exception{return dap.request(name,args).get(35,TimeUnit.SECONDS);}
    @Override public @NotNull XDebuggerEditorsProvider getEditorsProvider(){return editors;}
    @Override protected ProcessHandler doGetProcessHandler(){return handler;}
    @Override public void sessionInitialized(){
        if(!started.compareAndSet(false,true))return;
        getSession().getConsoleView().print(buildLog,ConsoleViewContentType.SYSTEM_OUTPUT);
        getSession().setPauseActionSupported(true);
        commands.execute(()->{
            try{
                capabilities=request("initialize",Json.object("adapterID","lldb","clientID","bolt-intellij","clientName","Bolt IntelliJ","pathFormat","path","linesStartAt1",true,"columnsStartAt1",true,"supportsVariableType",true,"supportsRunInTerminalRequest",options.terminal()&&options.ownsProcess()));
                var launched=dap.request(options.command(),formatters.arguments(options,launch));
                initialized.get(30,TimeUnit.SECONDS);
                List<String> files;synchronized(breakpoints){files=new ArrayList<>(breakpoints.keySet());}
                for(String file:files)syncBreakpoints(file);
                if(options.exceptions()){
                    if(!Boolean.TRUE.equals(Json.get(capabilities,"supportsFunctionBreakpoints")))throw new ExecutionException("Adapter cannot set Bolt exception breakpoints");
                    Object response=request("setFunctionBreakpoints",Json.object("breakpoints",List.of(Json.object("name","bolt_raise"),Json.object("name","bolt_rethrow"),Json.object("name","bolt_failure"))));
                    for(Object breakpoint:Json.list(Json.get(response,"breakpoints")))if(!Boolean.TRUE.equals(Json.get(breakpoint,"verified")))failure(new ExecutionException("Bolt exception hook not resolved: "+Json.string(Json.get(breakpoint,"message"))));
                }
                request("configurationDone",Json.object());configured=true;
                launched.get(35,TimeUnit.SECONDS);
                // Registrations arriving during configuration must also be sent.
                synchronized(breakpoints){files=new ArrayList<>(breakpoints.keySet());}
                for(String file:files)syncBreakpoints(file);
            }catch(Exception e){failure(e);stop();}
        });
    }
    private void event(String name,Object body){
        switch(name){
            case "initialized"->initialized.complete(null);
            case "output"->{String text=Json.string(Json.get(body,"output"));String category=Json.string(Json.get(body,"category"));ui(()->getSession().getConsoleView().print(text,category.equals("stderr")?ConsoleViewContentType.ERROR_OUTPUT:category.equals("stdout")?ConsoleViewContentType.NORMAL_OUTPUT:ConsoleViewContentType.SYSTEM_OUTPUT));}
            case "continued"->{generation.incrementAndGet();ui(()->getSession().sessionResumed());}
            case "thread"->{if(activeThread==0&&Json.string(Json.get(body,"reason")).equals("started"))activeThread=(int)Json.integer(Json.get(body,"threadId"),0);}
            case "stopped"->suspended((int)Json.integer(Json.get(body,"threadId"),0));
            case "exited"->exitCode=(int)Json.integer(Json.get(body,"exitCode"),0);
            case "terminated"->stop();
            case "adapterError"->{failure(new ExecutionException(Json.string(Json.get(body,"message"))));stop();}
            default->{ }
        }
    }
    @Override public XBreakpointHandler<?> @NotNull [] getBreakpointHandlers(){return new XBreakpointHandler<?>[]{new XBreakpointHandler<XLineBreakpoint<BoltBreakpointType.Properties>>(BoltBreakpointType.class){
        @Override public void registerBreakpoint(@NotNull XLineBreakpoint<BoltBreakpointType.Properties> breakpoint){
            String file=path(breakpoint);if(file==null)return;
            synchronized(breakpoints){var list=breakpoints.computeIfAbsent(file,k->new ArrayList<>());if(!list.contains(breakpoint))list.add(breakpoint);}
            updateBreakpoints(file);
        }
        @Override public void unregisterBreakpoint(@NotNull XLineBreakpoint<BoltBreakpointType.Properties> breakpoint,boolean temporary){
            String file=path(breakpoint);if(file==null)return;
            synchronized(breakpoints){var list=breakpoints.get(file);if(list!=null)list.remove(breakpoint);}
            updateBreakpoints(file);
        }
    }};}
    private static String path(XLineBreakpoint<?> b){var p=b.getSourcePosition();return p==null?null:p.getFile().getPath();}
    private void updateBreakpoints(String file){if(configured&&!stopped.get())commands.execute(()->{try{syncBreakpoints(file);}catch(Exception e){failure(e);}});}
    private void syncBreakpoints(String file)throws Exception {
        List<XLineBreakpoint<BoltBreakpointType.Properties>> snapshot;synchronized(breakpoints){snapshot=List.copyOf(breakpoints.getOrDefault(file,List.of()));}
        List<Object> points=new ArrayList<>();
        for(var b:snapshot){
            var point=Json.object("line",b.getLine()+1);
            if(b.getConditionExpression()!=null)point.put("condition",b.getConditionExpression().getExpression());
            if(b.getSuspendPolicy()==SuspendPolicy.NONE){
                if(!Boolean.TRUE.equals(Json.get(capabilities,"supportsLogPoints"))){ui(()->getSession().setBreakpointInvalid(b,"Adapter does not support logpoints"));return;}
                String message=b.getLogExpressionObject()==null?"Breakpoint at "+file+":"+(b.getLine()+1):"{"+b.getLogExpressionObject().getExpression()+"}";
                point.put("logMessage",message);
            }
            points.add(point);
        }
        var temp=temporary;if(temp!=null&&temp.getFile().getPath().equals(file))points.add(Json.object("line",temp.getLine()+1));
        Object result=request("setBreakpoints",Json.object("source",Json.object("path",file),"breakpoints",points,"sourceModified",false));
        var returned=Json.list(Json.get(result,"breakpoints"));
        for(int i=0;i<snapshot.size();i++){
            var b=snapshot.get(i);var response=i<returned.size()?returned.get(i):null;
            boolean verified=Boolean.TRUE.equals(Json.get(response,"verified"));String message=Json.string(Json.get(response,"message"));
            ui(()->{if(verified)getSession().setBreakpointVerified(b);else getSession().setBreakpointInvalid(b,message.isBlank()?"No executable LLVM location for this line":message);});
        }
    }
    private void control(String name){
        if(activeThread<=0||stopped.get())return;
        var ready=name.equals("pause")?CompletableFuture.completedFuture(null):expressionCompiler.release(0);
        ready.thenCompose(unused->dap.request(name,Json.object("threadId",activeThread))).whenComplete((v,e)->{if(e!=null)failure(e);});
    }
    @Override public void resume(@NotNull XSuspendContext context){control("continue");}
    @Override public void startStepOver(@NotNull XSuspendContext context){control("next");}
    @Override public void startStepInto(@NotNull XSuspendContext context){control("stepIn");}
    @Override public void startStepOut(@NotNull XSuspendContext context){control("stepOut");}
    @Override public void startPausing(){
        if(activeThread>0){control("pause");return;}
        dap.request("threads",Json.object()).whenComplete((v,e)->{if(e!=null){failure(e);return;}var threads=Json.list(Json.get(v,"threads"));if(!threads.isEmpty()){activeThread=(int)Json.integer(Json.get(threads.getFirst(),"id"),0);control("pause");}});
    }
    @Override public void runToPosition(@NotNull XSourcePosition position,XSuspendContext context){
        commands.execute(()->{try{var old=temporary;temporary=position;if(old!=null)syncBreakpoints(old.getFile().getPath());syncBreakpoints(position.getFile().getPath());control("continue");}catch(Exception e){failure(e);}});
    }
    private void suspended(int thread){
        final long epoch=generation.incrementAndGet();activeThread=thread;
        if(temporary!=null){String file=temporary.getFile().getPath();temporary=null;updateBreakpoints(file);}
        dap.request("threads",Json.object()).thenCompose(body->{
            List<Object> threads=new ArrayList<>(Json.list(Json.get(body,"threads")));
            if(activeThread==0&&!threads.isEmpty())activeThread=(int)Json.integer(Json.get(threads.getFirst(),"id"),0);
            int current=activeThread;
            return dap.request("stackTrace",Json.object("threadId",current,"startFrame",0,"levels",100)).thenAcceptAsync(stack->{
                var frames=frames(stack,epoch);List<Stack> stacks=new ArrayList<>();
                for(Object item:threads){int id=(int)Json.integer(Json.get(item,"id"),0);stacks.add(new Stack(id,Json.string(Json.get(item,"name")),id==current?frames:null,epoch));}
                Stack selected=stacks.stream().filter(s->s.id==current).findFirst().orElse(new Stack(current,"Thread "+current,frames,epoch));
                var context=new XSuspendContext(){@Override public XExecutionStack getActiveExecutionStack(){return selected;}@Override public XExecutionStack @NotNull [] getExecutionStacks(){return stacks.toArray(XExecutionStack[]::new);}};
                ui(()->{if(generation.get()==epoch&&!stopped.get())getSession().positionReached(context);});
            });
        }).exceptionally(e->{failure(e);return null;});
    }
    private List<Frame> frames(Object body,long epoch){return Json.list(Json.get(body,"stackFrames")).stream().map(value->new Frame(value,epoch)).toList();}
    private final class Stack extends XExecutionStack {
        final int id;final long epoch;volatile List<Frame> frames;
        Stack(int id,String name,List<Frame> frames,long epoch){super(name);this.id=id;this.frames=frames;this.epoch=epoch;}
        @Override public XStackFrame getTopFrame(){return frames==null||frames.isEmpty()?null:frames.getFirst();}
        @Override public void computeStackFrames(int first,@NotNull XStackFrameContainer container){
            activeThread=id;
            if(frames!=null){container.addStackFrames(frames.subList(Math.min(first,frames.size()),frames.size()),true);return;}
            dap.request("stackTrace",Json.object("threadId",id,"startFrame",0,"levels",100)).whenCompleteAsync((v,e)->{if(e!=null)container.errorOccurred(error(e));else if(generation.get()==epoch){frames=frames(v,epoch);container.addStackFrames(frames.subList(Math.min(first,frames.size()),frames.size()),true);}});
        }
    }
    private final class Frame extends XStackFrame {
        final Object data;final int id;final long epoch;
        Frame(Object data,long epoch){this.data=data;this.id=(int)Json.integer(Json.get(data,"id"),0);this.epoch=epoch;
            String path=Json.string(Json.get(Json.get(data,"source"),"path"));String uri=DebugSources.uri(path);
            if(uri!=null&&!sourceFiles.containsKey(uri))try{String text=sources.resolve(path);if(text!=null){var file=new com.intellij.testFramework.LightVirtualFile(uri.substring(uri.lastIndexOf('/')+1),BoltFileType.INSTANCE,text);file.setWritable(false);sourceFiles.put(uri,file);}}catch(Exception e){failure(e);}
        }
        @Override public Object getEqualityObject(){return List.of(epoch,id);}
        @Override public XSourcePosition getSourcePosition(){String path=Json.string(Json.get(Json.get(data,"source"),"path"));if(path.isBlank())return null;String uri=DebugSources.uri(path);var file=uri==null?LocalFileSystem.getInstance().findFileByPath(path.replace('\\','/')):sourceFiles.get(uri);return file==null?null:XSourcePositionImpl.create(file,Math.max(0,(int)Json.integer(Json.get(data,"line"),1)-1));}
        @Override public void customizePresentation(@NotNull ColoredTextContainer component){component.append(Json.string(Json.get(data,"name")),SimpleTextAttributes.REGULAR_ATTRIBUTES);}
        @Override public void computeChildren(@NotNull XCompositeNode node){
            dap.request("scopes",Json.object("frameId",id)).whenComplete((v,e)->{
                if(e!=null){node.setErrorMessage(error(e));return;}if(generation.get()!=epoch)return;
                List<CompletableFuture<Object>> requests=new ArrayList<>();List<Object> parents=new ArrayList<>();
                for(Object scope:Json.list(Json.get(v,"scopes")))if(!Boolean.TRUE.equals(Json.get(scope,"expensive"))){parents.add(Json.get(scope,"variablesReference"));requests.add(dap.request("variables",Json.object("variablesReference",Json.get(scope,"variablesReference"))));}
                CompletableFuture.allOf(requests.toArray(CompletableFuture[]::new)).whenComplete((unused,err)->{
                    if(err!=null){node.setErrorMessage(error(err));return;}if(generation.get()!=epoch)return;
                    Map<String,Set<String>> readonly=new HashMap<>();
                    for(var scope:requests)for(Object variable:Json.list(Json.get(scope.join(),"variables"))){String name=Json.string(Json.get(variable,"name"));if(name.startsWith("__bolt_readonly_"))readonly.computeIfAbsent(name.substring("__bolt_readonly_".length()),unusedName->new HashSet<>()).add(Json.string(Json.get(variable,"memoryReference")));}
                    var children=new XValueChildrenList();
                    for(int i=0;i<requests.size();i++)for(Object variable:Json.list(Json.get(requests.get(i).join(),"variables"))){
                        String name=Json.string(Json.get(variable,"name"));if(name.startsWith("__bolt_"))continue;
                        var value=new Value(variable,epoch,parents.get(i));value.frameId=id;value.constant=readonly.getOrDefault(name,Set.of()).contains(Json.string(Json.get(variable,"memoryReference")));children.add(name,value);
                    }
                    node.addChildren(children,true);
                });
            });
        }
        @Override public XDebuggerEvaluator getEvaluator(){return new XDebuggerEvaluator(){
            @Override public void evaluate(@NotNull String expression,@NotNull XEvaluationCallback callback,XSourcePosition source){
                new BoltWatch(dap,expressionCompiler).evaluate(expression,id).whenComplete((v,e)->{if(e!=null)callback.errorOccurred(error(e));else if(generation.get()!=epoch)callback.errorOccurred("Execution has resumed");else callback.evaluated(new Value(BoltDebugValuePresentation.fromEvaluation(v),epoch));});
            }
        };}
    }
    private final class Value extends XValue {
        volatile Object data;int frameId;boolean constant;final long epoch;final int reference;final Object parent;final CompletableFuture<DebugMemory.Snapshot> decoded;
        Value(Object data,long epoch){this(data,epoch,null);}
        Value(Object data,long epoch,Object parent){this(data,epoch,parent,true);}
        Value(Object data,long epoch,Object parent,boolean decode){this.data=data;this.epoch=epoch;this.parent=parent;reference=(int)Json.integer(Json.get(data,"variablesReference"),0);
            decoded=decode&&Boolean.TRUE.equals(Json.get(capabilities,"supportsReadMemoryRequest"))&&DebugMemory.supported(Json.string(Json.get(data,"type")))?CompletableFuture.supplyAsync(()->{
                if(generation.get()!=epoch)return null;try{return memory.snapshot(data);}catch(Exception e){return null;}
            }):CompletableFuture.completedFuture(null);
        }
        @Override public XValueModifier getModifier(){
            if(parent==null||frameId<=0||constant)return null;
            String target=Json.string(Json.get(data,"evaluateName"));if(target.isEmpty())target=Json.string(Json.get(data,"name"));
            if(target.startsWith("$"))return null;
            final String binding=target;
            return new XValueModifier(){@Override public void setValue(@NotNull XExpression expression,@NotNull XModificationCallback callback){
                if(generation.get()!=epoch){callback.errorOccurred("Execution has resumed");return;}
                expressionCompiler.evaluate(binding+" = ("+expression.getExpression()+")",frameId).whenComplete((v,e)->{
                    if(e!=null)callback.errorOccurred(error(e));else if(generation.get()!=epoch)callback.errorOccurred("Execution has resumed");else callback.valueModified();
                });
            }};
        }
        @Override public void computePresentation(@NotNull XValueNode node,@NotNull XValuePlace place){
            node.setPresentation(null,new BoltDebugValuePresentation(data),reference>0||!Json.list(Json.get(data,"children")).isEmpty());
            decoded.thenAccept(snapshot->{if(snapshot!=null&&generation.get()==epoch){var formatted=Json.object("value",snapshot.summary(),"type",Json.get(data,"type"),"memoryReference",Json.get(data,"memoryReference"));node.setPresentation(null,new BoltDebugValuePresentation(formatted),!snapshot.children().isEmpty()||reference>0);}});
        }
        @Override public void computeChildren(@NotNull XCompositeNode node){decoded.thenAccept(snapshot->{
            if(generation.get()!=epoch)return;
            var inline=Json.list(Json.get(data,"children"));if(!inline.isEmpty()){var children=new XValueChildrenList();for(Object variable:inline)children.add(Json.string(Json.get(variable,"name")),new Value(variable,epoch,null,false));node.addChildren(children,true);return;}
            if(snapshot!=null){var children=new XValueChildrenList();for(Object variable:snapshot.children())children.add(Json.string(Json.get(variable,"name")),new Value(variable,epoch,null,false));if(reference>0)children.add("[native storage]",new Value(data,epoch,parent,false));node.addChildren(children,true);return;}
            dap.request("variables",Json.object("variablesReference",reference)).whenComplete((v,e)->{if(e!=null)node.setErrorMessage(error(e));else if(generation.get()==epoch){var children=new XValueChildrenList();for(Object variable:Json.list(Json.get(v,"variables"))){var value=new Value(variable,epoch,reference);value.frameId=frameId;value.constant=constant;children.add(Json.string(Json.get(variable,"name")),value);}node.addChildren(children,true);}});
        });}
    }
    @Override public void stop(){
        if(!stopped.compareAndSet(false,true))return;
        configured=false;generation.incrementAndGet();initialized.completeExceptionally(new ExecutionException("Debugger stopped"));
        ApplicationManager.getApplication().executeOnPooledThread(()->{
            try{try{expressionCompiler.release(0).get(20,TimeUnit.SECONDS);}catch(Exception ignored){}dap.request("disconnect",Json.object("terminateDebuggee",options.ownsProcess())).get(3,TimeUnit.SECONDS);}catch(Exception ignored) { }
            finally{dap.close();terminal.close();formatters.close();sources.close();commands.shutdownNow();handler.finished(exitCode);}
        });
    }
    private final class DebugHandler extends ProcessHandler {
        private final AtomicBoolean finished=new AtomicBoolean();
        void finished(int code){if(finished.compareAndSet(false,true))notifyProcessTerminated(code);}
        @Override protected void destroyProcessImpl(){BoltDebugProcess.this.stop();}
        @Override protected void detachProcessImpl(){BoltDebugProcess.this.stop();}
        @Override public boolean detachIsDefault(){return false;}
        @Override public OutputStream getProcessInput(){return options.terminal()&&options.ownsProcess()?terminal.input():null;}
    }
}
