package dev.bolt.idea.core;

import java.util.*;
import java.util.function.Function;

/** Immutable compiler-resolved snapshot. Never infers a declaration from spelling. */
public final class BoltSymbols {
    public record Symbol(String id, String name, String category, String label, String owner,
                         List<String> overloads, Object definition, boolean readOnly, boolean overloadSelected,
                         Long sizeBytes, String sizeNote) {
        public Symbol(String id,String name,String category,String label,String owner,List<String> overloads,Object definition,boolean readOnly) {
            this(id,name,category,label,owner,overloads,definition,readOnly,false);
        }
        public Symbol(String id,String name,String category,String label,String owner,List<String> overloads,Object definition,boolean readOnly,boolean overloadSelected) {
            this(id,name,category,label,owner,overloads,definition,readOnly,overloadSelected,null,"");
        }
    }
    public record Hit(int start, int end, Symbol symbol) { }
    private final List<Hit> hits;
    private final Map<String,String> virtualDocuments;
    private final Map<String,String> typeKinds;
    private final Map<String,List<Object>> alternatives=new HashMap<>();
    private final Map<String,List<Object>> overloadDefinitions=new HashMap<>();
    public record TypeDefinition(String name,String qualified,String category,Object definition) { }
    private final Map<String,List<TypeDefinition>> typeDefinitions=new HashMap<>();
    private final Map<String,Object> ownerDefinitions=new HashMap<>();
    private final Map<String,String> ownerKinds=new HashMap<>();
    public BoltSymbols(BoltSession.Analysis analysis) {
        var root=analysis.symbols();var positions=new LspPositions(analysis.text());
        Map<String,Symbol> symbols=new HashMap<>();
        for(var entry:Json.map(Json.get(root,"symbols")).entrySet()) {
            var value=entry.getValue();if(!Json.list(Json.get(value,"definitions")).isEmpty())alternatives.put(Json.string(entry.getKey()),List.copyOf(Json.list(Json.get(value,"definitions"))));var hover=Json.get(value,"hover");var details=Json.get(hover,"bolt");
            String id=Json.string(entry.getKey());
            var definitions=Json.list(Json.get(details,"overloadDefinitions"));
            if(!definitions.isEmpty())overloadDefinitions.put(id,List.copyOf(definitions));
            List<TypeDefinition> linkedTypes=new ArrayList<>();
            for(var item:Json.list(Json.get(details,"typeDefinitions"))) {
                String name=Json.string(Json.get(item,"name")),qualified=Json.string(Json.get(item,"qualified")),category=Json.string(Json.get(item,"category"));
                Object definition=Json.get(item,"definition");
                if(!name.isEmpty()&&!category.isEmpty())linkedTypes.add(new TypeDefinition(name,qualified,category,definition));
            }
            if(!linkedTypes.isEmpty())typeDefinitions.put(id,List.copyOf(linkedTypes));
            Object ownerDefinition=Json.get(details,"ownerDefinition");
            String ownerKind=Json.string(Json.get(details,"ownerCategory"));
            if(ownerDefinition!=null&&!ownerKind.isEmpty()){ownerDefinitions.put(id,ownerDefinition);ownerKinds.put(id,ownerKind);}
            List<String> overloads=new ArrayList<>();for(var item:Json.list(Json.get(details,"overloads")))if(item instanceof String text)overloads.add(text);
            symbols.put(id,new Symbol(id,Json.string(Json.get(value,"name")),Json.string(Json.get(value,"category")),
                Json.string(Json.get(details,"label")),Json.string(Json.get(details,"owner")),List.copyOf(overloads),Json.get(value,"definition"),Boolean.TRUE.equals(Json.get(value,"readOnly")),false,
                sizeBytes(Json.get(details,"sizeBytes")),Json.string(Json.get(details,"sizeNote"))));
        }
        Map<String,String> types=new HashMap<>();
        for(var symbol:symbols.values())if(Set.of("class","struct","interface","enum","concept","typeParameter").contains(symbol.category())) {
            String qualified=symbol.id().substring(symbol.id().indexOf(':')+1);
            addTypeKind(types,qualified,symbol.category());
            addTypeKind(types,withoutTypeArguments(qualified),symbol.category());
            addTypeKind(types,symbol.name(),symbol.category());
        }
        for(var definitions:typeDefinitions.values())for(var definition:definitions) {
            addTypeKind(types,definition.name(),definition.category());
            if(!definition.qualified().isEmpty()) {
                addTypeKind(types,definition.qualified(),definition.category());
                addTypeKind(types,withoutTypeArguments(definition.qualified()),definition.category());
            }
        }
        for(var entry:ownerKinds.entrySet()) {
            var symbol=symbols.get(entry.getKey());if(symbol==null||symbol.owner().isEmpty())continue;
            addTypeKind(types,symbol.owner(),entry.getValue());
            String shortName=shortTypeName(symbol.owner());if(!shortName.isEmpty())addTypeKind(types,shortName,entry.getValue());
        }
        typeKinds=Map.copyOf(types);
        List<Hit> values=new ArrayList<>();
        for(var entry:Json.list(Json.get(root,"occurrences"))) {
            var range=Json.get(entry,"range");int start=positions.offset(Json.get(range,"start")),end=positions.offset(Json.get(range,"end"));
            var symbol=symbols.get(Json.string(Json.get(entry,"symbol")));
            if(symbol!=null&&start>=0&&end>start)values.add(new Hit(start,end,symbol));
        }
        values.sort(Comparator.comparingInt(Hit::start).thenComparingInt(Hit::end));hits=List.copyOf(values);
        Map<String,String> virtuals=new HashMap<>();for(var item:Json.map(Json.get(root,"virtualDocuments")).entrySet())if(item.getValue() instanceof String text)virtuals.put(Json.string(item.getKey()),text);
        virtualDocuments=Map.copyOf(virtuals);
    }
    private static Long sizeBytes(Object value) {
        if(!(value instanceof Byte||value instanceof Short||value instanceof Integer||value instanceof Long))return null;
        long bytes=((Number)value).longValue();return bytes>=0?bytes:null;
    }
    private static void addTypeKind(Map<String,String> types,String name,String kind) {
        if(!name.isEmpty())types.merge(name,kind,(a,b)->a.equals(b)?a:"");
    }
    private static String withoutTypeArguments(String name) {
        StringBuilder out=new StringBuilder();int depth=0;
        for(int i=0;i<name.length();i++) {
            char c=name.charAt(i);if(c=='<')depth++;else if(c=='>')depth=Math.max(0,depth-1);else if(depth==0)out.append(c);
        }
        return out.toString();
    }
    public static String shortTypeName(String name) {
        String value=withoutTypeArguments(name);int dot=value.lastIndexOf('.');return dot<0?value:value.substring(dot+1);
    }
    /** A spelling alone must not choose between a class and a struct with the same short name. */
    public String typeKind(String name){return typeKinds.getOrDefault(name,"");}
    public Object ownerDefinition(Symbol symbol){return ownerDefinitions.get(symbol.id());}
    public String ownerKind(Symbol symbol){return ownerKinds.getOrDefault(symbol.id(),"");}
    public Object typeDefinition(Symbol symbol,String name) {
        var definitions=typeDefinitions.getOrDefault(symbol.id(),List.of());
        TypeDefinition exact=null;for(var definition:definitions)
            if(definition.qualified().equals(name)||withoutTypeArguments(definition.qualified()).equals(name)) {
                if(exact!=null&&!Objects.equals(exact.definition(),definition.definition()))return null;exact=definition;
            }
        if(exact!=null)return exact.definition();
        TypeDefinition shortMatch=null;String shortName=shortTypeName(name);
        for(var definition:definitions)if(definition.name().equals(shortName)) {
            if(shortMatch!=null&&!Objects.equals(shortMatch.definition(),definition.definition()))return null;shortMatch=definition;
        }
        return shortMatch==null?null:shortMatch.definition();
    }
    public Hit at(int offset) {
        int low=0,high=hits.size();while(low<high){int middle=(low+high)>>>1;if(hits.get(middle).start()<=offset)low=middle+1;else high=middle;}
        for(int i=low-1;i>=0&&hits.get(i).start()<=offset;i--){var h=hits.get(i);if(offset<h.end())return h;if(i<low-3)break;}return null;
    }
    public List<Object> definitions(Symbol symbol){
        var selected=symbol.definition()==null?List.of():List.of(symbol.definition());
        return symbol.overloadSelected()?selected:alternatives.getOrDefault(symbol.id(),selected);
    }
    public Object definition(Symbol symbol,int index) {
        int count=Math.max(1,symbol.overloads().size()),selected=Math.floorMod(index,count);
        var definitions=overloadDefinitions.get(symbol.id());
        if(definitions!=null&&definitions.size()==count)return definitions.get(selected);
        // Older servers cannot identify another overload's source. Never reuse
        // the selected call's location while showing a different signature.
        int original=symbol.overloads().isEmpty()?0:symbol.overloads().indexOf(symbol.label());
        return selected==original?symbol.definition():null;
    }
    public Symbol selectOverload(Symbol symbol,int index) {
        Object location=definition(symbol,index);if(location==null)return null;
        String label=symbol.overloads().isEmpty()?symbol.label():symbol.overloads().get(Math.floorMod(index,symbol.overloads().size()));
        return new Symbol(symbol.id(),symbol.name(),symbol.category(),label,symbol.owner(),symbol.overloads(),location,
            Json.string(Json.get(location,"uri")).startsWith("bolt-stdlib:///"),true,symbol.sizeBytes(),symbol.sizeNote());
    }
    public Map<String,String> virtualDocuments(){return virtualDocuments;}
    public List<Hit> hits(){return hits;}
    public static String escape(String text){return text.replace("&","&amp;").replace("<","&lt;").replace(">","&gt;").replace("\"","&quot;").replace("'","&#39;");}
    public static String documentation(Symbol symbol,int index) {
        int count=Math.max(1,symbol.overloads().size()),original=symbol.overloads().isEmpty()?0:symbol.overloads().indexOf(symbol.label());
        return documentation(symbol,index,Math.floorMod(index,count)==original?symbol.definition():null);
    }
    public static String documentation(Symbol symbol,int index,Object definition) {
        return documentation(symbol,index,definition,BoltSymbols::escape);
    }
    /** The IDE supplies theme-aware signature HTML; all other content remains escaped here. */
    public static String documentation(Symbol symbol,int index,Object definition,Function<String,String> signatureHtml) {
        var overloads=symbol.overloads().isEmpty()?List.of(symbol.label()):symbol.overloads();int selected=Math.floorMod(index,overloads.size());
        StringBuilder out=new StringBuilder("<html><body><div class='definition'><pre>").append(signatureHtml.apply(overloads.get(selected))).append("</pre></div>");
        if(overloads.size()>1)out.append("<div><a href='psi_element://bolt-overload/").append((selected+overloads.size()-1)%overloads.size()).append("'>&#9664;</a> ").append(selected+1).append(" / ").append(overloads.size()).append(" <a href='psi_element://bolt-overload/").append((selected+1)%overloads.size()).append("'>&#9654;</a></div>");
        if(symbol.sizeBytes()!=null||!symbol.sizeNote().isEmpty()) {
            out.append("<div class='content'>Size: ");
            if(symbol.sizeBytes()!=null)out.append(symbol.sizeBytes()).append(symbol.sizeBytes()==1?" byte":" bytes");
            if(!symbol.sizeNote().isEmpty()) {
                if(symbol.sizeBytes()!=null)out.append(" (");
                out.append(escape(symbol.sizeNote()));
                if(symbol.sizeBytes()!=null)out.append(")");
            }
            out.append("</div>");
        }
        return out.append("</body></html>").toString();
    }
}
