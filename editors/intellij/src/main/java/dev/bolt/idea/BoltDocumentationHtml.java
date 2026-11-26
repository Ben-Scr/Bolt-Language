package dev.bolt.idea;

import com.intellij.openapi.editor.colors.EditorColorsScheme;
import com.intellij.openapi.editor.colors.TextAttributesKey;
import dev.bolt.idea.core.BoltSignatureHighlighting;
import dev.bolt.idea.core.BoltSymbols;
import dev.bolt.idea.core.Json;
import java.awt.Font;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.Locale;
import java.util.Set;
import java.util.function.Function;

/** Signature colors and source links are derived from compiler-provided semantic data. */
final class BoltDocumentationHtml {
    private static final Set<String> LINKABLE_TYPES=Set.of("class","struct","interface","enum","concept");

    static String signature(String label,BoltSymbols.Symbol symbol,Function<String,String> typeKind,EditorColorsScheme scheme) {
        return signature(label,symbol,typeKind,name->null,null,"",scheme);
    }

    static String signature(String label,BoltSymbols.Symbol symbol,Function<String,String> typeKind,
                            Function<String,Object> typeDefinition,Object ownerDefinition,String ownerKind,
                            EditorColorsScheme scheme) {
        String shown=displayLabel(label,symbol,ownerKind);
        String owner=BoltSymbols.shortTypeName(symbol.owner());
        Function<String,String> kinds=name->{
            if(!ownerKind.isEmpty()&&(name.equals(owner)||name.equals(symbol.owner())))return ownerKind;
            return typeKind==null?"":typeKind.apply(name);
        };
        StringBuilder html=new StringBuilder();int end=0;
        String category=symbol.category();
        if("method".equals(category)&&symbol.name().equals(owner)&&Set.of("class","struct").contains(ownerKind))category="constructor";
        for(var span:BoltSignatureHighlighting.spans(shown,symbol.name(),category,kinds)) {
            html.append(BoltSymbols.escape(shown.substring(end,span.start())));
            String word=shown.substring(span.start(),span.end());
            var key=key(span.kind(),word);var attributes=key==null?null:scheme.getAttributes(key);
            StringBuilder style=new StringBuilder();
            if(attributes!=null) {
                var color=attributes.getForegroundColor();
                if(color!=null)style.append("color:").append(String.format(Locale.ROOT,"#%06x",color.getRGB()&0xffffff)).append(';');
                if((attributes.getFontType()&Font.BOLD)!=0)style.append("font-weight:bold;");
                if((attributes.getFontType()&Font.ITALIC)!=0)style.append("font-style:italic;");
            }
            Object definition=null;
            if(linkable(span.kind(),symbol.category())) {
                if(LINKABLE_TYPES.contains(symbol.category())&&word.equals(symbol.name()))definition=symbol.definition();
                else if(ownerDefinition!=null&&!ownerKind.isEmpty()&&word.equals(owner))definition=ownerDefinition;
                else if(typeDefinition!=null) {
                    String qualified=qualifiedTypeAt(shown,span.start(),span.end());
                    definition=typeDefinition.apply(qualified);
                    if(definition==null&&!qualified.equals(word))definition=typeDefinition.apply(word);
                }
            }
            String href=navigationUrl(definition);
            // Keep the color on the anchor itself. Nested spans and an inline
            // text-decoration override interfere with IDEA's hovered-link attributes.
            if(href!=null) {
                html.append("<a href=\"").append(href).append('"');
                if(!style.isEmpty())html.append(" style=\"").append(style).append('"');
                html.append('>');
            } else if(!style.isEmpty())html.append("<span style=\"").append(style).append("\">");
            html.append(BoltSymbols.escape(word));
            if(href!=null)html.append("</a>");
            else if(!style.isEmpty())html.append("</span>");
            end=span.end();
        }
        return html.append(BoltSymbols.escape(shown.substring(end))).toString();
    }

    private static boolean linkable(String typeKind,String symbolCategory) {
        if(!LINKABLE_TYPES.contains(typeKind))return false;
        // For variables/fields/properties/parameters only object-value types should
        // become source links. Method signatures may link every resolved user type.
        if(Set.of("variable","field","property","parameter","local").contains(symbolCategory))
            return typeKind.equals("class")||typeKind.equals("struct");
        return true;
    }

    static String navigationUrl(Object definition) {
        String uri=Json.string(Json.get(definition,"uri"));
        Object range=Json.get(definition,"range");
        if(uri.isEmpty()||range==null)return null;
        String json=Json.write(Json.object("uri",uri,"range",range));
        return "bolt-nav://"+Base64.getUrlEncoder().withoutPadding().encodeToString(json.getBytes(StandardCharsets.UTF_8));
    }

    private static String displayLabel(String label,BoltSymbols.Symbol symbol,String ownerKind) {
        if(!"method".equals(symbol.category())||ownerKind.isEmpty()||symbol.owner().isEmpty())return label;
        String owner=BoltSymbols.shortTypeName(symbol.owner());
        if(owner.isEmpty()||owner.equals(symbol.name()))return label; // Constructors already carry their class name.
        for(int from=0;;) {
            int at=label.indexOf(symbol.name(),from);if(at<0)return label;
            int end=at+symbol.name().length();
            boolean left=at==0||!identifier(label.charAt(at-1));
            boolean right=end==label.length()||!identifier(label.charAt(end));
            if(left&&right) {
                int next=end;
                if(next<label.length()&&label.charAt(next)=='<') {
                    int depth=0;
                    while(next<label.length()) {
                        char c=label.charAt(next++);if(c=='<')depth++;else if(c=='>'&&--depth==0)break;
                    }
                }
                if(next<label.length()&&label.charAt(next)=='(') {
                    if(at>0&&label.charAt(at-1)=='.')return label;
                    return label.substring(0,at)+owner+"."+label.substring(at);
                }
            }
            from=end;
        }
    }

    private static String qualifiedTypeAt(String label,int start,int end) {
        int left=start;
        while(left>0&&label.charAt(left-1)=='.') {
            int p=left-1;if(p==0)break;p--;
            if(!identifier(label.charAt(p)))break;
            while(p>0&&identifier(label.charAt(p-1)))p--;
            left=p;
        }
        int right=end;
        while(right<label.length()&&label.charAt(right)=='.') {
            int p=right+1;if(p>=label.length()||!identifier(label.charAt(p)))break;
            while(p<label.length()&&identifier(label.charAt(p)))p++;
            right=p;
        }
        return label.substring(left,right);
    }

    private static boolean identifier(char c){return Character.isLetterOrDigit(c)||c=='_';}

    private static TextAttributesKey key(String kind,String word) {
        return switch(kind) {
            case "primitiveType" -> BoltColors.TYPE;
            case "control" -> BoltColors.CONTROL;
            case "braces" -> BoltColors.BRACES;
            case "parentheses" -> BoltColors.PARENTHESES;
            case "brackets" -> BoltColors.BRACKETS;
            case "blockComment" -> BoltColors.BLOCK_COMMENT;
            default -> BoltColors.semantic(kind,word);
        };
    }
    private BoltDocumentationHtml() { }
}
