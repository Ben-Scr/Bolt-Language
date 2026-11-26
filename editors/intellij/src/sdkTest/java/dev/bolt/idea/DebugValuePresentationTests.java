package dev.bolt.idea;

import com.intellij.xdebugger.frame.presentation.XValuePresentation.XValueTextRenderer;
import dev.bolt.idea.core.Json;
import java.lang.reflect.Proxy;
import java.util.*;

/** Exercise the actual IntelliJ renderer contract, including unaddressable values. */
public final class DebugValuePresentationTests {
    private static List<String> render(Object data){
        var chunks=new ArrayList<String>();
        var renderer=(XValueTextRenderer)Proxy.newProxyInstance(XValueTextRenderer.class.getClassLoader(),new Class<?>[]{XValueTextRenderer.class},(proxy,method,args)->{chunks.add(method.getName()+":"+args[0]);return null;});
        new BoltDebugValuePresentation(data).renderValue(renderer);return chunks;
    }
    private static void check(boolean value,String message){if(!value)throw new AssertionError(message);}
    public static void main(String[] args){
        Object pointer=Json.object("type","int *","value","0x1000","memoryReference","0x2000");
        check(render(pointer).equals(List.of("renderValue:0x1000","renderComment: @ 0x2000")),"pointer contents and storage address remain distinct");
        Object watch=BoltDebugValuePresentation.fromEvaluation(Json.object("result","42","type","int","memoryReference","0x3000","variablesReference",0));
        check(new BoltDebugValuePresentation(watch).getType().equals("int"),"type retained");
        check(render(watch).equals(List.of("renderValue:42","renderComment: @ 0x3000")),"evaluation retains its adapter-provided address");
        check(render(Json.object("value","<unavailable>")).equals(List.of("renderValue:<unavailable>")),"no invented address for unavailable values");
        check(render(Json.object("value","9","memoryReference"," ")).equals(List.of("renderValue:9")),"blank address is omitted");
        System.out.println("Debugger SDK value/address rendering checks passed");
    }
}
