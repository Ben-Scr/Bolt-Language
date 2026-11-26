#!/usr/bin/env python3
"""Actual LSP packets: only known attributes; conditional source and optional diagnostics."""
import argparse,pathlib,tempfile,json
from lsp_protocol import Client,position
P=pathlib.Path
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=P);a=p.parse_args();count=0

def check(ok,msg):
 global count
 if not ok:raise AssertionError(msg)
 count+=1;print('PASS '+msg,flush=True)
with tempfile.TemporaryDirectory(prefix='bolt-attributes13-') as d:
 root=P(d);(root/'bolt.proj').write_text('format=1\n[project]\nname="Editor13"\n');(root/'src').mkdir();path=root/'src/Main.bolt';uri=path.as_uri()
 source='''import System
namespace Demo;
public class FancyAttribute : System.Attribute {}
public class FancyChildAttribute : FancyAttribute {}
public class Ordinary {}
[Reflect]
[Fancy]
[FancyChild]
[Unknown]
[Ordinary]
public class Player {
public:
    [JsonName("score")] int Score;
    [JsonIgnore] int Hidden;
    [ReadOnly] int Read() { return Score; }
}
[Reflect] public enum State { Idle, Running }
#ifdef Debug
public class DebugType {}
#else
public class ReleaseType {}
#endif
static class Program {
public:
    void Main() {
        Player? value = null;
        if (value != null) { value.Score = 4; }
        Console.WriteLine("#ifdef Windows");
    }
}
'''
 path.write_text(source);c=Client(a.bolt)
 try:
  c.request('initialize',{'rootUri':root.as_uri(),'capabilities':{},'initializationOptions':{'boltSemanticNotifications':True}})
  c.send('initialized',{})
  def update(text,version=1):
   if version==1:c.send('textDocument/didOpen',{'textDocument':{'uri':uri,'languageId':'bolt','version':version,'text':text}})
   else:c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':text}]})
   c.barrier()
  def kind(text,word,occurrence=0,inside=0):
   pos=position(text,word,occurrence,inside);spans=c.latest('bolt/semanticTokens',uri)['tokens']
   return next((s['kind'] for s in spans if s['line']==pos['line'] and s['start']<=pos['character']<s['start']+s['length']),None)
  update(source)
  check(not c.latest('textDocument/publishDiagnostics',uri)['diagnostics'],'valid attribute and optional source analyzed')
  for name in ('Reflect','Fancy','FancyChild','JsonName','JsonIgnore','ReadOnly'):
   check(kind(source,'['+name,inside=1)=='decorator','resolved attribute color '+name)
  check(kind(source,'[Unknown]',inside=1) not in ('decorator','class','type'),'unknown attribute is not colored as a type')
  check(kind(source,'[Ordinary]',inside=1) not in ('decorator','class','type'),'ordinary class is not an attribute class')
  check(kind(source,'[Reflect]',1,1)=='decorator','enum attribute color')
  check(kind(source,'"score"',inside=1)=='string','attribute string arguments retain literal color')
  check(kind(source,'#ifdef Debug')=='macro','conditional directive color')
  check(kind(source,'DebugType')=='class' and kind(source,'ReleaseType')=='comment','active profile declaration vs excluded code')
  c.send('workspace/didChangeConfiguration',{'settings':{'bolt':{'profile':'Release'}}});c.barrier()
  check(kind(source,'ReleaseType')=='class' and kind(source,'DebugType')=='comment','LSP profile selection reverses branches')
  c.send('workspace/didChangeConfiguration',{'settings':{'bolt':{'profile':''}}});c.barrier()
  check(kind(source,'DebugType')=='class','empty profile override restores project default')
  # Unknown identifiers never regain synthetic colors under an unrelated diagnostic.
  invalid=source.replace('Console.WriteLine("#ifdef Windows");','Console.WriteLine("#ifdef Windows"); Lol();')
  update(invalid,2);check(bool(c.latest('textDocument/publishDiagnostics',uri)['diagnostics']),'unknown method diagnostic retained')
  check(kind(invalid,'Lol()') not in ('method','class','decorator'),'unknown method remains neutral')
  check(kind(invalid,'[Fancy]',inside=1)=='decorator' and kind(invalid,'[Reflect]',inside=1)=='decorator','known attribute colors survive semantic errors')
  invalid=source.replace('Console.WriteLine("#ifdef Windows");','Console.WriteLine("#ifdef Windows"); int missing')
  update(invalid,3);check(bool(c.latest('textDocument/publishDiagnostics',uri)['diagnostics']),'syntax error reported')
  check(kind(invalid,'[FancyChild]',inside=1)=='decorator','custom attribute ancestry recovered despite syntax error')
  check(kind(invalid,'[Unknown]',inside=1) not in ('decorator','class','type'),'unknown attribute stays neutral during recovery')
  # Attribute named Class exists, but its base named Attribute is unrelated to System.Attribute.
  wrong=source.replace('public class Ordinary {}','public class Attribute {}\npublic class Ordinary : Attribute {}')
  update(wrong,4);check(kind(wrong,'[Ordinary]',inside=1) not in ('decorator','class','type'),'unrelated Attribute base is not mistaken for System.Attribute')
  # Completion exposes safe optional properties even before a value is proven.
  partial=source.replace('if (value != null) { value.Score = 4; }','value.')
  update(partial,5);pos=position(partial,'value.',inner=len('value.'))
  result=c.request('textDocument/completion',{'textDocument':{'uri':uri},'position':pos})['result']
  items=result['items'] if isinstance(result,dict) else result
  names={i['label'].split('(')[0] for i in items}
  check('HasValue' in names and 'Value' in names,'optional completion includes HasValue and Value')
  # Both accesses remain property tokens, narrowing doesn't recolor variable as Value.
  text=source.replace('if (value != null) { value.Score = 4; }','Console.WriteLine(value.HasValue); if (value != null) { Console.WriteLine(value.Value.Score); }')
  update(text,6)
  check(kind(text,'value.HasValue',inside=6)=='property' and kind(text,'value.Value.Score',inside=6)=='property','optional wrapper members are properties')
  check(kind(text,'value != null') in ('variable','local'),'guarded variable retains variable color')
  # Unsupported platform syntax/imports may exist in disabled branches.
  text='#if false\nimport "Missing.bolt"\n[Reflect] class Broken { @\n#endif\n'+source
  update(text,7);check(not c.latest('textDocument/publishDiagnostics',uri)['diagnostics'],'inactive import/syntax excluded from editor diagnostics')
  check(kind(text,'[Reflect]',inside=1)=='comment','inactive attribute is not semantically colored')
  # Known attribute disappears if its declaration is removed in an unsaved document.
  text=source.replace('public class FancyAttribute : System.Attribute {}','public class FancyAttribute {}')
  update(text,8);check(kind(text,'[Fancy]',inside=1) not in ('decorator','class','type'),'removed attribute ancestry invalidates old semantic color')
  # Masked bytes include CRLF and astral Unicode, retaining correct UTF-16 positions.
  text=('// 😀\n'+source).replace('\n','\r\n');update(text,9)
  check(kind(text,'[Reflect]',inside=1)=='decorator','CRLF/UTF16 attribute positions')
 finally:c.stop()
print(f'{count} attribute/preprocessor/optional LSP checks passed')
