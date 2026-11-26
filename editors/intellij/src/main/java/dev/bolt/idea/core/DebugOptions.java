package dev.bolt.idea.core;

import java.nio.file.*;
import java.util.*;

/** DAP connection policy independent of the IDE. Remote uses LLDB gdb-remote. */
public record DebugOptions(String mode,long pid,String remote,boolean terminal,boolean exceptions,Map<String,String> sourceMap) {
    public static DebugOptions launch(){return new DebugOptions("Launch",0,"",false,false,Map.of());}
    public DebugOptions {
        if(!Set.of("Launch","Attach","Remote").contains(mode))throw new IllegalArgumentException("Unknown debug mode");
        if(mode.equals("Attach")&&pid<=0)throw new IllegalArgumentException("Attach requires a positive process ID");
        if(mode.equals("Remote")&&!remote.matches("[A-Za-z0-9_.\\[\\]:-]+:[0-9]{1,5}"))throw new IllegalArgumentException("Remote address must be host:port (IPv6: [host]:port)");
        if(mode.equals("Remote")){int port=Integer.parseInt(remote.substring(remote.lastIndexOf(':')+1));if(port<1||port>65535)throw new IllegalArgumentException("Invalid remote port");}
        sourceMap=Map.copyOf(sourceMap);
    }
    public boolean ownsProcess(){return mode.equals("Launch");}
    public String command(){return ownsProcess()?"launch":"attach";}
    public Map<String,Object> arguments(DebugLaunch launch){
        var args=ownsProcess()?launch.dapArguments():Json.object("program",launch.executable().toString());
        if(mode.equals("Attach")){args.put("pid",pid);args.put("stopOnEntry",true);}
        if(mode.equals("Remote")){args.put("attachCommands",List.of("gdb-remote "+remote));args.put("stopOnEntry",true);}
        if(ownsProcess()&&terminal)args.put("console","integratedTerminal");
        args.put("sourceMap",sourceMap.entrySet().stream().map(e->List.of(e.getKey(),e.getValue())).toList());
        args.put("enableSyntheticChildDebugging",true);
        return args;
    }
    public static Map<String,String> parseSourceMap(String text){
        Map<String,String> result=new LinkedHashMap<>();
        for(String row:text.split("\\R"))if(!row.isBlank()){int split=row.indexOf('=');if(split<1||row.substring(split+1).isBlank())throw new IllegalArgumentException("Source mappings must be remote-path=local-path, one per line");result.put(row.substring(0,split).strip(),row.substring(split+1).strip());}
        return result;
    }
}
