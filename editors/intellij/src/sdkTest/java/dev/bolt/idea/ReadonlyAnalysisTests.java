package dev.bolt.idea;

import dev.bolt.idea.core.BoltSession;
import dev.bolt.idea.core.BoltSymbols;
import dev.bolt.idea.core.Json;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

/** End-to-end Java client checks using the real compiler's embedded source snapshot. */
public final class ReadonlyAnalysisTests {
    private static void check(boolean condition,String message){if(!condition)throw new AssertionError(message);}
    public static void main(String[] args)throws Exception {
        Path root=Files.createTempDirectory("bolt-readonly-java-");
        try(var session=new BoltSession()) {
            Files.writeString(root.resolve("bolt.project"),"{}");Path file=root.resolve("Main.bolt");
            String text="import System\nstatic class Program{public:void Main(){Exception error=Exception(\"test\");Print(error.Message);}}";
            Files.writeString(file,text);var documents=new LinkedHashMap<Path,String>();documents.put(file,text);
            Path compiler=Path.of(args[0]);var origin=session.analyze(compiler,root,documents,file,0,()->{});
            var symbols=new BoltSymbols(origin);var hit=symbols.at(text.indexOf("Exception"));
            check(hit!=null,"original project resolves Exception");
            String uri=Json.string(Json.get(hit.symbol().definition(),"uri"));String source=symbols.virtualDocuments().get(uri);
            var analysis=session.analyzeVirtual(compiler,root,documents,file,0,uri,source,()->{});
            int declaration=source.indexOf("class Exception")+"class ".length();
            check(analysis.spans().stream().anyMatch(s->s.start()==declaration&&s.kind().equals("class")),"embedded class color uses exact source offset");
            for(String category:List.of("class","interface","method","field","property","parameter","decorator"))
                check(analysis.spans().stream().anyMatch(s->s.kind().equals(category)),"embedded source retains "+category+" colors");
            check(analysis.problems().isEmpty(),"readonly source does not acquire project diagnostics");
            var embedded=new BoltSymbols(analysis);var target=embedded.at(declaration);
            check(target!=null&&Json.string(Json.get(target.symbol().definition(),"uri")).equals(uri),"navigation in readonly source resolves its declaration");
            check(!embedded.virtualDocuments().isEmpty(),"readonly source retains further type-link targets");
            check(session.analyzeVirtual(compiler,root,documents,file,0,uri,source,()->{})==analysis,"unchanged embedded source reuses analysis");
            try {session.analyzeVirtual(compiler,root,documents,file,0,uri,source+" ",()->{});throw new AssertionError("expected stale-source rejection");}
            catch(IOException expected){check(expected.getMessage().contains("Embedded source changed"),"mismatching embedded source is rejected");}
            documents.put(file,text+"\n// unsaved change");
            check(session.analyzeVirtual(compiler,root,documents,file,0,uri,source,()->{})!=analysis,"origin edits invalidate embedded analysis");
            check(documents.size()==1&&!documents.containsValue(source),"embedded source is never sent as an editable overlay");
            Path concepts=root.resolve("Concepts.bolt");
            Files.writeString(concepts,"namespace Models;\nconcept Scalar<T> = Number<T>;\n");
            String constrained="import System.Linq\nimport System.Collections.Generic\nimport Models\nstatic class Program{public:T Pick<T>(T value):Scalar<T>{return value;}void Main(){List<int> values;Enumerable.Sort<List<int>,int>(values);Print(Pick<int>(1));}}";
            documents.put(file,constrained);
            var constrainedSymbols=new BoltSymbols(session.analyze(compiler,root,documents,file,1,()->{}));
            var sort=constrainedSymbols.at(constrained.indexOf("Sort<"));check(sort!=null,"Sort hover resolves");
            String sortLabel=sort.symbol().overloads().stream().filter(s->s.contains("Comparable")).findFirst().orElseThrow();
            var scheme=(com.intellij.openapi.editor.colors.EditorColorsScheme)java.lang.reflect.Proxy.newProxyInstance(
                ReadonlyAnalysisTests.class.getClassLoader(),new Class<?>[]{com.intellij.openapi.editor.colors.EditorColorsScheme.class},
                (p,m,a)->m.getName().equals("getAttributes")?new com.intellij.openapi.editor.markup.TextAttributes(a[0]==BoltColors.CONCEPT?java.awt.Color.GREEN:java.awt.Color.WHITE,null,null,null,0):null);
            for(String name:List.of("Iterable","Comparable")) {
                check(constrainedSymbols.typeKind(name).equals("concept"),"intrinsic concept hover kind: "+name);
                check(constrainedSymbols.typeDefinition(sort.symbol(),name)==null,"intrinsic concept has no invented source link");
            }
            String html=BoltDocumentationHtml.signature(sortLabel,sort.symbol(),constrainedSymbols::typeKind,scheme);
            for(String name:List.of("Iterable","Comparable"))check(html.contains("color:#00ff00;\">"+name+"</span>"),"Sort hover renders concept color: "+name);
            var pick=constrainedSymbols.at(constrained.indexOf("Pick<T>"));check(pick!=null,"user concept method hover resolves");
            check(constrainedSymbols.typeKind("Scalar").equals("concept"),"namespace concept short name is colored");
            check(constrainedSymbols.typeKind("Models.Scalar").equals("concept"),"namespace concept qualified name is colored");
            check(constrainedSymbols.typeDefinition(pick.symbol(),"Scalar")!=null,"source concept retains its definition link");
            System.out.println("Readonly analysis tests passed");
        }finally {
            try(var paths=Files.walk(root)){for(var path:paths.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(path);}
        }
    }
}
