package dev.bolt.idea.core;
import java.util.*;
import java.io.IOException;
/** Real protocol decoding/edit planning; IntelliJ SDK/UI is explicitly not mocked. */
public final class Features15Tests {
 private static int count;
 private static void check(boolean value,String name){if(!value)throw new AssertionError(name);count++;System.out.println("PASS "+name);}
 private static Object range(int a,int b){return Json.object("start",Json.object("line",0,"character",a),"end",Json.object("line",0,"character",b));}
 private static void reject(Runnable r,String name){boolean failed=false;try{r.run();}catch(IllegalArgumentException e){failed=true;}check(failed,name);}
 private static BoltSymbols.Symbol hoverSymbol(Object... sizeDetails){
  var details=Json.object("label","Vec2 position","owner","Example");details.putAll(Json.object(sizeDetails));
  var snapshot=Json.object("symbols",Json.object("s",Json.object("name","position","category","local","definition",Json.object("uri","file:///Main.bolt","range",range(0,8)),"hover",Json.object("bolt",details))),"occurrences",List.of(Json.object("range",range(0,8),"symbol","s")));
  return new BoltSymbols(new BoltSession.Analysis("position",List.of(),List.of(),Json.parse(Json.write(snapshot)))).at(0).symbol();
 }
 public static int run(){
  var e=BoltEditing.blockEnter("    if (a) {  }",13,"    ");
  check(e!=null&&e.text().equals("\n        \n    "),"Enter between braces uses enclosing indentation");
  e=BoltEditing.blockEnter("\t{",2,"\t");check(e!=null&&e.text().equals("\n\t\t"),"Enter obeys tab indentation");
  check(BoltEditing.blockEnter("\"{\"",2,"  ")==null,"no brace indentation inside string");
  check(BoltEditing.blockEnter("// {",4,"  ")==null,"no brace indentation inside comment");
  check(BoltEditing.genericInsertionSafe("List<",5),"generic insertion accepts end of type prefix");
  check(!BoltEditing.genericInsertionSafe("List<=",5),"generic insertion never changes <=");
  check(!BoltEditing.genericInsertionSafe("i<<",2),"generic insertion never changes shift");
  check(!BoltEditing.genericInsertionSafe("\"List<",6),"generic insertion rejects strings");
  check(!BoltEditing.genericInsertionSafe("// List<",8),"generic insertion rejects comments");
  check(!BoltEditing.genericInsertionSafe("List<int",5),"generic insertion does not modify existing type arguments");
  for(String keyword:List.of("event","implicit","explicit"))check(BoltScanner.scan(keyword).get(0).kind()==BoltScanner.Kind.KEYWORD,"new lexical keyword "+keyword);
  var record=Json.object("severity","info","message","[bolt] Debug compilation: 3.2 ms");
  List<BoltConsole.Output> output=new ArrayList<>();var console=new BoltConsole();String data=BoltConsole.PREFIX+Json.write(record)+"\n";
  for(int i=0;i<data.length();i++)console.accept(data.substring(i,i+1),true,output::add);
  check(output.size()==1&&output.get(0).severity().equals("info")&&!output.get(0).text().contains("BOLT1"),"chunked stderr timing record is normal output");
  console.accept(BoltConsole.PREFIX+Json.write(Json.object("severity","warning","message","C++ native backend selected"))+"\n",true,output::add);
  check(output.getLast().severity().equals("warning"),"C++ fallback is a warning, not red");
  console.accept("file.cpp(1): warning C4996: old\n",false,output::add);check(output.getLast().severity().equals("warning"),"external compiler warning on stdout recognized");
  console.accept("  1 | deprecated();\n",true,output::add);check(output.getLast().severity().equals("warning"),"warning source continuation preserves severity");
  console.accept("file.cpp:1: error: failed\n",false,output::add);check(output.getLast().severity().equals("error"),"compiler error on stdout recognized");
  console.accept("Built Debug: program\n",false,output::add);check(output.getLast().severity().equals("info"),"Built status is informational");
  console.accept(BoltConsole.PREFIX+"bad record\n",true,output::add);check(output.getLast().text().contains("bad record"),"malformed status record remains visible");
  int before=output.size();console.accept(BoltConsole.PREFIX+"{\"phase\":\"run\"}\n",true,output::add);check(output.size()==before,"application phase marker is hidden");
  console.accept("input: ",false,output::add);check(output.getLast().text().equals("input: "),"application prompt flushes without newline");
  console.accept("warning: normal program text\n",false,output::add);check(output.getLast().severity().equals("info"),"program stdout is not guessed as compiler warning");
  console.accept("Unhandled Exception",true,output::add);console.finish(output::add);check(output.getLast().severity().equals("error")&&output.getLast().text().equals("Unhandled Exception"),"partial application stderr retained on exit");
  console=new BoltConsole();output.clear();console.accept("x".repeat(140000),true,output::add);console.finish(output::add);check(output.stream().mapToInt(x->x.text().length()).sum()==140000,"unbounded lines cannot grow status buffer forever");
  String source="Foo(a,b)";Object help=Json.object("boltCallStart",Json.object("line",0,"character",3),"activeParameter",1,"activeSignature",1,"signatures",List.of(
    Json.object("label","int Foo(int a, int b)","parameters",List.of(Json.object("label","int a"),Json.object("label","int b"))),
    Json.object("label","double Foo(double a, double b)","parameters",List.of(Json.object("label",List.of(11,19)),Json.object("label",List.of(21,29))))));
  var decoded=BoltSignatures.decode(help,source);check(decoded!=null&&decoded.opening()==3&&decoded.argument()==1&&decoded.selected()==1,"parameter popup active overload and argument decode");
  check(decoded.signatures().get(0).parameters().get(1).start()==15,"repeated parameter types resolve correct label span");
  check(BoltSignatures.decode(Json.object(),source)==null,"invalid signature data creates no fake call");
  String original="int x=1;Print(x);";String uri="file:///tmp/Main.bolt";
  Object edit=Json.object("documentChanges",List.of(Json.object("textDocument",Json.object("uri",uri,"version",1),"boltOriginalText",original,"edits",List.of(Json.object("range",range(4,5),"newText","value"),Json.object("range",range(14,15),"newText","value")))));
  var changes=BoltEdits.decode(edit);check(changes.get(0).apply(original).equals("int value=1;Print(value);"),"rename edits apply descending offsets");
  reject(()->changes.get(0).apply(original+" "),"rename rejects stale document before edit");
  var overlap=Json.object("documentChanges",List.of(Json.object("textDocument",Json.object("uri",uri),"boltOriginalText",original,"edits",List.of(Json.object("range",range(4,8),"newText","x"),Json.object("range",range(5,6),"newText","y")))));
  reject(()->BoltEdits.decode(overlap),"overlapping workspace edits rejected");
  var builtin=Json.object("documentChanges",List.of(Json.object("textDocument",Json.object("uri","bolt-stdlib:///System/Console.bolt"),"boltOriginalText",original,"edits",List.of())));
  reject(()->BoltEdits.decode(builtin),"read-only virtual stdlib cannot be renamed");
  var duplicate=Json.object("documentChanges",List.of(Json.list(Json.get(edit,"documentChanges")).get(0),Json.list(Json.get(edit,"documentChanges")).get(0)));
  reject(()->BoltEdits.decode(duplicate),"duplicate rename document rejected");
  var symbol=new BoltSymbols.Symbol("method:Foo","Foo","method","int Foo(int a)","Example",List.of("int Foo(int a)","T Foo<T>(T a)"),Json.object("uri",uri,"range",range(0,3)),false);
  String html=BoltSymbols.documentation(symbol,1);check(html.contains("&lt;T&gt;")&&!html.contains("Foo<T>"),"hover escapes generic HTML");
  check(html.contains("psi_element://bolt-overload/0")&&html.contains("2 / 2"),"hover overload arrows have explicit targets");
  var sized=hoverSymbol("sizeBytes",8);html=BoltSymbols.documentation(sized,0);
  check(sized.sizeBytes()==8&&html.contains("<div class='content'>Size: 8 bytes</div>"),"hover decodes compiler byte size and renders its unit");
  check(html.indexOf("</pre>")<html.indexOf("Size: 8 bytes")&&!html.contains("Example")&&!html.contains("file:///Main.bolt"),"compact hover size follows definition without owner or source footer");
  check(sized.owner().equals("Example")&&Json.string(Json.get(sized.definition(),"uri")).equals("file:///Main.bolt"),"compact hover retains owner and definition metadata for navigation");
  check(BoltSymbols.documentation(hoverSymbol("sizeBytes",1),0).contains("Size: 1 byte</div>"),"one-byte hover size uses singular unit");
  check(BoltSymbols.documentation(hoverSymbol("sizeBytes",0),0).contains("Size: 0 bytes</div>"),"zero-byte compiler size remains visible");
  check(hoverSymbol("sizeBytes",4294967296L).sizeBytes()==4294967296L&&BoltSymbols.documentation(hoverSymbol("sizeBytes",Long.MAX_VALUE),0).contains("Size: 9223372036854775807 bytes"),"hover preserves full 64-bit byte sizes");
  var unsized=hoverSymbol();check(unsized.sizeBytes()==null&&!BoltSymbols.documentation(unsized,0).contains("Size:"),"older server without size metadata preserves hover content");
  for(Object invalid:List.of(-1,0.5,8.0,1.0e30,"8",true)) {
   var rejectedSize=hoverSymbol("sizeBytes",invalid);
   check(rejectedSize.sizeBytes()==null&&!BoltSymbols.documentation(rejectedSize,0).contains("Size:"),"invalid compiler size is not invented: "+invalid);
  }
  String note="runtime-dependent <T> & \"data\" 'size'";
  html=BoltSymbols.documentation(hoverSymbol("sizeNote",note),0);
  check(html.contains("Size: runtime-dependent &lt;T&gt; &amp; &quot;data&quot; &#39;size&#39;</div>")&&!html.contains("<T>"),"unknown runtime size note is displayed and HTML escaped");
  html=BoltSymbols.documentation(hoverSymbol("sizeBytes",8,"sizeNote","reference & <target>"),0);
  check(html.contains("Size: 8 bytes (reference &amp; &lt;target&gt;)</div>"),"compiler byte size includes escaped explanatory note");
  Object snapshot=Json.object("symbols",Json.object("s",Json.object("name","Foo","category","method","readOnly",false,"definition",symbol.definition(),"hover",Json.object("bolt",Json.object("label",symbol.label(),"owner","Example","overloads",symbol.overloads())))),"occurrences",List.of(Json.object("range",range(0,3),"symbol","s")),"virtualDocuments",Json.object("bolt-stdlib:///Test.bolt","class Test {}"));
  var symbols=new BoltSymbols(new BoltSession.Analysis(source,List.of(),List.of(),snapshot));
  check(symbols.at(0)!=null&&symbols.at(2)!=null&&symbols.at(3)==null,"compiler symbol hit uses exact identifier range");
  check(symbols.at(4)==null,"unresolved argument has no made-up definition");
  check(symbols.virtualDocuments().get("bolt-stdlib:///Test.bolt").equals("class Test {}"),"snapshot preserves embedded source text");
  check(symbols.definition(symbols.at(0).symbol(),1)==null,"older server never reuses selected location for another overload");
  Object secondDefinition=Json.object("uri","file:///Other.bolt","range",Json.object("start",Json.object("line",7,"character",1),"end",Json.object("line",7,"character",4)));
  snapshot=Json.object("symbols",Json.object("s",Json.object("name","Foo","category","method","readOnly",false,"definition",symbol.definition(),"hover",Json.object("bolt",Json.object("label",symbol.label(),"owner","Example","overloads",symbol.overloads(),"overloadDefinitions",List.of(symbol.definition(),secondDefinition),"sizeBytes",8,"sizeNote","reference")))),"occurrences",Json.get(snapshot,"occurrences"));
  symbols=new BoltSymbols(new BoltSession.Analysis(source,List.of(),List.of(),snapshot));
  var selected=symbols.selectOverload(symbols.at(0).symbol(),1);
  check(selected.label().equals("T Foo<T>(T a)")&&selected.definition().equals(secondDefinition),"hover selection retains the displayed overload's compiler source");
  check(selected.sizeBytes()==8&&selected.sizeNote().equals("reference")&&BoltSymbols.documentation(selected,1).contains("Size: 8 bytes (reference)"),"hover overload selection preserves compiler size metadata");
  check(symbols.definitions(selected).equals(List.of(secondDefinition)),"hover navigation selects the displayed overload only");
  String selectedHtml=BoltSymbols.documentation(selected,1,symbols.definition(selected,1));
  check(selectedHtml.contains("T Foo&lt;T&gt;(T a)")&&!selectedHtml.contains("file:///Other.bolt")&&!selectedHtml.contains(uri),"compact hover displays the selected overload without a source footer");
  Map<String,Object> group=new LinkedHashMap<>();for(var entry:Json.map(Json.get(Json.get(snapshot,"symbols"),"s")).entrySet())group.put(Json.string(entry.getKey()),entry.getValue());group.put("definitions",List.of(symbol.definition(),secondDefinition));
  snapshot=Json.object("symbols",Json.object("s",group),"occurrences",Json.get(snapshot,"occurrences"));
  symbols=new BoltSymbols(new BoltSession.Analysis(source,List.of(),List.of(),snapshot));
  check(symbols.definitions(symbols.at(0).symbol()).size()==2,"ambiguous known method group retains exact navigation choices");
  check(symbols.definitions(symbols.selectOverload(symbols.at(0).symbol(),1)).equals(List.of(secondDefinition)),"choosing an ambiguous hover overload yields one exact target");
  var typeSnapshot=Json.object("symbols",Json.object(
      "class:Geometry.Point",Json.object("name","Point","category","struct"),
      "class:Drawing.Point",Json.object("name","Point","category","class"),
      "class:System.Action<int>",Json.object("name","Action","category","interface"),
      "enum:State",Json.object("name","State","category","enum"),
      "local:42",Json.object("name","PretendType","category","variable")));
  var typeSymbols=new BoltSymbols(new BoltSession.Analysis("",List.of(),List.of(),typeSnapshot));
  check(typeSymbols.typeKind("Geometry.Point").equals("struct")&&typeSymbols.typeKind("Drawing.Point").equals("class"),"hover preserves compiler class/struct categories for qualified types");
  check(typeSymbols.typeKind("Point").isEmpty(),"hover does not guess the category of an ambiguous short type name");
  check(typeSymbols.typeKind("System.Action").equals("interface")&&typeSymbols.typeKind("Action").equals("interface"),"hover recognizes generic type base and short name");
  check(typeSymbols.typeKind("State").equals("enum")&&typeSymbols.typeKind("Missing").isEmpty()&&typeSymbols.typeKind("PretendType").isEmpty(),"hover type colors use only resolved type symbols");
  List<String> rendered=new ArrayList<>();
  String colored=BoltSymbols.documentation(symbol,1,secondDefinition,label->{rendered.add(label);return "<span style=\"color:#123456\">"+BoltSymbols.escape(label)+"</span>";});
  check(rendered.equals(List.of("T Foo<T>(T a)"))&&colored.contains("color:#123456\">T Foo&lt;T&gt;(T a)</span>"),"documentation colors the selected overload and preserves escaped signature HTML");
  check(!colored.contains("file:///Other.bolt")&&colored.contains("psi_element://bolt-overload/0"),"compact colored hover retains overload navigation without a source footer");
  System.out.println(count+" new IDE-core checks passed");return count;
 }
}
