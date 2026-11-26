package dev.bolt.idea.core;
import java.util.*;

/** Exact-version workspace edits. Validate every document before modifying any. */
public final class BoltEdits {
    public record Edit(int start,int end,String replacement) { }
    public record Change(String uri,String original,List<Edit> edits) {
        public String apply(String current){if(!current.equals(original))throw new IllegalArgumentException("File changed since rename analysis: "+uri);StringBuilder out=new StringBuilder(current);for(var edit:edits)out.replace(edit.start(),edit.end(),edit.replacement());return out.toString();}
    }
    public static List<Change> decode(Object response) {
        List<Change> changes=new ArrayList<>();Set<String> uris=new HashSet<>();
        for(var raw:Json.list(Json.get(response,"documentChanges"))) {
            String uri=Json.string(Json.get(Json.get(raw,"textDocument"),"uri"));Object text=Json.get(raw,"boltOriginalText");
            if(!uri.startsWith("file:")||!uris.add(uri)||!(text instanceof String original))throw new IllegalArgumentException("Invalid or read-only rename document");
            LspPositions positions=new LspPositions(original);List<Edit> edits=new ArrayList<>();
            for(var value:Json.list(Json.get(raw,"edits"))) {
                var range=Json.get(value,"range");int start=positions.offset(Json.get(range,"start")),end=positions.offset(Json.get(range,"end"));
                Object replacement=Json.get(value,"newText");if(start<0||end<start||!(replacement instanceof String r))throw new IllegalArgumentException("Invalid rename range");
                edits.add(new Edit(start,end,r));
            }
            edits.sort(Comparator.comparingInt(Edit::start).reversed());int before=original.length()+1;
            for(var edit:edits){if(edit.end()>before)throw new IllegalArgumentException("Overlapping rename edits");before=edit.start();}
            changes.add(new Change(uri,original,List.copyOf(edits)));
        }
        return List.copyOf(changes);
    }
    private BoltEdits(){}
}
