package dev.bolt.idea;

import com.intellij.lang.parameterInfo.ParameterInfoUIContext;
import com.intellij.lang.parameterInfo.UpdateParameterInfoContext;
import com.intellij.openapi.editor.colors.EditorColorsScheme;
import com.intellij.openapi.editor.markup.TextAttributes;
import com.intellij.psi.PsiElement;
import com.intellij.psi.PsiManager;
import com.intellij.psi.SingleRootFileViewProvider;
import com.intellij.testFramework.LightVirtualFile;
import com.intellij.util.ui.JBHtmlEditorKit;
import dev.bolt.idea.core.BoltSignatures;
import dev.bolt.idea.core.BoltSymbols;
import dev.bolt.idea.core.Json;
import java.awt.Color;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.function.BiFunction;
import javax.swing.JEditorPane;
import javax.swing.SwingUtilities;
import javax.swing.event.HyperlinkEvent;
import javax.swing.text.html.CSS;
import javax.swing.text.html.HTML;
import javax.swing.text.html.HTMLDocument;

/** Run against the installed IDEA SDK; exercises its real hovered-link renderer. */
public final class DocumentationUiTests {
    private static void check(boolean value,String message) { if(!value)throw new AssertionError(message); }
    @SuppressWarnings("unchecked")
    private static <T> T proxy(Class<T> type,BiFunction<String,Object[],Object> call) {
        return (T)Proxy.newProxyInstance(type.getClassLoader(),new Class<?>[]{type},(p,m,a)->call.apply(m.getName(),a));
    }
    private static void links() throws Exception {
        Object location=Json.object("uri","bolt-stdlib:///Vec2.bolt","range",Json.object(
            "start",Json.object("line",3,"character",4),"end",Json.object("line",3,"character",8)));
        var symbol=new BoltSymbols.Symbol("method:Vec2.Add","Add","method","Vec2 Add(this Vec2 a, Vec2 b)","",List.of(),location,true);
        var scheme=proxy(EditorColorsScheme.class,(name,args)->name.equals("getAttributes")?new TextAttributes(Color.CYAN,null,null,null,0):null);
        String html=BoltDocumentationHtml.signature(symbol.label(),symbol,name->name.equals("Vec2")?"struct":"",
            name->name.equals("Vec2")?location:null,null,"",scheme);
        String href=BoltDocumentationHtml.navigationUrl(location);
        check(html.split("<a ",-1).length-1==3,"return type and both parameter types are links");
        check(html.contains("style=\"color:#00ffff;\""),"links retain the signature's theme color");
        String decoded=new String(Base64.getUrlDecoder().decode(href.substring("bolt-nav://".length())),StandardCharsets.UTF_8);
        check(Json.write(Json.parse(decoded)).equals(Json.write(location)),"link retains the exact source range and embedded URI");
        check(BoltDocumentationHtml.navigationUrl(Json.object("uri","file:///missing.bolt"))==null,"no link without a source range");

        var pane=new JEditorPane();pane.setEditorKit(new JBHtmlEditorKit());pane.setText("<html><body><pre>"+html+"</pre></body></html>");
        var document=(HTMLDocument)pane.getDocument();var iterator=document.getIterator(HTML.Tag.A);
        check(iterator.isValid(),"IDEA parses the semantic link");
        int start=iterator.getStartOffset();var element=document.getCharacterElement(start);
        pane.fireHyperlinkUpdate(new HyperlinkEvent(pane,HyperlinkEvent.EventType.ENTERED,null,href,element));
        check("underline".equals(String.valueOf(document.getCharacterElement(start).getAttributes().getAttribute(CSS.Attribute.TEXT_DECORATION))),"IDEA underlines the type on mouse entry");
        pane.fireHyperlinkUpdate(new HyperlinkEvent(pane,HyperlinkEvent.EventType.EXITED,null,href,document.getCharacterElement(start)));
        check("none".equals(String.valueOf(document.getCharacterElement(start).getAttributes().getAttribute(CSS.Attribute.TEXT_DECORATION))),"IDEA removes the underline on mouse exit");
    }
    private static void overloads() {
        for(String name:List.of("Vec2","Add")) {
            var signatures=List.of(new BoltSignatures.Signature(name+"(float x, float y)",List.of()),
                new BoltSignatures.Signature(name+"(float scalar)",List.of()),
                new BoltSignatures.Signature(name+"(Aes aes)",List.of()));
            var help=new BoltSignatures.Help(0,0,1,signatures);var data=new HashMap<Object,Object>();
            var owner=proxy(PsiElement.class,(method,args)->switch(method) {
                case "getUserData" -> args[0].toString().equals("bolt.signature.help")?help:data.get(args[0]);
                case "putUserData" -> {data.put(args[0],args[1]);yield null;}
                default -> null;
            });
            Object[] highlighted={null};
            long[] stamp={1};
            var document=proxy(com.intellij.openapi.editor.Document.class,(method,args)->method.equals("getModificationStamp")?stamp[0]:null);
            var editor=proxy(com.intellij.openapi.editor.Editor.class,(method,args)->method.equals("getDocument")?document:null);
            var update=proxy(UpdateParameterInfoContext.class,(method,args)->switch(method) {
                case "getEditor" -> editor;
                case "getObjectsToView" -> signatures.toArray();
                case "getHighlightedParameter" -> highlighted[0];
                case "setHighlightedParameter" -> {highlighted[0]=args[0];yield null;}
                default -> null;
            });
            var handler=new BoltParameterInfo();handler.updateParameterInfo(owner,update);
            check(highlighted[0]==signatures.get(1),"compiler-selected overload starts highlighted");
            for(int selected:List.of(1,2,0)) {
                highlighted[0]=signatures.get(selected);handler.updateParameterInfo(owner,update);
                for(int i=0;i<signatures.size();i++) {
                    boolean[] visible={true};
                    var ui=proxy(ParameterInfoUIContext.class,(method,args)->switch(method) {
                        case "getParameterOwner" -> owner;
                        case "getCurrentParameterIndex" -> 0;
                        case "getDefaultParameterColor" -> Color.WHITE;
                        case "setUIComponentVisible" -> {visible[0]=(boolean)args[0];yield null;}
                        default -> null;
                    });
                    handler.updateUI(signatures.get(i),ui);
                    check(visible[0]==(i==selected),"only selected overload is shown after keyboard switching: "+name);
                }
            }
            highlighted[0]=signatures.get(0);stamp[0]++;handler.updateParameterInfo(owner,update);
            check(highlighted[0]==signatures.get(1),"typing resumes compiler-selected overload after manual selection");
            check(handler.supportsOverloadSwitching(),"all overloads remain keyboard-accessible");
        }
    }
    private static void constructorColors() {
        var scheme=proxy(EditorColorsScheme.class,(name,args)-> {
            if(!name.equals("getAttributes"))return null;
            Color color=args[0]==BoltColors.STRUCT?Color.GREEN:args[0]==BoltColors.CLASS?Color.CYAN:Color.YELLOW;
            return new TextAttributes(color,null,null,null,0);
        });
        for(String kind:List.of("struct","class")) {
            var symbol=new BoltSymbols.Symbol("ctor","Vec2","method","Vec2(float x)","Geometry.Vec2",List.of(),null,false);
            String html=BoltDocumentationHtml.signature(symbol.label(),symbol,name->"",name->null,null,kind,scheme);
            check(html.startsWith("<span style=\"color:"+(kind.equals("struct")?"#00ff00":"#00ffff")+";\">Vec2</span>"),"constructor hover uses its resolved "+kind+" color");
        }
    }
    private static void parameterRendering() {
        var descriptor=new BoltSignatures.Signature("T Pick<T>(T value, int count)",List.of());
        String label="string Pick<T>(string value, int count)";int start=label.indexOf("int count");
        var display=new BoltSignatures.Signature(label,List.of(new BoltSignatures.Parameter(15,27),new BoltSignatures.Parameter(start,start+9)));
        var owner=proxy(PsiElement.class,(name,args)->name.equals("getUserData")?switch(args[0].toString()) {
            case "bolt.signature.visible" -> descriptor;
            case "bolt.signature.display" -> display;
            default -> null;
        }:null);
        Object[][] rendered={null};
        var ui=proxy(ParameterInfoUIContext.class,(name,args)->switch(name) {
            case "getParameterOwner" -> owner;
            case "getCurrentParameterIndex" -> 1;
            case "getDefaultParameterColor" -> Color.WHITE;
            case "setupUIComponentPresentation" -> {rendered[0]=args;yield null;}
            default -> null;
        });
        var handler=new BoltParameterInfo();handler.updateUI(descriptor,ui);
        check(label.equals(rendered[0][0]),"popup renders the latest inferred signature using the original descriptor");
        check(label.substring((int)rendered[0][1],(int)rendered[0][2]).equals("int count"),"popup highlights exactly the current parameter");
        boolean[] closed={false};
        var update=proxy(UpdateParameterInfoContext.class,(name,args)->{if(name.equals("removeHint"))closed[0]=true;return null;});
        handler.processFoundElementForUpdatingParameterInfo(null,update);
        check(closed[0],"leaving a call closes its parameter popup");
    }
    private static void readonlyHighlightingEligibility() throws Exception {
        var file=new LightVirtualFile("File.bolt",BoltFileType.INSTANCE,"public class File {}");
        file.setWritable(false);
        file.putUserData(BoltNavigation.VIRTUAL_SOURCE,new BoltNavigation.VirtualSource("bolt-stdlib:///System/IO/File.bolt",java.nio.file.Path.of("Main.bolt")));
        // Constructor-only test: the explicit language avoids any manager query.
        // Allocate a manager shell so the platform's nonnull/type checks hold,
        // without starting an IDE application or bypassing provider construction.
        var unsafeClass=Class.forName("sun.misc.Unsafe");var field=unsafeClass.getDeclaredField("theUnsafe");field.setAccessible(true);
        var manager=(PsiManager)unsafeClass.getMethod("allocateInstance",Class.class).invoke(field.get(null),Class.forName("com.intellij.psi.impl.PsiManagerImpl"));
        var project=proxy(com.intellij.openapi.project.Project.class,(name,args)->null);
        for(var member:manager.getClass().getDeclaredFields())if(member.getType()==com.intellij.openapi.project.Project.class) {
            member.setAccessible(true);member.set(manager,project);
        }
        var baseline=new SingleRootFileViewProvider(manager,file,true,BoltLanguage.INSTANCE) {};
        check(!baseline.isPhysical(),"standard LightVirtualFile provider is rejected by the daemon");
        var factory=new BoltFileViewProviderFactory();
        var opened=factory.createFileViewProvider(file,BoltLanguage.INSTANCE,manager,true);
        check(opened.isPhysical(),"opened embedded source passes the daemon's physical-PSI gate");
        check(opened.isEventSystemEnabled(),"opened embedded source retains document events");
        check(!file.isWritable(),"enabling highlighting preserves readonly source");
        check(!factory.createFileViewProvider(file,BoltLanguage.INSTANCE,manager,false).isPhysical(),"detached PSI copies stay nonphysical");
        var scratch=new LightVirtualFile("scratch.bolt",BoltFileType.INSTANCE,"class Scratch {}");
        check(!factory.createFileViewProvider(scratch,BoltLanguage.INSTANCE,manager,true).isPhysical(),"unrelated scratch files keep normal eligibility");
        var daemonClass=Class.forName("com.intellij.codeInsight.daemon.impl.DaemonCodeAnalyzerImpl");
        var daemon=unsafeClass.getMethod("allocateInstance",Class.class).invoke(field.get(null),daemonClass);
        for(Class<?> type=daemonClass;type!=null;type=type.getSuperclass())for(var member:type.getDeclaredFields()) {
            if(java.lang.reflect.Modifier.isStatic(member.getModifiers()))continue;
            if(member.getType()==com.intellij.openapi.project.Project.class){member.setAccessible(true);member.set(daemon,project);}
            if(member.getName().equals("myDisabledHighlightingFiles")){member.setAccessible(true);member.set(daemon,new HashSet<>());}
        }
        com.intellij.psi.PsiFile[] psi={null};
        for(var provider:List.of(baseline,opened)) {
            psi[0]=proxy(com.intellij.psi.PsiFile.class,(name,args)->switch(name) {
                case "isPhysical" -> provider.isPhysical();
                case "getProject" -> project;
                case "getOriginalFile" -> psi[0];
                case "getVirtualFile" -> file;
                case "getFileType" -> BoltFileType.INSTANCE;
                default -> null;
            });
            boolean available=(boolean)daemonClass.getMethod("isHighlightingAvailable",com.intellij.psi.PsiFile.class).invoke(daemon,psi[0]);
            check(available==(provider==opened),"actual IntelliJ daemon allows only the corrected embedded provider");
        }
    }
    public static void main(String[] args) throws Exception {
        SwingUtilities.invokeAndWait(()->{try {links();overloads();parameterRendering();constructorColors();readonlyHighlightingEligibility();}catch(Exception e){throw new RuntimeException(e);}});
        System.out.println("Documentation UI tests passed");
    }
}
