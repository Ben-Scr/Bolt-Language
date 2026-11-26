#!/usr/bin/env python3
"""Real stdio LSP checks for new library and language completion contexts."""
import argparse
from pathlib import Path
import tempfile
from lsp_protocol import Client

def main():
    p=argparse.ArgumentParser();p.add_argument('--bolt',type=Path,required=True);a=p.parse_args();count=0
    with tempfile.TemporaryDirectory(prefix='bolt-library-editor-') as tmp:
        r=Path(tmp);(r/'bolt.project').write_text('{}');f=r/'Program.bolt';f.write_text('static class Program{public:void Main(){}}')
        client=Client(a.bolt);version=0
        client.request('initialize',{'processId':None,'rootUri':r.as_uri(),'capabilities':{}});client.send('initialized',{})
        def items(body,declarations=''):
            nonlocal version
            text='import System\nimport System.IO\nimport System.Reflection\nimport System.Threading.Tasks\nimport Ecs\npublic struct Position{public:int X;}\n'+declarations+'\nstatic class Program{public:T Add<T>(T... values):Addable<T>{return values[0];}void Main(){'+body+'}}'
            at=text.index('|');text=text.replace('|','');pos={'line':text[:at].count('\n'),'character':len(text[:at].rsplit('\n',1)[-1].encode('utf-16-le'))//2};version+=1
            if version==1:client.send('textDocument/didOpen',{'textDocument':{'uri':f.as_uri(),'languageId':'bolt','version':version,'text':text}})
            else:client.send('textDocument/didChange',{'textDocument':{'uri':f.as_uri(),'version':version},'contentChanges':[{'text':text}]})
            ret=client.request('textDocument/completion',{'textDocument':{'uri':f.as_uri()},'position':pos})
            return ret.get('result',{}).get('items',[])
        cases=[
            ('File.|',{'Create','ReadAllText','ReadAllTextAsync','WriteAllText','Delete'}),
            ('File.Create("x").|',{'Read','Write','Close','Length'}),
            ('File.Open("x").|',{'Seek','Dispose'}),
            ('string value="x";value.|',{'Length','Trim','Replace','ToUpper','Insert','Remove'}),
            ('string.|',{'FromUtf8','Join','IsNullOrWhiteSpace'}),
            ('Random value;value.|',{'Next','NextBytes','NextBool'}),
            ('Task<int> value;value.|',{'Result','Wait','IsCompleted'}),
            ('Position[] values;for(value : values){value.|}',{'X'}),
            ('Registry registry;for(row : registry.Query<Position>()){row.|}',{'Get','Entity'}),
            ('try{}catch(IOException& error){error.|}',{'Message','SourceLine','ToString'}),
            ('Stopwatch timer;timer.|',{'Start','Pause','Unpause','Reset','Elapsed'}),
            ('Json.|',{'Serialize','Deserialize','SerializeBytes','SerializeBinary','DeserializeBinary'}),
        ]
        try:
            for body,expected in cases:
                result=items(body);names={v['label'] for v in result};assert expected<=names,(body,expected-names,result)
                count+=1;print('PASS completion: '+body,flush=True)
            result=items('Program.|');method=next(v for v in result if v['label']=='Add')
            assert 'T... values' in method['detail'] and 'Addable<T>' in method['detail'],method
            count+=1;print('PASS explicit type constraint and variadic signature',flush=True)
            packs='class PackContainer<T...>{public:void Own<U...>(U... values){} void Shadow<T...>(T... values){} void ClassPack(T... values){} Action<T...> Callback; Action<T...> Property {get{return Callback;}} Action<T...> Get(){return Callback;}}'
            result={item['label']:item for item in items('PackContainer<int,string> value;value.|',packs)}
            assert result['Own']['detail']=='void Own<U...>(U... values)',result['Own']
            assert result['Shadow']['detail']=='void Shadow<T...>(T... values)',result['Shadow']
            assert 'int values0' in result['ClassPack']['detail'] and 'string values1' in result['ClassPack']['detail'],result['ClassPack']
            count+=1;print('PASS class and method type packs retain separate scopes',flush=True)
            for expression,expected in [('value.Callback.',2),('value.Property.',2),('value.Get().',2)]:
                result=items('PackContainer<int,string> value;'+expression+'|',packs)
                invoke=next(item for item in result if item['label']=='Invoke')
                assert 'int values0' in invoke['detail'] and 'string values1' in invoke['detail'],invoke
                assert len(invoke['data']['bolt']['parameters'])==expected,invoke
                count+=1;print('PASS expanded class-pack member type '+expression,flush=True)
            invoke=next(item for item in items('PackContainer<> value;value.Callback.|',packs) if item['label']=='Invoke')
            assert invoke['detail']=='void Invoke()',invoke
            count+=1;print('PASS empty class pack flows through member types',flush=True)
            factories='Action<T...> MakePack<T...>(){Action<T...> value;return value;} Action<T...> InferPack<T...>(T... values){Action<T...> result;return result;} Action<T...> Repack<T...>(Action<T...> value){return value;}'
            for expression,expected in [('MakePack<int,string>().',2),('MakePack<>().',0),('InferPack(4,"typed").',2)]:
                invoke=next(item for item in items(expression+'|',factories) if item['label']=='Invoke')
                assert len(invoke['data']['bolt']['parameters'])==expected,invoke
                if expected:assert 'int values0' in invoke['detail'] and 'string values1' in invoke['detail'],invoke
                count+=1;print('PASS generic method return pack '+expression,flush=True)
            invoke=next(item for item in items('Action<int,string> callback;Repack(callback).|',factories) if item['label']=='Invoke')
            assert 'int values0' in invoke['detail'] and 'string values1' in invoke['detail'],invoke
            count+=1;print('PASS method return pack inferred from generic parameter type',flush=True)
            for expression,types in [
                ('InferPack(1u,2l,3ul,4f,5.0).',['uint','long','ulong','float','double']),
                ('InferPack(2147483648,4294967296u,9223372036854775808,-2147483648).',['long','ulong','ulong','int']),
            ]:
                invoke=next(item for item in items(expression+'|',factories) if item['label']=='Invoke')
                assert invoke['data']['bolt']['parameters']==[f'{type} values{index}' for index,type in enumerate(types)],invoke
                count+=1;print('PASS exact numeric types in inferred return pack '+expression,flush=True)
            result=items('File.|');method=next(v for v in result if v['label']=='Open')
            assert ' = FileMode.Open' in method['detail'] and ' = FileAccess.ReadWrite' in method['detail'],method
            count+=1;print('PASS native defaults in method signature',flush=True)
            result=items('Task<int> value;value.|');method=next(v for v in result if v['label']=='Result')
            assert 'int' in method['detail'],method
            count+=1;print('PASS generic task member specialization',flush=True)
        finally:client.stop()
    print(f'{count} library LSP checks passed')
if __name__=='__main__':main()
