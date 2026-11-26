#!/usr/bin/env python3
"""Regression cases for partial semantic coloring and real import completion."""
import argparse
from pathlib import Path
import tempfile
from lsp_protocol import Client, position

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--bolt',type=Path,required=True);args=ap.parse_args();passed=0
    with tempfile.TemporaryDirectory(prefix='bolt-editor-resilience-') as directory:
        root=Path(directory);(root/'bolt.project').write_text('{}')
        helper=root/'Helpers.bolt';helper.write_text('namespace Demo.Utility; public class Helper { public: static int Twice(int n){return n*2;} }')
        file=root/'Program.bolt';file.write_text('static class Program {public:void Main(){}}');uri=file.as_uri()
        client=Client(args.bolt);version=0;source=''
        client.request('initialize',{'processId':None,'rootUri':root.as_uri(),'initializationOptions':{'boltSemanticNotifications':True},'capabilities':{}});client.send('initialized',{})
        def update(text):
            nonlocal version,source
            source=text;version+=1
            if version==1:client.send('textDocument/didOpen',{'textDocument':{'uri':uri,'languageId':'bolt','version':version,'text':text}})
            else:client.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':text}]})
            client.barrier()
            return client.latest('textDocument/publishDiagnostics',uri)['diagnostics']
        def kind(word,index=0):
            pos=position(source,word,index)
            for token in client.latest('bolt/semanticTokens',uri)['tokens']:
                if token['line']==pos['line'] and token['start']==pos['character']:return token['kind']
            return None
        def check(value,message):
            nonlocal passed
            assert value,message;passed+=1;print('PASS '+message,flush=True)
        def completion(prefix):
            text=prefix+'\nstatic class Program {public:void Main(){}}';update(text)
            result=client.request('textDocument/completion',{'textDocument':{'uri':uri},'position':position(text,'\n',inner=0)})
            check('error' not in result,'completion returns a result for '+prefix)
            return result['result']['items']
        base='''import Demo.Utility
public static class Program {
  public static int Twice(int n) {return n*2;}
  public static void Main() {
    Console.WriteLine("hello 🐈");
    int total = Twice(12);
    Helper.Twice(total);
    int x%s
  }
}
'''
        try:
            # Start with invalid text; this must not depend on a stale good snapshot.
            diagnostics=update(base%'')
            check(bool(diagnostics),'missing semicolon is still diagnosed')
            for word,category,index in [('Program','class',0),('Main','method',0),('Console','class',0),('WriteLine','method',0),('Twice','method',1),('Helper','class',0),('Twice','method',2),('total','variable',1)]:
                check(kind(word,index)==category,f'broken first-open retains {word} occurrence {index} as {category}; received {kind(word,index)}')
            diagnostics=update(base%';');check(not diagnostics,'fixing the syntax removes diagnostics')
            check(kind('WriteLine')=='method','valid source has method color')
            diagnostics=update((base%';').replace('int x;','UnknownThing x;'))
            check(bool(diagnostics),'semantic error retained')
            check(kind('WriteLine')=='method','unrelated semantic error preserves preceding method')
            diagnostics=update((base%';').replace('int x;','int x = ;'))
            check(bool(diagnostics) and kind('Main')=='method' and kind('WriteLine')=='method','incomplete initializer does not clear the scope')
            diagnostics=update((base%';').replace('int x;','int x = "unterminated'))
            check(bool(diagnostics) and kind('WriteLine')=='method','unclosed string retains earlier valid call')
            # Return to a syntactically valid project before separate import edits.
            items=completion('import ');names={x['label'] for x in items}
            check({'System.IO','System.Threading.Tasks','Math','Ecs','Demo.Utility'}<=names,'empty import lists actual standard and project namespaces')
            by_name={x['label']:x for x in items}
            for name,target in [('File','System.IO.File'),('FileStream','System.IO.FileStream'),('Console','System.Console'),('Task','System.Threading.Tasks.Task')]:
                item=by_name[name]
                check(item['kind']==7 and item['detail']=='class '+target and item['data']['bolt']['type']==target,
                      name+' import identifies its declared class')
            check(all(by_name[n]['kind']==9 for n in ('Ecs','System','System.IO','Demo.Utility','Helpers')),
                  'namespaces and source modules keep module icons')
            check(all(x['kind'] in (7,8,9,13,22) for x in items),'import results contain only types and modules')
            items=completion('import F')
            check(all(x['kind']==7 and x['textEdit']['newText']==x['label'] for x in items)
                  and {'File','FileStream'}<={x['label'] for x in items},'File prefix retains class icons and short import spelling')
            items=completion('import System.');names={x['label'] for x in items}
            check('System.IO' in names and all(n.startswith('System.') for n in names),'dotted import narrows suggestions')
            io=next(x for x in items if x['label']=='System.IO')
            check(io['textEdit']['range']['start']=={'line':0,'character':7} and io['textEdit']['newText']=='System.IO','qualified import replaces entire prefix exactly once')
            items=completion('import Sys');check(any(x['label']=='System.IO' for x in items),'partial import prefix supported')
            items=completion('import System.Threading.');check([x['label'] for x in items]==['System.Threading.Tasks'],'nested standard import supported')
            items=completion('import Demo.');check(any(x['label']=='Demo.Utility' for x in items),'project namespace available with dotted prefix')
            # New unsaved files are included by the compiler's source override model.
            other=root/'NotSaved.bolt';client.send('textDocument/didOpen',{'textDocument':{'uri':other.as_uri(),'languageId':'bolt','version':1,'text':'namespace Unwritten.Tools; public class Tool {}'}})
            items=completion('import Unw');check(any(x['label']=='Unwritten.Tools' for x in items),'unsaved file contributes namespaces')
            client.send('textDocument/didChange',{'textDocument':{'uri':other.as_uri(),'version':2},'contentChanges':[{'text':'namespace Renamed.Tools; public class Tool {}'}]})
            items=completion('import Unw');check(not items,'renaming unsaved namespace does not leave stale import suggestions')
            items=completion('import Ren');check(any(x['label']=='Renamed.Tools' for x in items),'renamed namespace becomes available')
            # Not module completion inside comments or strings.
            update('// import System.\nstatic class Program{public:void Main(){}}')
            ret=client.request('textDocument/completion',{'textDocument':{'uri':uri},'position':{'line':0,'character':17}})['result']['items']
            check(not any(x.get('kind')==9 for x in ret),'no import completion in line comment')
        finally:client.stop()
    print(f'{passed} editor resilience/import checks passed')
if __name__=='__main__':main()
