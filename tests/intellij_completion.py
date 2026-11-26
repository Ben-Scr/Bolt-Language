#!/usr/bin/env python3
"""Current IntelliJ contract regression tests against a real native Bolt language server."""
import argparse
from pathlib import Path
import tempfile
from lsp_protocol import Client, position

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt',required=True,type=Path)
    args=parser.parse_args()
    count=0
    failures=[]
    def check(name, action):
        nonlocal count
        try: action()
        except (AssertionError,KeyError) as error:
            failures.append(name);print('FAIL',name,repr(error),flush=True)
        else: count+=1;print('PASS',name,flush=True)
    with tempfile.TemporaryDirectory(prefix='bolt-idea-members-') as temp:
        root=Path(temp);(root/'bolt.project').write_text('{}')
        helper='''class Value { public: int X; }
class Other { public: string Name; }
class Base<T> {public: T Data; T Get(){return Data;} }
class Derived<U> : Base<U> {public: U More;}
class Sample {public: int Public; static int Shared; int Size(){return 1;} private: int Secret;}
'''
        (root/'Types.bolt').write_text(helper)
        path=root/'Program.bolt';uri=path.as_uri();params={'textDocument':{'uri':uri}}
        path.write_text('static class Program {public: void Main(){} }')
        c=Client(args.bolt.resolve());version=0;text=''
        c.request('initialize',{'processId':None,'rootUri':root.as_uri(),'capabilities':{}})
        c.send('initialized',{})
        def change(source):
            nonlocal version,text
            caret=source.index('|') if '|' in source else len(source)
            text=source.replace('|','');version+=1
            if version==1:
                c.send('textDocument/didOpen',{'textDocument':{'uri':uri,'languageId':'bolt','version':version,'text':text}})
            else:
                c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':text}]})
            return {'line':text[:caret].count('\n'),'character':len(text[:caret].rsplit('\n',1)[-1].encode('utf-16-le'))//2}
        def program(body):
            return '''import System.Collections.Generic
static class Program {public:
T Echo<T>(T value){return value;}
Value Select(int n){Value v;return v;}
Other Select(string s){Other v;return v;}
void Main(){'''+body+'\n} }'
        def items(source):
            caret=change(source);resp=c.request('textDocument/completion',{**params,'position':caret})
            assert 'error' not in resp,resp
            return resp['result']['items']
        def names(source):return {it['label'] for it in items(source)}
        def has(source, expected, forbidden=()):
            result=names(source)
            assert set(expected)<=result and not set(forbidden)&result,result
        try:
            check('private members are available through this inside their declaring class',lambda:has('class Self {private: int Hidden; public: void Run(){this.|} }',['Hidden','Run']))
            check('private members excluded through another class',lambda:has(program('Sample s; s.|'),['Public','Size'],['Secret','Shared']))
            check('reference receiver preserves public members',lambda:has(program('Sample s; Sample& r = s; r.|'),['Public','Size'],['Secret','Shared']))
            check('arrow completion resolves pointer element type',lambda:has(program('Sample s; Sample* p = &s; p->|'),['Public','Size'],['Secret','Shared']))
            check('generic class inheritance substitutes base type parameters',lambda:has(program('Derived<Value> d; d.Data.|'),['X'],['Name']))
            check('generic return inferred from value argument',lambda:has(program('Value v; Echo(v).|'),['X'],['Name']))
            check('overload receiver uses string argument type',lambda:has(program('Select("value").|'),['Name'],['X']))
            check('overload receiver uses numeric argument type',lambda:has(program('Select(1).|'),['X'],['Name']))
            check('overloaded signatures survive native JSON transport',lambda: (
                None if len([x for x in items(program('Program.|')) if x['label']=='Select'])==2 else (_ for _ in ()).throw(AssertionError('lost overload'))))
            check('members inside string interpolation',lambda:has(program('Sample s; Print($"result {s.|}");'),['Public','Size'],['Secret']))
            check('local inner scope shadows outer receiver type',lambda:has(program('Value value; {Other value; value.|}'),['Name'],['X']))
            def screenshot():
                source='''import System;
public static class Program {
    public static void Main() {
        Console.WriteLine("Hello World");
        Console.WriteLine($"Cross sum {CrossSum(24)}");
    }
    public static int CrossSum(int n) {
        int cSum = 0;
        while(n > 0){ cSum += n % 10; n /= 10; }
        return cSum;
    }
}'''
                change(source)
                result=c.request('textDocument/semanticTokens/full',params)['result']['data']
                caps=c.initialized if hasattr(c,'initialized') else None
                legend=['namespace','type','class','enum','typeParameter','parameter','variable','property','enumMember','function','method','keyword','string','number','comment','operator','interface','identifier','struct','field','character']
                tokens={};line=col=0
                for i in range(0,len(result),5):
                    dl,dc,n,k,_=result[i:i+5];line+=dl;col=col+dc if dl==0 else dc;tokens[(line,col)]=(n,legend[k])
                for word,occ,kind in [('Program',0,'class'),('Main',0,'method'),('Console',0,'class'),('WriteLine',0,'method'),('CrossSum',0,'method'),('CrossSum',1,'method'),('n',0,'parameter'),('cSum',0,'variable'),('24',0,'number')]:
                    # Find n as a token, not within import/Console.
                    if word=='n': at=source.index('int n')+4;pre=source[:at];pos={'line':pre.count('\n'),'character':len(pre.rsplit('\n',1)[-1])}
                    else:pos=position(source,word,occ)
                    assert tokens[(pos['line'],pos['character'])]==(len(word),kind),(word,tokens.get((pos['line'],pos['character'])))
            check('screenshot sample has correct semantic categories including interpolation',screenshot)
        finally:
            c.request("shutdown",None);c.send("exit",None);c.p.wait(timeout=5)
    print(f'{count} IntelliJ contract checks passed; {len(failures)} failed')
    return 1 if failures else 0
if __name__=='__main__':raise SystemExit(main())
