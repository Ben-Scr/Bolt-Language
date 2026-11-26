package dev.bolt.idea.core;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.TimeUnit;

/** No JUnit or IDEA dependency: runs the same transport, lexer and session as the plugin. */
public final class CoreTests {
    private static int count;
    private static void check(boolean condition, String name) {
        if (!condition) throw new AssertionError(name);
        count++; System.out.println("PASS " + name);
    }
    @FunctionalInterface interface Operation { void run() throws Exception; }
    private static void rejects(Operation action, String name) throws Exception {
        try { action.run(); } catch (IOException | IllegalArgumentException e) { check(true, name); return; }
        throw new AssertionError("Expected rejection: " + name);
    }
    public static void main(String[] args) throws Exception {
        Object value = Json.object("unicode", "Ä 🧪", "array", Arrays.asList(true, null, 12L, "a\nb\"c"), "empty", Json.object());
        check(Json.parse(Json.write(value)).equals(value), "JSON round trip, Unicode, escapes, nulls and numbers");
        rejects(() -> Json.parse("{\"a\":1,\"a\":2}"), "duplicate JSON keys rejected");
        rejects(() -> Json.parse("[1,]"), "trailing comma rejected");
        rejects(() -> Json.parse("01"), "leading-zero number rejected");
        rejects(() -> Json.parse("\"unclosed"), "unclosed JSON string rejected");
        rejects(() -> Json.parse("true false"), "trailing JSON content rejected");
        rejects(() -> Json.parse("[".repeat(140) + "0" + "]".repeat(140)), "JSON depth bounded");
        String text = "int a = 1;\r\n// 🧪 ü\nT value;";
        LspPositions pos = new LspPositions(text);
        check(pos.offset(1, 3) == text.indexOf("🧪"), "UTF-16 column starts at supplementary code point");
        check(pos.offset(1, 4) == -1, "UTF-16 ranges cannot split surrogate pairs");
        check(pos.offset(2, 2) == text.indexOf("value"), "CRLF and UTF-16 line offsets");
        check(pos.offset(80, 0) == -1 && pos.offset(1, 80) == -1, "out-of-range LSP positions rejected");
        byte[] body = Json.write(Json.object("jsonrpc", "2.0", "text", "ü 🧪")).getBytes(StandardCharsets.UTF_8);
        ByteArrayOutputStream frame = new ByteArrayOutputStream();
        frame.write(("Content-Length: " + body.length + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII)); frame.write(body);
        check("ü 🧪".equals(Json.get(RpcClient.readMessage(new ByteArrayInputStream(frame.toByteArray())), "text")), "LSP framing uses UTF-8 byte length");
        rejects(() -> RpcClient.readMessage(new ByteArrayInputStream("Content-Length: 9000000\r\n\r\n".getBytes())), "LSP oversized frame rejected");
        rejects(() -> RpcClient.readMessage(new ByteArrayInputStream("Content-Length: 4\r\n\r\n{}".getBytes())), "LSP truncated body rejected");
        rejects(() -> RpcClient.readMessage(new ByteArrayInputStream("Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}".getBytes())), "LSP duplicate length rejected");
        String code = "struct S { char c='c'; async Task<int> M<T>() { try {} catch (E& e) {} finally {} } } // comment\n/* multi\nline */";
        List<BoltScanner.Token> tokens = BoltScanner.scan(code);
        check(tokens.stream().filter(t -> t.kind() == BoltScanner.Kind.CONTROL).count() == 3, "try/catch/finally use control-flow colors");
        check(tokens.stream().anyMatch(t -> t.kind() == BoltScanner.Kind.KEYWORD && code.substring(t.start(),t.end()).equals("async")), "async is a modifier (C# keyword color)");
        check(tokens.stream().anyMatch(t -> t.kind() == BoltScanner.Kind.CHARACTER && code.substring(t.start(),t.end()).equals("'c'")), "single-quoted character colors");
        check(tokens.stream().anyMatch(t -> t.kind() == BoltScanner.Kind.BLOCK_COMMENT && code.substring(t.start(),t.end()).contains("\n")), "multiline comment is a single restartable token");
        check(BoltScanner.scan("int... values; a ?? b; a >>= 2;").stream().filter(t -> t.kind() == BoltScanner.Kind.OPERATOR).count() == 3, "variadic, coalescing and shift operators");
        String number = "1.ToString() 0x1f 2.5e-3f";
        check(BoltScanner.scan(number).stream().anyMatch(t -> t.kind()==BoltScanner.Kind.DOT), "number scanning does not consume member-access dot");
        check(BoltScanner.scan("$\"sum {Add(\"a\", \"b\")}\"").size()==1, "interpolated string handles nested expression strings");
        String[] incomplete = {"\"unterminated", "/* unterminated", "'", "$\"{"};
        for (String item: incomplete) {
            List<BoltScanner.Token> ts=BoltScanner.scan(item);
            check(!ts.isEmpty() && ts.get(ts.size()-1).end()==item.length(), "partial token always advances: " + item);
        }
        String directives = "#if Windows && Debug\nint? value;\n#else\n// #endif is a comment\n#endif\n";
        check(BoltScanner.scan(directives).stream().filter(t -> t.kind()==BoltScanner.Kind.PREPROCESSOR).count()==3, "conditional directives have a dedicated lexical layer");
        check(BoltScanner.scan("[Reflect] class Player {}").stream().noneMatch(t -> t.kind().toString().equals("ATTRIBUTE")), "attribute semantic color is not guessed by the lexer");
        check(BoltScanner.scan("int? value = null; value ?? fallback;").stream().anyMatch(t -> t.kind()==BoltScanner.Kind.OPERATOR), "optional punctuation preserves ordinary lexical scanning");
        Random random=new Random(712731);
        for(int attempt=0;attempt<1000;attempt++) {
            StringBuilder s=new StringBuilder(); for(int i=0;i<100;i++) s.append((char)random.nextInt(128));
            int last=0; for(var t:BoltScanner.scan(s)){if(t.start()!=last || t.end()<=last) throw new AssertionError("lexer stalled");last=t.end();}
            if(last!=s.length()) throw new AssertionError("lexer dropped text");
        }
        check(true,"1000 partial/random buffers preserve all token boundaries");
        rejects(() -> CompilerLocator.locate("./bolt"), "project-relative compiler path rejected");
        completionUnit();
        count += Features15Tests.run();
        count += FoldingTests.run();
        count += FormattingTests.run();
        count += SignatureHighlightingTests.run();
        if(args.length > 0) { projectIntegration(Path.of(args[0]).toRealPath()); integration(Path.of(args[0]).toRealPath()); completionIntegration(Path.of(args[0]).toRealPath()); }
        System.out.println(count + " checks passed");
    }
    private static void projectIntegration(Path bolt)throws Exception {
        Path root=Files.createTempDirectory("bolt-model-ü space-");
        try {
            Path source=root.resolve("src/Main.bolt");Files.createDirectories(source.getParent());
            Files.writeString(root.resolve("bolt.proj"),"format=1\n[project]\nname=\"Model\"\n");
            String code="public static class Program {public:void Main(){Print(42);}}";Files.writeString(source,code);
            BoltProjectInfo info=BoltProjectInfo.query(bolt,root);
            check(info.name().equals("Model")&&info.file().equals(root.resolve("bolt.proj")),"IDE reads actual shared CLI model");
            check(info.executable()&&info.defaultProfile().equals("Debug"),"IDE model has build profile and project kind");
            check(info.sources().equals(List.of(source))&&info.sourceRoots().equals(List.of(source.getParent())),"IDE source roots match compiler");
            check(info.entries().size()==1&&info.entries().get(0).byteOffset()==code.indexOf("Main"),"IDE gutter locations come from compiler");
            check(info.entries().get(0).sourceHash().equals(BoltProjectInfo.sourceHash(code)),"IDE can reject stale entrypoint mapping");
            String crlf="// 🧪\r\n"+code;
            var entry=new BoltProjectInfo.Entry(source,2,crlf.substring(0,crlf.indexOf("Main")).getBytes(StandardCharsets.UTF_8).length,BoltProjectInfo.sourceHash(crlf));
            check(BoltProjectInfo.editorOffset(entry,crlf)==crlf.replace("\r\n","\n").indexOf("Main"),"entry marker offsets handle CRLF and supplementary Unicode");
            check(BoltProjectInfo.editorOffset(entry,crlf+" ")==-1,"changed source invalidates gutter entry hash");
            Files.writeString(root.resolve("bolt.conf"),"format=2\n");
            rejects(()->BoltProjectInfo.query(bolt,root),"IDE rejects malformed compiler project model");
            rejects(()->BoltProjectInfo.parse("{\"schema_version\":2}"),"unsupported project output schema rejected");
        }finally{try(var files=Files.walk(root)){for(Path p:files.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(p);}}
    }
    private static void integration(Path bolt) throws Exception {
        Path root=Files.createTempDirectory("bolt-idea-ü space-");
        try {
            Files.writeString(root.resolve("bolt.project"),"{}");
            Path file=root.resolve("Program.bolt"), helper=root.resolve("Holder.bolt");
            String holder="public class Holder { public: int X; }\n";
            String code="""
                import Console
                public class Box<T> { public: T Value; }
                public static class Program {
                public:
                    T Add<T>(T a, T b) : Addable { return a + b; }
                    void Main() {
                        // Unicode 🧪 checks range transport.
                        Holder h;
                        h.X = 2;
                        int result = Add<int>(2,3);
                        Console.WriteLine(result);
                    }
                }
                """;
            Files.writeString(file,code);Files.writeString(helper,holder);
            check(CompilerLocator.locate(bolt.toString()).equals(bolt),"explicit compiler path resolves");
            long first;
            try(BoltSession s=new BoltSession()) {
                var docs=new LinkedHashMap<Path,String>();docs.put(file,code);docs.put(helper,holder);
                var a=s.analyze(bolt,root,docs,file,0,()->{});
                check(a.problems().isEmpty(),"real Bolt LSP: valid multi-file project has no errors: "+a.problems());
                var generics=a.spans().stream().filter(t->t.kind().equals("typeParameter")).toList();
                check(generics.size()>=6,"generic declaration and use-site semantic colors: "+generics);
                check(generics.stream().allMatch(t->code.substring(t.start(),t.end()).equals("T")),"generic spans identify exact T token");
                check(a.spans().stream().anyMatch(t->t.kind().equals("field") && code.substring(t.start(),t.end()).equals("X")),"field semantic classification");
                first=s.processId();
                check(s.analyze(bolt,root,docs,file,0,()->{})==a,"unchanged analysis reuses immutable cache");
                String bad=code.replace("int result = Add<int>(2,3);", "int result = \"wrong\";");docs.put(file,bad);
                var b=s.analyze(bolt,root,docs,file,0,()->{});
                check(!b.problems().isEmpty(),"unsaved type error reported");
                check(b.problems().stream().anyMatch(p->p.code().startsWith("BLT") && p.start()>=0 && p.end()<=bad.length()),"compiler code and source span preserved");
                check(Files.readString(file).equals(code),"unsaved checking does not write source to disk");
                docs.put(file,code);
                check(s.analyze(bolt,root,docs,file,0,()->{}).problems().isEmpty(),"fixing an unsaved error clears diagnostics");
                docs.put(helper,holder.replace("int X", "string X"));
                check(!s.analyze(bolt,root,docs,file,0,()->{}).problems().isEmpty(),"unsaved change in another file invalidates current-file analysis");
                docs.remove(helper);
                check(s.analyze(bolt,root,docs,file,0,()->{}).problems().isEmpty(),"closed document restores disk version");
                check(s.processId()==first,"all edits reuse a single native LSP process");
                try {s.analyze(bolt,root,docs,file,0,()->{throw new IllegalStateException("cancel");});throw new AssertionError("expected cancellation");}
                catch(IllegalStateException expected){check(expected.getMessage().equals("cancel"),"cancellation propagates without fake diagnostics");}
                check(s.analyze(bolt,root,docs,file,0,()->{}).problems().isEmpty(),"session works after cancellation");
                Files.writeString(helper,holder.replace("int X", "string X"));
                check(!s.analyze(bolt,root,docs,file,1,()->{}).problems().isEmpty(),"disk-change epoch reloads workspace dependencies");
                Files.writeString(helper,holder);
                check(s.analyze(bolt,root,docs,file,2,()->{}).problems().isEmpty(),"disk correction clears diagnostics");
                String nestedDeclarations="private class Hidden {} public class Outer {public: class Inner {public: int Value;} enum State {Idle} private: class Secret {}}";
                String nestedCode="static class Program {public: void Main(){Outer.Inner value; Outer.State state=Outer.State.Idle;}}";
                docs.put(helper,nestedDeclarations);docs.put(file,nestedCode);
                var nested=s.analyze(bolt,root,docs,file,2,()->{});
                check(nested.problems().isEmpty(),"IntelliJ transport accepts nested class and enum references");
                var nestedSymbols=new BoltSymbols(nested);
                var inner=nestedSymbols.at(nestedCode.indexOf("Inner"));
                check(inner!=null&&inner.symbol().name().equals("Inner")&&inner.symbol().owner().equals("Outer"),
                    "IntelliJ navigation snapshot retains the nested type owner");
                check(nested.spans().stream().anyMatch(span->span.start()==nestedCode.indexOf("Inner")&&span.kind().equals("class")),
                    "IntelliJ transport retains nested class semantic color");
                String memberCode="static class Program {public: void Main(){Outer.}}";docs.put(file,memberCode);
                var nestedItems=s.complete(bolt,root,docs,file,2,memberCode.indexOf("Outer.")+6,()->{});
                check(nestedItems.stream().anyMatch(item->item.name().equals("Inner"))&&nestedItems.stream().noneMatch(item->item.name().equals("Secret")),
                    "IntelliJ nested completion respects declaring-type privacy");
                String privateCode="static class Program {public: void Main(){Hidden value;}}";docs.put(file,privateCode);
                var inaccessible=s.analyze(bolt,root,docs,file,2,()->{});
                check(inaccessible.problems().stream().anyMatch(problem->problem.message().contains("private to its declaring file")),
                    "IntelliJ displays shared file-private diagnostic");
                check(inaccessible.spans().stream().anyMatch(span->span.start()==privateCode.indexOf("Hidden")&&span.kind().equals("class")),
                    "IntelliJ preserves known type color under privacy diagnostic");
                docs.put(helper,holder);docs.put(file,code);
                check(s.analyze(bolt,root,docs,file,2,()->{}).problems().isEmpty(),"nested-type edits restore clean multi-file analysis");
                ProcessHandle server = ProcessHandle.of(first).orElseThrow();
                var exited = server.onExit();
                check(server.destroyForcibly(), "test server received the termination request");
                exited.get(5, TimeUnit.SECONDS);
                check(s.analyze(bolt,root,docs,file,2,()->{}).problems().isEmpty()&&s.processId()!=first,"server crash restarts with documents resynchronized");
                first=s.processId();
                String conditional = "#if Release\nstatic class Program {public:void Main(){Print(42);}}\n#else\nstatic class Program {public:void Main(){UndefinedInDebug();}}\n#endif\n";
                docs.put(file, conditional);
                s.setProfile("Release");
                check(s.analyze(bolt,root,docs,file,2,()->{}).problems().isEmpty() && s.processId()!=first,
                    "analysis profile switches process and selects Release branch");
                s.setProfile("Debug");
                check(!s.analyze(bolt,root,docs,file,2,()->{}).problems().isEmpty(),
                    "switching to Debug diagnoses the actual active branch");
                s.setProfile("");
                check(!s.analyze(bolt,root,docs,file,2,()->{}).problems().isEmpty(),
                    "project-default analysis profile is restored");
                rejects(() -> s.setProfile("Fake"), "unsupported analysis profile rejected");
                first=s.processId();
            }
            Thread.sleep(100);
            check(ProcessHandle.of(first).map(p->!p.isAlive()).orElse(true),"project disposal terminates language server");
        } finally {try(var paths=Files.walk(root)){for(Path p:paths.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(p);}}
    }
    private static void completionUnit() {
        String[] noncode = {"// Console.", "/* Console.", "\"Console.", "'c.", "$\"literal .", "$\"{{literal.}}"};
        for (String text : noncode) check(!BoltCompletion.isCode(text,text.length()),"no popup in literal/comment: "+text);
        String[] expressions = {"Console.", "pointer->", "$\"value {Console.", "$\"value {\"abc\".", "\"abc\". ", "/*comment*/\nConsole.", "$\"{ $\"{Console.}"};
        for (String text : expressions) check(BoltCompletion.isCode(text,text.length()),"popup in expression: "+text);
        String closed = "$\"value {Console.} literal\"";
        check(BoltCompletion.isCode(closed,closed.indexOf("Console.")+8),"popup inside closed interpolation");
        check(!BoltCompletion.isCode(closed,closed.indexOf("literal")+3),"literal portion stays non-code");
        var read = new BoltCompletion.Item("ReadLine","string ReadLine()","string","method",2,List.of(),List.of(),true);
        var write = new BoltCompletion.Item("WriteLine","void WriteLine(T value)","void","method",2,List.of("T value"),List.of(),true);
        var generic = new BoltCompletion.Item("Next","T Next<T>(T max)","T","method",2,List.of("T max"),List.of("T"),true);
        check(BoltCompletion.insertion(read,"").equals(new BoltCompletion.Insertion("()",2,false)),"zero-argument method places caret after parentheses");
        check(BoltCompletion.insertion(write,"").equals(new BoltCompletion.Insertion("()",1,true)),"argument-taking method places caret inside parentheses");
        check(BoltCompletion.insertion(write," (123)").suffix().isEmpty(),"existing call parentheses not duplicated");
        check(BoltCompletion.insertion(generic,"").equals(new BoltCompletion.Insertion("<>()",1,false)),"generic call leaves explicit type placeholder");
        check(BoltCompletion.insertion(generic,"<int>(10)").suffix().isEmpty(),"existing generic arguments not duplicated");
        check(generic.tail().equals("<T>(T max)  static"),"lookup shows generic parameters, argument types/names and static marker");
        check(BoltCompletion.prefixStart("Console.Wri",11)==8,"only member prefix is replaced");
        check(BoltCompletion.prefixStart("w->Me",5)==3,"arrow prefix preserved");
        check(BoltCompletion.prefixStart("import System.I",15)==7,"replace whole qualified import prefix");
        check(BoltCompletion.importPrefixStart("  import ",9)==9,"empty import activates suggestions");
        check(BoltCompletion.importPrefixStart("import System.",14)==7,"dotted import recognized");
        check(BoltCompletion.importPrefixStart("// import System.",17)==-1,"comment is not an import");
        check(BoltCompletion.importPrefixStart("int x; import ",14)==-1,"statement tail is not an import");
        var module = new BoltCompletion.Item("System.IO","module System.IO","","namespace",9,List.of(),List.of(),true);
        check(BoltCompletion.insertion(module, "").suffix().isEmpty(),"import insertion adds no call parentheses");
        var reflect = new BoltCompletion.Item("Reflect","attribute Reflect","","attribute",7,List.of(),List.of(),false);
        var header = new BoltCompletion.Item("Header","attribute Header","","attribute",7,List.of("string path"),List.of(),false);
        check(BoltCompletion.insertion(reflect, "").equals(new BoltCompletion.Insertion("]",1,false)),"attribute completion closes an open bracket");
        check(BoltCompletion.insertion(reflect, "]").suffix().isEmpty(),"attribute completion keeps auto-inserted closing bracket");
        check(BoltCompletion.insertion(header, "]").equals(new BoltCompletion.Insertion("()",1,true)),"parameter attribute places caret inside parentheses");
        check(BoltCompletion.insertion(header, "").equals(new BoltCompletion.Insertion("()]",1,true)),"parameter attribute closes its list");
        check(BoltCompletion.insertion(header, "(\"a.h\")] ").suffix().isEmpty(),"existing attribute arguments are not duplicated");
        check(BoltCompletion.insertion(reflect, ", ReadOnly]").suffix().isEmpty(),"existing attribute list comma is retained");
        check(BoltCompletion.prefixStart("[Demo.He",8)==1,"qualified attribute prefix replaced as a whole");
        check(BoltCompletion.prefixStart("[Reflect, Re",12)==10,"second attribute prefix replaced without its predecessor");
        var raw=Json.object("items",List.of(Json.object("label","ReadLine","kind",2,"detail","string ReadLine()"),
            Json.object("label","WriteLine","kind",2,"detail","void WriteLine()"),
            Json.object("label","WriteLine","kind",2,"detail","void WriteLine(string text)")));
        check(BoltCompletion.items(raw).size()==3,"older-server overloads decoded without losing distinct signatures");
        var imports=BoltCompletion.items(List.of(
            Json.object("label","File","kind",9,"detail","Bolt standard module File"),
            Json.object("label","Ecs","kind",9,"detail","Bolt standard module Ecs"),
            Json.object("label","Demo.File","kind",9,"detail","Project namespace/module Demo.File"),
            Json.object("label","File","kind",7,"detail","class Custom.File","data",Json.object("bolt",Json.object("type","Custom.File","category","class")))));
        check(imports.get(0).kind()==7 && imports.get(0).type().equals("System.IO.File"),"legacy standard File import shows a class");
        check(imports.get(1).kind()==9 && imports.get(2).kind()==9,"standard and project namespaces keep module icons");
        check(imports.get(3).type().equals("Custom.File"),"explicit compiler classification is preserved");
    }

    private static void completionIntegration(Path bolt) throws Exception {
        Path root=Files.createTempDirectory("bolt-completion-ü-");
        String helper="""
            public class Widget {
            public:
                int Value;
                static int Count;
                Widget() { }
                int Read() { return Value; }
                int Sum(int x) { return Value + x; }
                int Sum(int x, int y) { return Value+x+y; }
                static int Twice(int n) { return n*2; }
            private:
                int Secret;
                void Hidden() { }
            }
            public class GenericBox<T> {
            public:
                T Value;
                void Set(T value) { Value = value; }
                T Get() { return Value; }
            }
            """;
        try (BoltSession session = new BoltSession()) {
            Files.writeString(root.resolve("bolt.project"),"{}");
            Path file=root.resolve("Program.bolt"), other=root.resolve("Types.bolt");
            Files.writeString(file, "static class Program { public: void Main() {} }");
            Files.writeString(other, helper);
            var docs=new LinkedHashMap<Path,String>();
            java.util.function.Function<String,List<BoltCompletion.Item>> complete=body -> {
                String source="import System.Collections.Generic\nimport Math\nimport Random\nstatic class Program { public: " +
                    "Widget Make() { Widget w; return w; } T Echo<T>(T value) { return value; } void Main(){\n"+body+"\n} }";
                int caret=source.indexOf('|'); source=source.replace("|","");
                docs.put(file,source);
                try { return session.complete(bolt,root,docs,file,0,caret,()->{}); }
                catch(IOException e) { throw new java.io.UncheckedIOException(e); }
            };
            var console=complete.apply("Console.|");
            check(console.stream().anyMatch(i->i.name().equals("ReadLine")&&i.type().equals("string")),"Console dot includes ReadLine and its return type");
            check(console.stream().filter(i->i.name().equals("WriteLine")).count()==2,"both WriteLine overloads arrive in the actual Java client");
            check(console.stream().allMatch(BoltCompletion.Item::isStatic),"Console entries are static");
            var members=complete.apply("Widget w; w.|");
            check(members.stream().anyMatch(i->i.name().equals("Value")&&i.type().equals("int")),"instance field includes declared type");
            check(members.stream().filter(i->i.name().equals("Sum")).count()==2,"instance overloads preserved");
            check(members.stream().noneMatch(i->Set.of("Count","Twice","Secret","Hidden","Widget").contains(i.name())),"static/private/constructor members excluded from instance completion");
            var stat=complete.apply("Widget.|");
            check(stat.stream().map(BoltCompletion.Item::name).collect(java.util.stream.Collectors.toSet()).equals(Set.of("Count","Twice")),"type dot returns only public static fields/methods");
            var generic=complete.apply("GenericBox<int> box; box.|");
            check(generic.stream().anyMatch(i->i.name().equals("Value")&&i.type().equals("int")),"generic fields substitute concrete type");
            check(generic.stream().anyMatch(i->i.name().equals("Set")&&i.parameters().equals(List.of("int value"))),"generic method parameters substitute concrete type");
            check(complete.apply("List<Widget> widgets; widgets[0].|").stream().anyMatch(i->i.name().equals("Read")),"collection index result can be completed");
            check(complete.apply("Make().|").stream().anyMatch(i->i.name().equals("Read")),"method return can be completed");
            check(complete.apply("Widget w; Echo<Widget>(w).|").stream().anyMatch(i->i.name().equals("Read")),"explicit generic return can be completed");
            check(complete.apply("unknown.|").isEmpty(),"unknown receiver does not invent members");
            check(complete.apply("Math.|").stream().anyMatch(i->i.name().equals("Sin")&&i.typeParameters().equals(List.of("T"))),"Math generic method metadata");
            check(complete.apply("Random.|").stream().filter(i->i.name().equals("Next")).count()==2,"Random range overloads preserved");
            docs.put(other,helper.replace("int Value;","int Renamed;"));
            var changed=complete.apply("Widget w; w.|");
            check(changed.stream().anyMatch(i->i.name().equals("Renamed"))&&changed.stream().noneMatch(i->i.name().equals("Value")),"unsaved edits in other files update member completion");
            docs.remove(other);
            check(complete.apply("Widget w; w.|").stream().anyMatch(i->i.name().equals("Value")),"closing unsaved helper restores original declarations");
            check(Files.readString(other).equals(helper),"completion never writes source files");
        } finally {try(var paths=Files.walk(root)){for(Path p:paths.sorted(Comparator.reverseOrder()).toList())Files.deleteIfExists(p);}}
    }
}
