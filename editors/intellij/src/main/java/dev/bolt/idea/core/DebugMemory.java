package dev.bolt.idea.core;

import java.io.IOException;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.*;

/** Read-only decoding of ABI 0x000F0014/0x000F0015 storage prefixes. Never calls ToString or mutates a borrow. */
public final class DebugMemory {
    public record Snapshot(String summary,List<Object> children) { }
    private final DapClient dap;
    public DebugMemory(DapClient dap){this.dap=dap;}
    public static boolean supported(String type){return type.equals("string")||type.endsWith("]")||type.startsWith("System.Collections.Generic.List<")||type.startsWith("System.Collections.Generic.Queue<")||type.startsWith("System.Collections.Generic.Dictionary<")||type.startsWith("System.Collections.Generic.HashSet<");}
    private Object request(String name,Object args)throws Exception{return dap.request(name,args).get(30,TimeUnit.SECONDS);}
    private byte[] read(long address,int count)throws Exception{
        if(count<0||count>65536||address==0)throw new IOException("Invalid debug storage read");
        Object response=request("readMemory",Json.object("memoryReference","0x"+Long.toUnsignedString(address,16),"count",count));
        byte[] bytes=Base64.getDecoder().decode(Json.string(Json.get(response,"data")));
        if(bytes.length!=count||Json.integer(Json.get(response,"unreadableBytes"),0)!=0)throw new IOException("Debug storage is unreadable");
        return bytes;
    }
    private long number(Object variable)throws IOException{
        String text=Json.string(Json.get(variable,"value")).strip();
        try{return text.startsWith("0x")?Long.parseUnsignedLong(text.substring(2).split(" ",2)[0],16):Long.parseLong(text);}catch(NumberFormatException e){throw new IOException("Cannot read debug field value: "+text);}
    }
    private List<?> fields(Object variable)throws Exception{
        return Json.list(Json.get(request("variables",Json.object("variablesReference",Json.get(variable,"variablesReference"))),"variables"));
    }
    private Object field(Object variable,String name)throws Exception{for(Object child:fields(variable))if(Json.string(Json.get(child,"name")).equals(name))return child;throw new IOException("Missing debug storage field: "+name);}
    private long handle(Object variable)throws Exception{return number(field(variable,"__handle"));}
    private static long word(byte[] bytes,int offset){return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).getLong(offset);}
    private String string(long address)throws Exception{
        if(address==0)return "\"\"";
        long length=word(read(address,16),8);if(length<0)throw new IOException("Invalid UTF-8 byte length");
        int count=(int)Math.min(length,4096);String text=new String(read(address+16,count),StandardCharsets.UTF_8);
        return Json.write(text)+(length>count?" ... ("+length+" UTF-8 bytes)":"");
    }
    public Object stringAccess(Object variable,Integer index)throws Exception {
        long address=handle(variable);String text="";
        if(address!=0){long length=word(read(address,16),8);if(length<0||length>1048576)throw new IOException("String watch decoding is limited to 1 MiB of UTF-8");
            var bytes=new java.io.ByteArrayOutputStream();for(long offset=0;offset<length;offset+=65536)bytes.write(read(address+16+offset,(int)Math.min(65536,length-offset)));
            text=new String(bytes.toByteArray(),StandardCharsets.UTF_8);
        }
        int count=text.codePointCount(0,text.length());
        if(index==null)return Json.object("value",Integer.toString(count),"type","int","variablesReference",0);
        if(index<0||index>=count)throw new IOException("String index is out of bounds");
        return Json.object("value",Integer.toString(text.codePointAt(text.offsetByCodePoints(0,index))),"type","char","variablesReference",0);
    }
    public Snapshot snapshot(Object variable)throws Exception{
        String type=Json.string(Json.get(variable,"type"));
        if(type.equals("string"))return new Snapshot(string(handle(variable)),List.of());
        if(type.endsWith("]")){
            long address=handle(variable);if(address==0)return new Snapshot("Length = 0",List.of());
            byte[] prefix=read(address,40);long count=word(prefix,16),capacity=word(prefix,24),element=word(prefix,0),data=word(prefix,8);
            if(count<0||capacity<count)throw new IOException("Invalid sequence length/capacity");
            long size=word(read(element+40,8),0);
            int bracket=type.lastIndexOf('[');return sequence(type.substring(0,bracket),data,count,size,false);
        }
        if(type.startsWith("System.Collections.Generic.List<"))return buffer(field(variable,"storage"),false);
        if(type.startsWith("System.Collections.Generic.Queue<")){
            Snapshot outgoing=buffer(field(variable,"outgoing"),true),incoming=buffer(field(variable,"incoming"),false);
            List<Object> children=new ArrayList<>();int i=0;
            for(Object item:outgoing.children()){var copy=Json.object("name","["+(i++)+"]","value",Json.get(item,"value"),"type",Json.get(item,"type"),"memoryReference",Json.get(item,"memoryReference"),"variablesReference",Json.get(item,"variablesReference"));children.add(copy);}
            for(Object item:incoming.children()){if(i==128)break;var copy=Json.object("name","["+(i++)+"]","value",Json.get(item,"value"),"type",Json.get(item,"type"),"memoryReference",Json.get(item,"memoryReference"),"variablesReference",Json.get(item,"variablesReference"));children.add(copy);}
            return new Snapshot("Queue: outgoing "+outgoing.summary()+", incoming "+incoming.summary(),children);
        }
        if(type.startsWith("System.Collections.Generic.Dictionary<")||type.startsWith("System.Collections.Generic.HashSet<"))return buffer(field(field(variable,"table"),"entries"),false);
        throw new IOException("Unsupported debug storage type: "+type);
    }
    private Snapshot buffer(Object variable,boolean reverse)throws Exception{
        String type=Json.string(Json.get(variable,"type"));int first=type.indexOf('<'),last=type.lastIndexOf('>');
        if(first<0||last<first)throw new IOException("Missing buffer element type");
        String element=type.substring(first+1,last);long count=number(field(variable,"count")),capacity=number(field(variable,"capacity"));
        if(count<0||capacity<count)throw new IOException("Invalid buffer length/capacity");
        long data=number(field(variable,"memory"));long size=size(element);
        // Aggregate size comes from the actual target debug type, with no target function call.
        if(size==0){Object value=request("evaluate",Json.object("expression","sizeof("+element+")","context","watch"));try{size=Long.parseLong(Json.string(Json.get(value,"result")));}catch(NumberFormatException e){throw new IOException("Cannot resolve element size");}}
        return sequence(element,data,count,size,reverse);
    }
    private static int size(String type){return switch(type){case "bool","byte","sbyte"->1;case "short","ushort"->2;case "int","uint","float","char"->4;case "long","ulong","double","string"->8;default->0;};}
    private Snapshot sequence(String type,long data,long count,long stride,boolean reverse)throws Exception{
        if(count<0||stride<1||stride>65536||count>Long.MAX_VALUE/stride||(count>0&&data==0))throw new IOException("Invalid debug element storage");
        List<Object> children=new ArrayList<>();int shown=(int)Math.min(count,128);
        for(int i=0;i<shown;i++){
            long index=reverse?count-1-i:i,address=data+index*stride;
            String value;
            if(size(type)>0){byte[] bytes=read(address,size(type));ByteBuffer b=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);value=switch(type){
                case "bool"->bytes[0]==0?"false":"true";case "byte"->Integer.toString(Byte.toUnsignedInt(bytes[0]));case "sbyte"->Byte.toString(bytes[0]);
                case "short"->Short.toString(b.getShort());case "ushort"->Integer.toString(Short.toUnsignedInt(b.getShort()));
                case "int"->Integer.toString(b.getInt());case "uint","char"->Long.toString(Integer.toUnsignedLong(b.getInt()));
                case "long"->Long.toString(b.getLong());case "ulong"->Long.toUnsignedString(b.getLong());case "float"->Float.toString(b.getFloat());case "double"->Double.toString(b.getDouble());
                case "string"->string(b.getLong());default->throw new IOException("Unknown primitive type");};
            }else value="<"+stride+" bytes; native aggregate>";
            children.add(Json.object("name","["+i+"]","value",value,"type",type,"memoryReference","0x"+Long.toUnsignedString(address,16),"variablesReference",0));
        }
        return new Snapshot("Length = "+count+(count>shown?" (first "+shown+" elements)":""),List.copyOf(children));
    }
}
