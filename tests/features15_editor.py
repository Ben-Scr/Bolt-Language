#!/usr/bin/env python3
"""Real compiler LSP tests for editing/navigation/signatures; no UI claims."""
import argparse,json,tempfile
from pathlib import Path
from lsp_protocol import Client,position
parser=argparse.ArgumentParser();parser.add_argument('--bolt',required=True,type=Path);args=parser.parse_args();n=0

def check(cond,name,detail=None):
 global n
 assert cond,(name,detail)
 n+=1;print('PASS',name,flush=True)

def edits_text(result,text,uri):
 for doc in result['documentChanges']:
  if doc['textDocument']['uri']!=uri:continue
  assert doc['boltOriginalText']==text
  def off(p):return sum(len(x) for x in text.splitlines(keepends=True)[:p['line']])+len(text.splitlines()[p['line']].encode('utf-16-le')[:p['character']*2].decode('utf-16-le'))
  for e in sorted(doc['edits'],key=lambda e:(e['range']['start']['line'],e['range']['start']['character']),reverse=True):text=text[:off(e['range']['start'])]+e['newText']+text[off(e['range']['end']):]
 return text
with tempfile.TemporaryDirectory(prefix='bolt-editor15-ü ') as tmp:
 root=Path(tmp);(root/'bolt.project').write_text('{}');file=root/'Main.bolt';uri=file.as_uri()
 helpers='''namespace Models;
public class Point {
public:
 int X;
 Point(int x=0){X=x;}
 void Move(int amount){X+=amount;}
}
'''
 helper=root/'Point.bolt';helper.write_text(helpers)
 source='''import Models
import System.Collections.Generic
concept Numeric<T> = Number<T>;
static class Program {
public:
 int Add(int a,int b){return a+b;}
 double Add(double a,double b){return a+b;}
 T Identity<T>(T value){return value;}
 T First<T,U>(T a,U b){return a;}
 void Main(){
   Point point = Point(1);
   point.Move(2);
   Print(Add(1,2));
   Print(Add(1.0,2.0));
   Print(First<int,string>(Identity<int>(1),"ok"));
   List<int> values;
   int local=1;
   // point local Move Add should not be renamed in comments
   Print("local");Print(local);
 }
}
'''
 file.write_text(source);c=Client(args.bolt.resolve());version=0
 try:
  init=c.request('initialize',{'rootUri':root.as_uri(),'capabilities':{},'initializationOptions':{'boltSemanticNotifications':True}})['result'];c.send('initialized',{})
  def update(text):
   global version
   version+=1
   if version==1:c.send('textDocument/didOpen',{'textDocument':{'uri':uri,'languageId':'bolt','version':version,'text':text}})
   else:c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':text}]})
   c.barrier()
  def req(method,text,word='',inner=0,occurrence=0,**extra):
   p={'textDocument':{'uri':uri},**extra}
   if word:p['position']=position(text,word,occurrence,inner)
   return c.request(method,p)
  update(source)
  check(c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[],'clean multifile program')
  check(init['capabilities']['renameProvider']['prepareProvider'],'rename capability')
  r=req('textDocument/definition',source,'point.Move',inner=7)['result'];check(r[0]['uri']==helper.as_uri() and r[0]['range']['start']==position(helpers,'Move'),'Ctrl-click cross-file method location',r)
  h=req('textDocument/hover',source,'Add(1,2)',inner=1)['result'];check(len(h['bolt']['overloads'])==2 and 'int' in h['bolt']['label'],'hover exact overload plus all alternatives',h)
  labels=h['bolt']['overloads'];targets=h['bolt']['overloadDefinitions'];check(len(targets)==len(labels),'each hover overload carries its compiler definition',h)
  for label,target in zip(labels,targets):
   check(target['uri']==uri and target['range']['start']==position(source,'Add(int' if label.startswith('int ') else 'Add(double'),'hover alternate source matches its displayed signature',target)
  check(targets[labels.index(h['bolt']['label'])]==h['bolt']['definition'],'default hover source remains the selected call overload')
  h=req('textDocument/hover',source,'Point point',inner=1)['result'];check('Point' in h['contents']['value'],'class hover')
  for call,wanted in [('Add(1,2)','int'),('Add(1.0,2.0)','double')]:
   r=req('textDocument/signatureHelp',source,call,inner=len(call)-1)['result'];check(r and r['activeParameter']==1 and r['signatures'][r['activeSignature']]['label'].startswith(wanted+' '),'signature selection '+call,r)
  r=req('textDocument/signatureHelp',source,'First<int,string>(Identity<int>(1),"ok")',inner=len('First<int,string>(Identity<int>(1),'))['result'];check(r and r['activeParameter']==1 and 'string' in str(r['signatures']),'generic and nested-call commas not parameter separators',r)
  r=req('textDocument/signatureHelp',source,'Identity<int>(1)',inner=len('Identity<int>('))['result'];check(r and r['activeParameter']==0,'innermost call parameter help',r)
  r=req('textDocument/boltSymbolIndex',source)['result'];check(any(x['category']=='concept' for x in r['symbols'].values()),'symbol index concepts');check(any(k.startswith('bolt-stdlib:') for k in r['virtualDocuments']),'embedded stdlib virtual source',r.keys())
  target=req('textDocument/definition',source,'List<int>',inner=1)['result'][0];check(target['uri'].startswith('bolt-stdlib:///'),'stdlib navigates virtual source not fake file',target)
  virtual=c.request('bolt/virtualDocument',{'uri':target['uri']})['result'];check('class List' in str(virtual),'virtual source contains actual embedded library')
  refs=req('textDocument/references',source,'point.Move',context={'includeDeclaration':True})['result'];check(len(refs)==2,'variable references not comment name matches',refs)
  rename=req('textDocument/rename',source,'Print(local)',inner=7,newName='renamed');check('result' in rename,'semantic rename accepted',rename)
  changed=edits_text(rename['result'],source,uri);check('int renamed=1' in changed and 'Print(renamed)' in changed and 'Print("local")' in changed,'rename exact references not strings')
  check('point local Move Add should' in changed,'rename does not touch comments')
  rename=req('textDocument/rename',source,'Add(1,2)',inner=1,newName='Sum');check('result' in rename,'one overload rename validated',rename)
  changed=edits_text(rename['result'],source,uri);check('Sum(1,2)' in changed and 'Add(1.0,2.0)' in changed,'rename only selected overload')
  rename=req('textDocument/rename',source,'Point point',inner=1,newName='Vector');check('result' in rename,'type rename updates constructor and uses',rename)
  check(len(rename['result']['documentChanges'])==2,'type rename cross-file transaction')
  # Complete expressions prefer the compiler-selected overload, not token guesses.
  signatures=source.replace(' void Main(){',' int Choose(bool value){return 1;}\n int Choose(int value){return 2;}\n void Main(){').replace('   Point point = Point(1);','   Point point = Point(1);\n   Print(Choose(1<2));')
  update(signatures)
  r=req('textDocument/signatureHelp',signatures,'Point(1)',inner=7)['result'];check(r and 'int x' in str(r['signatures']),'constructor parameter info',r)
  r=req('textDocument/signatureHelp',signatures,'Choose(1<2)',inner=len('Choose(1<2'))['result'];check(r and 'bool value' in r['signatures'][r['activeSignature']]['label'],'semantic overload for a complete comparison expression',r)
  packs=source.replace(' void Main(){',' void Values<T...>(T... values){}\n void Prefixed<T...>(int first,T... values){}\n void Homogeneous(int... values){}\n void Main(){').replace('Print(Add(1,2));','Values(1,"two",true);Prefixed(0,1,"two",true);Homogeneous(1,2,3);')
  update(packs)
  check(not c.latest('textDocument/publishDiagnostics',uri)['diagnostics'],'signature fixture accepts heterogeneous and homogeneous packs')
  for call,index in [('Values(1,"two",true)',0),('Prefixed(0,1,"two",true)',1),('Homogeneous(1,2,3)',0)]:
   r=req('textDocument/signatureHelp',packs,call,inner=len(call)-1)['result'];selected=r['signatures'][r['activeSignature']]
   check(r['activeParameter']==index and selected['activeParameter']==index and index<len(selected['parameters']),'variadic active parameter stays within displayed pack signature '+call,r)
  broken=source.replace('Print(Add(1,2));','Print(Add("bad"));')
  update(broken)
  r=req('textDocument/definition',broken,'Add("bad")',inner=1)['result'];check(r==[],'invalid overload call has no guessed definition',r)
  h=req('textDocument/hover',broken,'Add("bad")',inner=1)['result'];check(h is None,'invalid overload call has no guessed hover',h)
  ambiguous=source.replace(' void Main(){',' int Choose(int a,double b){return a;}\n int Choose(double a,int b){return b;}\n void Main(){').replace('Print(Add(1,2));','Print(Choose(1,1));')
  update(ambiguous)
  r=req('textDocument/definition',ambiguous,'Choose(1,1)',inner=1)['result'];check(len(r)==2,'equally viable overloads offer exact declaration choices',r)
  snapshot=req('textDocument/boltSymbolIndex',ambiguous)['result'];check(any(x.get('ambiguous') and len(x['definitions'])==2 and x['readOnly'] for x in snapshot['symbols'].values()),'ambiguous choices cannot trigger guessed rename',snapshot.keys())
  update(source)
  # Core symbols and malformed names must not be renamed.
  check('error' in req('textDocument/rename',source,'List<int>',newName='Broken'),'readonly stdlib rename rejected')
  check('error' in req('textDocument/rename',source,'local=1',newName='if'),'keyword rename rejected')
  capture=source.replace('int local=1;','int local=1;int other=2;').replace('Print(local);','Print(local);Print(other);')
  update(capture);check('error' in req('textDocument/rename',capture,'local=1',newName='other'),'name collision/capture rejected')
  for text,expect in [('List<',True),('Identity<',True),('First<',True),('Point<',False),('local <',False),('Unknown<',False),('if(local <',False),('for(int i=0; i <',False),('while(local <',False),('Console.WriteLine<',True)]:
   edit=source.replace('int local=1;','int local=1;\n'+text)
   update(edit);r=req('textDocument/boltGenericContext',edit,text,inner=len(text))['result'];check(r['generic']==expect,'generic pairing context '+text,r)
  shadow=source.replace('int local=1;','int local=1;int Identity=2;Identity<')
  update(shadow);check(not req('textDocument/boltGenericContext',shadow,'Identity<',inner=9,occurrence=shadow.count('Identity<')-1)['result']['generic'],'local shadows generic method')
  unknown=source.replace('Print(local);','Print(local);Lol();')
  update(unknown);check(req('textDocument/hover',unknown,'Lol()')['result'] is None,'unknown callable has no hover')
  check(req('textDocument/definition',unknown,'Lol()')['result']==[],'unknown callable has no definition')
  check('error' in req('textDocument/rename',unknown,'local=1',newName='changed'),'rename requires sound project instead of guessing')
  tokens=c.latest('bolt/semanticTokens',uri)['tokens'];check(not any(t['line']==position(unknown,'Lol')['line'] and t['start']==position(unknown,'Lol')['character'] and t['kind'] not in ('identifier',) for t in tokens),'unknown callable not colored')
  events='''class P {public:event Action<int> Changed;void Raise(){Changed.Invoke(1);}} static class Program{public:void Handler(int x){} void Main(){P p;p.Changed+=Handler; p.Changed. }}'''
  update(events);r=req('textDocument/completion',events,'p.Changed.',inner=len('p.Changed.'))['result'];items=r.get('items',[]) if isinstance(r,dict) else r;check(not items,'external event does not suggest Invoke or storage',items)
  valid_event=events.replace(' p.Changed. ',' ')
  update(valid_event)
  r=req('textDocument/definition',valid_event,'Action<int>',inner=1)['result'];check(r and r[0]['uri'].endswith('/System/Action.bolt'),'Action annotation navigates delegate type, not private event storage',r)
  inside=events.replace('void Raise(){Changed.Invoke(1);}','void Raise(){Changed.}').replace(' p.Changed. ',' ')
  update(inside);r=req('textDocument/completion',inside,'Changed.',inner=len('Changed.'))['result'];items=r.get('items',[]) if isinstance(r,dict) else r;check(any('Invoke' in x['label'] for x in items),'declaring type event Invoke completion',items)
  update(source);c.stop()
 finally:c.kill()
print(n,'editor checks passed')
