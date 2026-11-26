#!/usr/bin/env python3
import argparse,pathlib,tempfile
from lsp_protocol import Client
p=argparse.ArgumentParser();p.add_argument('--bolt',required=True,type=pathlib.Path);a=p.parse_args();count=0
with tempfile.TemporaryDirectory(prefix='bolt-attrs14-') as d:
 r=pathlib.Path(d);(r/'bolt.proj').write_text('format=1\n[project]\nname="Attributes"\n');(r/'src').mkdir();f=r/'src/Main.bolt';f.write_text('');u=f.as_uri();c=Client(a.bolt)
 try:
  init=c.request('initialize',{'rootUri':r.as_uri(),'capabilities':{}})['result'];assert '[' in init['capabilities']['completionProvider']['triggerCharacters'];count+=1;c.send('initialized',{})
  version=0
  def test(body,yes=(),no=()):
   global count,version
   at=body.index('|');body=body.replace('|','');version+=1
   if version==1:c.send('textDocument/didOpen',{'textDocument':{'uri':u,'languageId':'bolt','version':version,'text':body}})
   else:c.send('textDocument/didChange',{'textDocument':{'uri':u,'version':version},'contentChanges':[{'text':body}]})
   c.barrier();before=body[:at];pos={'line':before.count('\n'),'character':len(before.rsplit('\n',1)[-1].encode('utf-16-le'))//2}
   out=c.request('textDocument/completion',{'textDocument':{'uri':u},'position':pos})['result'];items=out['items'];names={i['label'] for i in items}
   for name in yes:assert name in names,(name,names)
   for name in no:assert name not in names,(name,names)
   if yes:assert all(i.get('data',{}).get('bolt',{}).get('category')=='attribute' for i in items),names
   count+=1;print('PASS',count,yes,no,flush=True)
  test('[|',('Reflect','JsonName','ReadOnly'),('Console','int'))
  test('[Re|',('Reflect','ReadOnly'),('JsonName',))
  test('[Reflect, |',('Header','JsonName'))
  prefix='import Demo\nnamespace Here;\n'
  helper=r/'src/Custom.bolt';helper.write_text('namespace Demo;\npublic class FancyAttribute : System.Attribute {}\npublic class ChildAttribute : FancyAttribute {}\npublic class Ordinary {}\n')
  c.send('textDocument/didOpen',{'textDocument':{'uri':helper.as_uri(),'languageId':'bolt','version':1,'text':helper.read_text()}});c.barrier()
  test(prefix+'[|',('Fancy','Child'),('Ordinary','System.Attribute'))
  test('namespace Here;\n[Demo.|',('Demo.Fancy','Demo.Child'),('Reflect','Ordinary'))
  test('public class OwnAttribute : System.Attribute {}\n[Ow|',('Own',))
  test('public class OwnAttribute {}\n[Ow|',(),('Own',))
  test('static class Program{public: void Main(){ int[] a=new int[3]; a[|; }}',(),('Reflect','JsonName'))
  test('static class Program{public: void Main(){ int[| x; }}',(),('Reflect',))
  test('// [|',(),('Reflect',))
  test('/* [| */',(),('Reflect',))
  test('static class Program{public: void Main(){ Print("[|"); }}',(),('Reflect',))
  test('[Header("|")]',(),('Reflect',))
  test('#if false\n[|\n#endif',(),('Reflect',))
 finally:c.stop()
print(count,'attribute completion checks passed')
