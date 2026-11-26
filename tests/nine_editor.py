#!/usr/bin/env python3
"""Real LSP interactions for .bolt properties, setters and multi-declarators."""
from pathlib import Path
import argparse,tempfile
from lsp_protocol import Client,position

def main():
 p=argparse.ArgumentParser();p.add_argument('--bolt',type=Path,required=True);a=p.parse_args();passed=0
 def check(v,name):
  nonlocal passed
  assert v,name;passed+=1;print('PASS '+name,flush=True)
 with tempfile.TemporaryDirectory(prefix='bolt09-editor-') as d:
  root=Path(d);(root/'bolt.project').write_text('{}');path=root/'Program.bolt';uri=path.as_uri()
  source='''public class Counter {
public:
    int X, Y;
    int Count { get; private set; }
    int Clamped { get { return Count; } set { Count = value; } }
}
public static class Program {
    public static void Main() {
        Counter counter;
        int first = 1, second = 2;
        counter.Count;
    }
}
''';path.write_text(source)
  c=Client(a.bolt);c.request('initialize',{'processId':None,'rootUri':root.as_uri(),'initializationOptions':{'boltSemanticNotifications':True},'capabilities':{}});c.send('initialized',{})
  c.send('textDocument/didOpen',{'textDocument':{'uri':uri,'languageId':'bolt','version':1,'text':source}});c.barrier()
  check(not c.latest('textDocument/publishDiagnostics',uri)['diagnostics'],'.bolt project with properties and commas is analyzed')
  def kind(word,index=0):
   pos=position(source,word,index)
   return next((t['kind'] for t in c.latest('bolt/semanticTokens',uri)['tokens'] if t['line']==pos['line'] and t['start']==pos['character']),None)
  check(kind('Count {')=='property','property declaration has semantic property color')
  check(kind('Y;')=='field','second field declarator is a field')
  text=source.replace('counter.Count;','counter.');caret=position(text,'counter.',inner=len('counter.'))
  c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':2},'contentChanges':[{'text':text}]})
  result=c.request('textDocument/completion',{'textDocument':{'uri':uri},'position':caret})['result']['items']
  labels={i['label'] for i in result}
  check({'X','Y','Count','Clamped'} <= labels,'completion includes both fields and properties in broken source')
  check(not any('@get' in n or '@set' in n or '@property' in n for n in labels),'synthetic accessors/backing fields stay hidden')
  prop=next(i for i in result if i['label']=='Count');check(prop['kind']==10,'property completion has Property LSP kind')
  c.stop()
 print(f'{passed} editor-property checks passed',flush=True)
if __name__=='__main__':main()
