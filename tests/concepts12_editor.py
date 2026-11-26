#!/usr/bin/env python3
"""Actual protocol diagnostics and symbol-only concept/error colors."""
import tempfile, argparse
from pathlib import Path
from lsp_protocol import Client, position
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=Path);a=p.parse_args();count=0

def check(value,name):
 global count
 assert value,name
 count+=1;print('PASS '+name,flush=True)
with tempfile.TemporaryDirectory(prefix='bolt-project-colors12-') as d:
 root=Path(d);(root/'src').mkdir();(root/'bolt.proj').write_text('format=1\n[project]\nname="Colors"\n');path=root/'src/Main.bolt';uri=path.as_uri()
 source='''concept Scalar<T> = Number<T> && Addable<T>;
public static class Program {
public:
    T Add<T>(T a,T b):Scalar<T>{return a+b;}
    void Main(){
        int value=1;
        Console.WriteLine(Add<int>(value,2));
    }
}
''';path.write_text(source)
 c=Client(a.bolt);c.request('initialize',{'processId':None,'rootUri':root.as_uri(),'initializationOptions':{'boltSemanticNotifications':True},'capabilities':{}});c.send('initialized',{})
 try:
  def update(text,version=1):
   if version==1:c.send('textDocument/didOpen',{'textDocument':{'uri':uri,'languageId':'bolt','version':version,'text':text}})
   else:c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':text}]})
   c.barrier()
  def kind(text,word,index=0):
   pos=position(text,word,index)
   return next((t['kind'] for t in c.latest('bolt/semanticTokens',uri)['tokens'] if t['line']==pos['line'] and t['start']==pos['character']),None)
  update(source)
  check(not c.latest('textDocument/publishDiagnostics',uri)['diagnostics'],'manifest project language analysis succeeds')
  for word,index in [('Scalar',0),('Scalar',1),('Number',0),('Addable',0)]:check(kind(source,word,index)=='concept',f'concept semantic color {word}#{index}')
  text=source.replace('Console.WriteLine(Add<int>(value,2));','Console.WriteLine(Add<int>(value,2));\n        Lol();')
  update(text,2)
  check(bool(c.latest('textDocument/publishDiagnostics',uri)['diagnostics']),'unknown method diagnostic exists')
  check(kind(text,'Lol') in (None,'identifier'),'unknown Lol call is not method-colored')
  check(kind(text,'Console') in ('class','type'),'resolved Console remains colored with unrelated error')
  check(kind(text,'WriteLine')=='method','resolved WriteLine remains colored with unrelated error')
  check(kind(text,'value',1) in ('variable','local','localVariable'),'existing variable remains colored with unrelated error')
  text=source.replace('Console.WriteLine(Add<int>(value,2));','Console.WriteLine("ok");\n        int unfinished\n')
  update(text,3)
  check(bool(c.latest('textDocument/publishDiagnostics',uri)['diagnostics']),'missing semicolon retains diagnostic')
  check(kind(text,'Console') in ('class','type'),'class color persists on incomplete source')
  check(kind(text,'WriteLine')=='method','method color persists on incomplete source')
  text=source.replace('Add<int>(value,2)','Add<int>("wrong",2)');update(text,4)
  check(bool(c.latest('textDocument/publishDiagnostics',uri)['diagnostics']),'wrong arguments are diagnosed')
  check(kind(text,'Add<int>')=='method','actual method keeps color despite argument mismatch')
  # nonexistent concept cannot receive semantic concept coloring
  text=source.replace('Number<T>','Nonexistent<T>');update(text,5)
  check(kind(text,'Nonexistent') not in ('class','type','concept'),'unresolved concept has no invented concept/type color')
 finally:c.stop()
print(f'{count} concept/error-color protocol checks passed')
