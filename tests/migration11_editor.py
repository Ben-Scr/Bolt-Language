#!/usr/bin/env python3
"""Actual stdio LSP completion/diagnostics for the migrated source APIs."""
import argparse
from pathlib import Path
import tempfile
from lsp_protocol import Client

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bolt',required=True,type=Path);a=p.parse_args();count=0
    with tempfile.TemporaryDirectory(prefix='bolt-editor11-') as directory:
        root=Path(directory);(root/'bolt.project').write_text('{}');source=root/'Program.bolt';source.write_text('')
        client=Client(a.bolt);version=0
        client.request('initialize',{'processId':None,'rootUri':root.as_uri(),'capabilities':{}});client.send('initialized',{})
        def text(body):return 'import System\nimport System.IO\nimport System.Threading\nimport System.Threading.Tasks\nimport System.Diagnostics\nimport System.Text.Json\npublic static class Program{public:void Main(){'+body+'}}'
        def opened(content):
            nonlocal version
            version+=1
            if version==1:client.send('textDocument/didOpen',{'textDocument':{'uri':source.as_uri(),'languageId':'bolt','version':version,'text':content}})
            else:client.send('textDocument/didChange',{'textDocument':{'uri':source.as_uri(),'version':version},'contentChanges':[{'text':content}]})
        try:
            for body,names in [
                ('Console.|',{'Clear','ReadLine','Write','WriteLine'}),
                ('Thread.|',{'Sleep','Yield'}),
                ('Stopwatch watch;watch.|',{'ToString','ElapsedMilliseconds','Start','Pause','Unpause'}),
                ('Task<int> task;task.|',{'GetResult','Wait','Result','IsCompleted','IsFaulted'}),
                ('Task<void>.|',{'CompletedTask','Delay','WhenAll'}),
                ('Json.|',{'Serialize','SerializeBytes','SerializeBinary','Deserialize','DeserializeBinary'}),
                ('Converter.|',{'ToInt','ToUInt64','ToChar','ToBool','TryTo','To'}),
                ('Directory.|',{'Create','Delete','GetFiles','GetDirectories','GetFileSystemEntries'}),
                ('StreamWriter writer=StreamWriter("out.txt");writer.|',{'Write','WriteLine','Flush','Close','Dispose'})]:
                content=text(body);at=content.index('|');content=content.replace('|','');prefix=content[:at];opened(content)
                pos={'line':prefix.count('\n'),'character':len(prefix.rsplit('\n',1)[-1].encode('utf-16-le'))//2}
                result=client.request('textDocument/completion',{'textDocument':{'uri':source.as_uri()},'position':pos}).get('result',{}).get('items',[])
                found={v['label'] for v in result};assert names<=found,(body,names-found,result)
                if body=='Thread.|':assert 'int milliseconds' in next(x['detail'] for x in result if x['label']=='Sleep'),result
                if body=='Console.|':assert any(x['label']=='WriteLine' and 'IStringable<T>' in x['detail'] for x in result),result
                count+=1;print('PASS migrated completion '+body,flush=True)
            opened(text('Stopwatch timer; Console.WriteLine(timer); string s=(string)timer; Thread.Sleep(0); Console.Clear();'))
            client.barrier();diagnostics=client.latest('textDocument/publishDiagnostics',source.as_uri())['diagnostics']
            assert not [d for d in diagnostics if d.get('severity')==1],diagnostics
            count+=1;print('PASS Stopwatch implements IStringable and migrated APIs type-check unsaved',flush=True)
            opened(text('Thread.Sleep("bad");'))
            client.barrier();diagnostics=client.latest('textDocument/publishDiagnostics',source.as_uri())['diagnostics']
            assert any(d.get('severity')==1 for d in diagnostics),diagnostics
            count+=1;print('PASS wrong Sleep argument retains compiler diagnostic',flush=True)
        finally:client.stop()
    print(f'{count} migrated API LSP checks passed')
if __name__=='__main__':main()
