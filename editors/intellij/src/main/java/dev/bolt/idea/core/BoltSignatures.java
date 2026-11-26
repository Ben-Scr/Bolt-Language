package dev.bolt.idea.core;
import java.util.*;

/** Protocol data only; no UI or PSI dependencies. */
public final class BoltSignatures {
    public record Parameter(int start,int end) { }
    public record Signature(String label,List<Parameter> parameters) { }
    public record Help(int opening,int argument,int selected,List<Signature> signatures) { }
    public static Help decode(Object result,String source) {
        int opening=new LspPositions(source).offset(Json.get(result,"boltCallStart"));if(opening<0)return null;
        List<Signature> signatures=new ArrayList<>();
        for(var raw:Json.list(Json.get(result,"signatures"))) {
            String label=Json.string(Json.get(raw,"label"));List<Parameter> parameters=new ArrayList<>();int previous=0;
            for(var p:Json.list(Json.get(raw,"parameters"))) {
                Object value=Json.get(p,"label");int start=-1,end=-1;
                if(value instanceof String text){start=label.indexOf(text,previous);end=start<0?-1:start+text.length();}
                else if(value instanceof List<?> pair&&pair.size()==2){start=(int)Json.integer(pair.get(0),-1);end=(int)Json.integer(pair.get(1),-1);}
                if(start<0||end<start||end>label.length()){start=-1;end=-1;}else previous=end;
                parameters.add(new Parameter(start,end));
            }
            signatures.add(new Signature(label,List.copyOf(parameters)));
        }
        if(signatures.isEmpty())return null;
        return new Help(opening,(int)Json.integer(Json.get(result,"activeParameter"),0),Math.max(0,Math.min(signatures.size()-1,(int)Json.integer(Json.get(result,"activeSignature"),0))),List.copyOf(signatures));
    }
    private BoltSignatures(){}
}
