#!/usr/bin/env python3
"""Exercise the real stdio server, including unsaved text, UTF-16 and semantic bindings."""
import argparse
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading

def deadline(seconds):
    """Preserve the ordinary deadlines and allow slower emulated targets in CI."""
    factor = float(os.environ.get("BOLT_LSP_TEST_TIMEOUT_SCALE", "1"))
    if not math.isfinite(factor) or factor < 1:
        raise ValueError("BOLT_LSP_TEST_TIMEOUT_SCALE must be a finite value of at least 1")
    return seconds * factor


class Client:
    def __init__(self, bolt):
        self.p = subprocess.Popen([str(bolt), 'lsp'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.inbox = queue.Queue()
        self.history = []
        self.seq = 0
        def read():
            try:
                while True:
                    headers = {}
                    while True:
                        line = self.p.stdout.readline()
                        if not line: raise EOFError('server closed stdout')
                        if line in (b'\r\n', b'\n'): break
                        key, value = line.decode().split(':', 1)
                        headers[key.lower()] = value.strip()
                    body = self.p.stdout.read(int(headers['content-length']))
                    self.inbox.put(json.loads(body))
            except Exception as e:
                self.inbox.put(e)
        threading.Thread(target=read, daemon=True).start()
    def raw(self, body):
        self.p.stdin.write(b'Content-Length: '+str(len(body)).encode()+b'\r\n\r\n'+body)
        self.p.stdin.flush()
    def send(self, method, params=None, id=None):
        message = {'jsonrpc':'2.0', 'method':method}
        if params is not None: message['params'] = params
        if id is not None: message['id'] = id
        self.raw(json.dumps(message, ensure_ascii=False).encode())
    def receive(self):
        item = self.inbox.get(timeout=deadline(15))
        if isinstance(item, Exception): raise item
        self.history.append(item)
        return item
    def request(self, method, params=None):
        self.seq += 1
        self.send(method, params, self.seq)
        while True:
            message = self.receive()
            if message.get('id') == self.seq: return message
    def barrier(self):
        self.request('bolt/testBarrier')
    def latest(self, method, uri=None):
        for item in reversed(self.history):
            if item.get('method') == method and (uri is None or item['params'].get('uri') == uri):
                return item['params']
        raise AssertionError('Notification missing: '+method)
    def stop(self):
        self.request('shutdown')
        self.send('exit')
        self.p.stdin.close()
        assert self.p.wait(timeout=deadline(10)) == 0, self.p.stderr.read().decode()
    def kill(self):
        if self.p.poll() is None: self.p.kill()
        self.p.wait()

def position(text, word, occurrence=0, inner=0):
    start = -1
    for _ in range(occurrence+1): start = text.index(word, start+1)
    start += inner
    prefix = text[:start]
    return {'line':prefix.count('\n'), 'character':len(prefix.rsplit('\n',1)[-1].encode('utf-16-le'))//2}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bolt', required=True, type=Path)
    args = parser.parse_args()
    count = 0
    def passed(name):
        nonlocal count
        count += 1
        print('PASS '+name)
    with tempfile.TemporaryDirectory(prefix='bolt-lsp-') as directory:
        root = Path(directory) / 'project space ü'
        root.mkdir()
        (root/'bolt.project').write_text('{}')
        helper = 'public class Point { public: int X; Point(int x) { X = x; } int Get() { return X; } }\n'
        (root/'Point.bolt').write_text(helper)
        text = ('import Console\nstatic class Program {\npublic:\n    void Main() {\n'
                '        int value = 2;\n        Point point = Point(value);\n'
                '        Console.WriteLine(point.Get());\n    }\n}\n')
        path = root/'Program.bolt'
        path.write_text(text)
        uri = path.as_uri()
        params = {'textDocument':{'uri':uri}}
        c = Client(args.bolt)
        try:
            assert c.request('textDocument/hover',params)['error']['code'] == -32002
            passed('request before initialization rejected')
            init = c.request('initialize', {'processId':None,'rootUri':root.as_uri(),
                    'workspaceFolders':[{'uri':root.as_uri(),'name':'test'}],
                    'initializationOptions':{'boltSemanticNotifications':True},'capabilities':{}})
            assert init['result']['capabilities']['positionEncoding'] == 'utf-16'
            assert 'semanticTokensProvider' in init['result']['capabilities']
            passed('initialize with UTF-16 and semantic token capabilities')
            c.send('initialized',{})
            c.send('textDocument/didOpen', {'textDocument':{'uri':uri,'languageId':'bolt','version':1,'text':text}})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics'] == []
            passed('multi-file project with Unicode/space URI analyzed')
            custom = c.latest('bolt/semanticTokens',uri)
            assert len(custom['textHash']) == 16
            assert any(t['kind']=='variable' for t in custom['tokens'])
            passed('custom semantic notification with stable text hash')
            semantic = c.request('textDocument/semanticTokens/full',params)['result']['data']
            assert len(semantic)%5==0 and len(semantic)>20
            passed('standard semantic tokens framing')
            valuepos = position(text,'value',1,1)
            hover = c.request('textDocument/hover',{**params,'position':valuepos})['result']
            assert hover and 'int value' in hover['contents']['value'], hover
            passed('hover resolves a local variable')
            definition = c.request('textDocument/definition',{**params,'position':valuepos})['result']
            assert definition[0]['uri']==uri and definition[0]['range']['start']==position(text,'value')
            passed('definition resolves the actual local declaration')
            references = c.request('textDocument/references',{**params,'position':valuepos,'context':{'includeDeclaration':True}})['result']
            assert len(references)==2, references
            passed('references are bound symbols, not textual matches')
            typename=c.request('textDocument/definition',{**params,'position':position(text,'Point',0,1)})['result']
            assert typename and typename[0]['uri']==(root/'Point.bolt').as_uri(),typename
            assert typename[0]['range']['start']==position(helper,'Point'),typename
            passed('type annotation resolves to the actual class name, not its modifier')
            getpos = position(text,'Get',0,1)
            target = c.request('textDocument/definition',{**params,'position':getpos})['result']
            assert target and target[0]['uri']==(root/'Point.bolt').as_uri(),target
            passed('cross-file method definition')
            symbols = c.request('textDocument/documentSymbol',params)['result']
            assert {'Program','Main'} <= {s['name'] for s in symbols}
            passed('document symbols expose declarations')
            bad = text.replace('int value = 2;', 'int value = "bad";')
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':2},'contentChanges':[{'text':bad}]})
            c.barrier()
            d = c.latest('textDocument/publishDiagnostics',uri)
            assert d['version']==2 and d['diagnostics'][0]['code']=='BLT2002', d
            assert path.read_text()==text
            passed('unsaved type errors published without changing disk')
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':1},'contentChanges':[{'text':text}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['version']==2
            passed('stale document changes ignored')
            begin=position(bad,'"bad"')
            end=dict(begin,character=begin['character']+5)
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':3},'contentChanges':[{'range':{'start':begin,'end':end},'text':'3'}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            passed('incremental change clears the old diagnostic')
            utf = 'static class Program { public: void Main() { string label = "😀"; int amount = "bad"; } }\r\n'
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':4},'contentChanges':[{'text':utf}]})
            c.barrier()
            diag=c.latest('textDocument/publishDiagnostics',uri)['diagnostics'][0]
            # Compiler diagnoses the initializer, not the variable name.
            assert diag['range']['start']==position(utf,'"bad"'),diag
            passed('UTF-8 source positions converted to UTF-16 after emoji')
            pair=position(utf,'😀')
            middle=dict(pair, character=pair['character']+1)
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':5},'contentChanges':[{'range':{'start':middle,'end':middle},'text':'x'}]})
            c.barrier()
            assert 'surrogate pair' in c.latest('window/logMessage')['message']
            passed('invalid edit splitting a surrogate pair rejected atomically')
            incomplete='static class Program { public: void Main() { Console. } }'
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':6},'contentChanges':[{'text':incomplete}]})
            completion=c.request('textDocument/completion',{**params,'position':position(incomplete,'Console.',inner=8)})['result']
            assert {'WriteLine','ReadLine'} <= {i['label'] for i in completion['items']},completion
            passed('builtin completion while the document is syntactically incomplete')
            incomplete='static class Program { public: void Main() { string s = "unfinished'
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':7},'contentChanges':[{'text':incomplete}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics'][0]['code']=='BLT1002'
            assert any(t['kind']=='string' for t in c.latest('bolt/semanticTokens',uri)['tokens'])
            passed('unterminated string still receives syntax coloring')
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':8},'contentChanges':[{'text':text}]})
            c.barrier()
            # Newly created unsaved source is included by its project scope.
            unsaved=(root/'Extra.bolt').as_uri()
            c.send('textDocument/didOpen',{'textDocument':{'uri':unsaved,'languageId':'bolt','version':1,'text':'public class Extra { public: int Value; }'}})
            c.barrier()
            assert not (root/'Extra.bolt').exists()
            assert c.latest('textDocument/publishDiagnostics',unsaved)['diagnostics']==[]
            passed('new unsaved file included in the project')
            feature=(
                'public interface IValue { int Get(); }\n'
                'class Scalar:IValue,IStringable { public: int Value=3;\n'
                'int Get(){return Value;}\n'
                'string ToString(){return $"Scalar {Value}";}\n'
                'static Scalar operator +(Scalar a,Scalar b){Scalar r;r.Value=a.Value+b.Value;return r;}\n'
                '}\nstatic class Program{public:void Main(){Scalar value;IValue& view=value;Print($"value:{view.Get()}");}}\n')
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':9},'contentChanges':[{'text':feature}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            tokens=c.latest('bolt/semanticTokens',uri)['tokens']
            def colored(word,kind,occurrence=0):
                at=position(feature,word,occurrence)
                return any(t['line']==at['line'] and t['start']==at['character'] and t['kind']==kind for t in tokens)
            assert colored('interface','keyword') and colored('IValue','interface'),tokens
            assert colored('operator','keyword'),tokens
            passed('interface declarations and operator keyword receive semantic coloring')
            assert colored('view','variable',1),tokens
            hover=c.request('textDocument/hover',{**params,'position':position(feature,'view',1,1)})['result']
            assert hover and 'view' in hover['contents']['value'],hover
            passed('interpolated expressions retain local-variable coloring and hover binding')
            symbols=c.request('textDocument/documentSymbol',params)['result']
            assert any(x['name']=='IValue' and x['kind']==11 for x in symbols),symbols
            assert 'interface' in init['result']['capabilities']['semanticTokensProvider']['legend']['tokenTypes']
            passed('standard interface symbol kind and token legend')
            definition=c.request('textDocument/definition',{**params,'position':position(feature,'IValue',2,1)})['result']
            assert definition and definition[0]['range']['start']==position(feature,'IValue'),definition
            passed('interface type annotation navigates to its declaration')
            broken=feature.replace('{view.Get()}', '{Missing}')
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':10},'contentChanges':[{'text':broken}]})
            c.barrier()
            diagnostic=c.latest('textDocument/publishDiagnostics',uri)['diagnostics'][0]
            assert diagnostic['range']['start']==position(broken,'Missing'),diagnostic
            passed('interpolation diagnostic points to the original expression in the editor')
            numeric_types=('byte','short','ushort','int','uint','long','ulong','float','double')
            version=11
            for index,type_name in enumerate(numeric_types):
                expression=type_name+'.'+('', 'Ma', 'Par', 'Try')[index%4]
                incomplete='static class Program { public: void Main() { '+expression+' } }'
                c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':incomplete}]})
                version+=1
                completion=c.request('textDocument/completion',{**params,'position':position(incomplete,expression,inner=len(expression))})['result']
                suggestions={item['label']:item for item in completion['items']}
                assert suggestions.keys()=={'MinValue','MaxValue','Parse','TryParse'},completion
                canonical={'int':'int','uint':'uint'}.get(type_name,type_name)
                assert all(suggestions[name]['kind']==21 and suggestions[name]['detail']==canonical+' constant' for name in ('MinValue','MaxValue')),completion
                for name,detail in (('Parse',canonical+' Parse(string text)'),('TryParse','bool TryParse(string text, '+canonical+'& value)')):
                    assert suggestions[name]['kind']==2 and suggestions[name]['detail']==detail,(type_name,suggestions[name])
            passed('numeric constant and parsing completions cover all primitive widths and partial names')
            for type_name in ('bool','string'):
                incomplete='static class Program { public: void Main() { '+type_name+'. } }'
                c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':incomplete}]})
                version+=1
                completion=c.request('textDocument/completion',{**params,'position':position(incomplete,type_name+'.',inner=len(type_name)+1)})['result']
                assert not {'MinValue','MaxValue','Parse','TryParse'} & {item['label'] for item in completion['items']},completion
            passed('non-numeric primitives do not advertise numeric constants or parsing methods')
            limits='static class Program { public: void Main() {\n'+''.join(
                'Print('+type_name+'.'+member+');\n'
                for type_name in numeric_types for member in ('MinValue','MaxValue'))+'} }'
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':limits}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            tokens=c.latest('bolt/semanticTokens',uri)['tokens']
            for type_name in numeric_types:
                for member in ('MinValue','MaxValue'):
                    at=position(limits,type_name+'.'+member,inner=len(type_name)+1)
                    assert any(t['line']==at['line'] and t['start']==at['character'] and t['kind']=='property' for t in tokens),(type_name,member,tokens)
            passed('numeric limits are recognized and colored as read-only properties')
            parsing='static class Program { public: void Main() {\n'+''.join(
                'var parsed_'+type_name+'_value = '+type_name+'.Parse("42");\n'
                'var valid_'+type_name+'_value = '+type_name+'.TryParse("43", parsed_'+type_name+'_value);\n'
                'Print(parsed_'+type_name+'_value); Print(valid_'+type_name+'_value);\n'
                for type_name in numeric_types)+'} }'
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':parsing}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            tokens=c.latest('bolt/semanticTokens',uri)['tokens']
            for type_name in numeric_types:
                canonical={'int':'int','uint':'uint'}.get(type_name,type_name)
                for method,signature in (('Parse',canonical+' Parse(string text)'),('TryParse','bool TryParse(string text, '+canonical+'& value)')):
                    expression=type_name+'.'+method+'('
                    at=position(parsing,expression,inner=len(type_name)+1)
                    assert any(t['line']==at['line'] and t['start']==at['character'] and t['kind']=='method' for t in tokens),(type_name,method,tokens)
                    receiver=position(parsing,expression)
                    assert any(t['line']==receiver['line'] and t['start']==receiver['character'] and t['kind']=='type' for t in tokens),(type_name,tokens)
                    hover=c.request('textDocument/hover',{**params,'position':at})['result']
                    assert hover and signature in hover['contents']['value'],(type_name,method,hover)
                    help=c.request('textDocument/signatureHelp',{**params,'position':position(parsing,expression,inner=len(expression))})['result']
                    assert help and help['signatures'][0]['label']==signature and help['activeParameter']==0,(type_name,method,help)
                help=c.request('textDocument/signatureHelp',{**params,'position':position(parsing,'"43", parsed_'+type_name+'_value',inner=6)})['result']
                assert help and help['activeParameter']==1 and help['signatures'][0]['parameters'][1]['label']==canonical+'& value',(type_name,help)
                for prefix,result_type in (('parsed_',canonical),('valid_','bool')):
                    name=prefix+type_name+'_value'
                    for occurrence in (0,1):
                        hover=c.request('textDocument/hover',{**params,'position':position(parsing,name,occurrence)})['result']
                        assert hover and result_type+' '+name in hover['contents']['value'],(name,hover)
            passed('numeric parsing methods retain type coloring, method hover, argument signatures and inferred variable types')
            completion=c.request('textDocument/completion',{**params,'position':position(parsing,'Print',len(numeric_types)*2-1)})['result']
            suggestions={item['label']:item for item in completion['items']}
            for type_name in numeric_types:
                canonical={'int':'int','uint':'uint'}.get(type_name,type_name)
                assert suggestions['parsed_'+type_name+'_value']['detail']==canonical+' parsed_'+type_name+'_value',suggestions
                assert suggestions['valid_'+type_name+'_value']['detail']=='bool valid_'+type_name+'_value',suggestions
            passed('completion infers numeric Parse and boolean TryParse results for var locals')
            inferred=(
                'import System.Collections.Generic\n'
                'static class Program { public: void Main() {\n'
                '    var value = 0;\n'
                '    var point = Point(value);\n'
                '    var items = List<int>();\n'
                '    items.Add(point.Get());\n'
                '    for (var index = 0; index < 1; index++) { Print(items[index]); }\n'
                '} }\n')
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':inferred}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            tokens=c.latest('bolt/semanticTokens',uri)['tokens']
            for occurrence in range(4):
                at=position(inferred,'var',occurrence)
                assert any(t['line']==at['line'] and t['start']==at['character'] and t['length']==3 and t['kind']=='keyword' for t in tokens),tokens
            passed('var declarations and for initializers receive keyword coloring')
            inferred_types={'value':'int','point':'Point','items':'System.Collections.Generic.List<int>','index':'int'}
            for name,type_name in inferred_types.items():
                for occurrence in (0,1):
                    at=position(inferred,name,occurrence)
                    assert any(t['line']==at['line'] and t['start']==at['character'] and t['kind']=='variable' for t in tokens),(name,tokens)
                    hover=c.request('textDocument/hover',{**params,'position':at})['result']
                    assert hover and type_name+' '+name in hover['contents']['value'],(name,hover)
                definition=c.request('textDocument/definition',{**params,'position':position(inferred,name,1)})['result']
                assert definition and definition[0]['uri']==uri and definition[0]['range']['start']==position(inferred,name),(name,definition)
            passed('inferred primitive, class, collection and loop locals retain hover types and definition bindings')
            completion=c.request('textDocument/completion',{**params,'position':position(inferred,'var')})['result']
            suggestions={item['label']:item for item in completion['items']}
            assert suggestions['var']['kind']==14,suggestions['var']
            assert not inferred_types.keys() & suggestions.keys(),suggestions
            completion=c.request('textDocument/completion',{**params,'position':position(inferred,'Print')})['result']
            suggestions={item['label']:item for item in completion['items']}
            for name,type_name in inferred_types.items():
                assert suggestions[name]['detail'] in (type_name+' '+name,type_name.rsplit('.',1)[-1]+' '+name),suggestions[name]
            passed('completion includes var and inferred local types only after their declarations')
            definition=c.request('textDocument/definition',{**params,'position':position(inferred,'var',1)})['result']
            assert definition==[],definition
            passed('var keyword is not bound as an explicit class type annotation')
            constants=(
                'import Math\n'
                'const T GlobalPi<T> : FloatingPoint = (T)Math.Pi;\n'
                'static class Constants { public:\n'
                '    const T CirclePi<T> : FloatingPoint = (T)Math.Pi;\n'
                '    const T CircleTau<T> : FloatingPoint = (T)Math.Tau; }\n'
                'static class Program { public: void Main() {\n'
                '    Print(Math.Pi<float>); Print(Math.E<double>); Print(Math.Tau);\n'
                '    Print(Constants.CirclePi<float>); Print(Constants.CircleTau<double>); Print(GlobalPi<double>);\n'
                '    var inferredMath = Math.Pi<float>; var inferredCustom = Constants.CirclePi<double>;\n'
                '    var inferredGlobal = GlobalPi<float>; Print(inferredMath); Print(inferredCustom); Print(inferredGlobal);\n'
                '} }\n')
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':constants}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            for expression,detail in (('Math.Pi<float>','const float Math.Pi<float>'),('Math.E<double>','const double Math.E<double>'),('Math.Tau','const double Math.Tau')):
                hover=c.request('textDocument/hover',{**params,'position':position(constants,'Print('+expression+')',inner=11)})['result']
                assert hover and detail in hover['contents']['value'],(expression,hover)
            passed('Math constant hover reports explicit types and the default double type')
            for name,type_name in (('CirclePi','float'),('CircleTau','double'),('GlobalPi','double')):
                use=position(constants,name,1)
                hover=c.request('textDocument/hover',{**params,'position':use})['result']
                assert hover and 'const '+type_name+' '+name in hover['contents']['value'],(name,hover)
                definition=c.request('textDocument/definition',{**params,'position':use})['result']
                assert definition and definition[0]['uri']==uri and definition[0]['range']['start']==position(constants,name),(name,definition)
            passed('generic constant hover and definition preserve specialized values and original declarations')
            symbols=c.request('textDocument/documentSymbol',params)['result']
            for name in ('CirclePi','CircleTau','GlobalPi'):
                symbol=next(item for item in symbols if item['name']==name)
                assert symbol['kind']==14,symbol
            completion=c.request('textDocument/completion',{**params,'position':position(constants,'Constants.CirclePi',inner=10)})['result']
            for name in ('CirclePi','CircleTau'):
                suggestion=next(item for item in completion['items'] if item['label']==name)
                assert suggestion['kind']==21 and 'const T '+name+'<T>' in suggestion['detail'] and '(' not in suggestion['detail'],suggestion
            completion=c.request('textDocument/completion',{**params,'position':position(constants,'Math.Pi<float>',inner=5)})['result']
            suggestions={item['label']:item for item in completion['items']}
            for name in ('Pi','E','Tau'):
                suggestion=suggestions[name]
                assert suggestion['kind']==21 and '<float>' in suggestion['detail'] and '<double>' in suggestion['detail'],suggestion
            passed('generic constant symbols and completion use constant kinds and value syntax')
            completion=c.request('textDocument/completion',{**params,'position':position(constants,'Print(inferredMath)')})['result']
            suggestions={item['label']:item for item in completion['items']}
            for name,type_name in (('inferredMath','float'),('inferredCustom','double'),('inferredGlobal','float')):
                assert name in suggestions and type_name+' '+name in suggestions[name]['detail'],(name,suggestions.get(name))
            passed('completion infers local types from typed Math and user-defined generic constants')
            instance=(
                'class Counter { public: int Value;\n'
                'Counter(int Value) { this.Value = Value; }\n'
                'Counter& Self() { return this; }\n'
                'int Read() { return this.Value; }\n'
                'string Describe() { return $"Value: {this.Value}"; }\n'
                'int ReadInferred() { var self = this; var value = this.Value; return self.Value + value; }\n'
                '}\nstatic class Program { public: void Main() { Counter counter = Counter(3); Print(counter.Self().Read()); } }\n')
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':instance}]})
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            tokens=c.latest('bolt/semanticTokens',uri)['tokens']
            for occurrence in range(6):
                at=position(instance,'this',occurrence)
                assert any(t['line']==at['line'] and t['start']==at['character'] and t['length']==4 and t['kind']=='keyword' for t in tokens),tokens
                hover=c.request('textDocument/hover',{**params,'position':at})['result']
                assert hover and 'Counter& this' in hover['contents']['value'],hover
            passed('this receives keyword coloring and current-instance hover in constructors, methods and interpolation')
            field_at=position(instance,'this.Value',inner=5)
            definition=c.request('textDocument/definition',{**params,'position':field_at})['result']
            assert definition and definition[0]['range']['start']==position(instance,'Value'),definition
            argument_at=position(instance,'= Value',inner=2)
            definition=c.request('textDocument/definition',{**params,'position':argument_at})['result']
            assert definition and definition[0]['range']['start']==position(instance,'Value',1),definition
            passed('this.field resolves the field while a matching bare name resolves the constructor parameter')
            for name,type_name in (('self','Counter'),('value','int')):
                for occurrence in (0,1):
                    hover=c.request('textDocument/hover',{**params,'position':position(instance,name,occurrence)})['result']
                    assert hover and type_name+' '+name in hover['contents']['value'],(name,hover)
                definition=c.request('textDocument/definition',{**params,'position':position(instance,name,1)})['result']
                assert definition and definition[0]['range']['start']==position(instance,name),(name,definition)
            passed('locals inferred from this and this.field retain their types and definition bindings')
            completion=c.request('textDocument/completion',{**params,'position':position(instance,'this')})['result']
            suggestion=next(item for item in completion['items'] if item['label']=='this')
            assert suggestion['kind']==14 and 'Current class instance' in suggestion['detail'],suggestion
            passed('this completion describes the current class instance')
            static_instance='static class Program { public: void Main() { Print(this); } }'
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':static_instance}]})
            c.barrier()
            diagnostics=c.latest('textDocument/publishDiagnostics',uri)['diagnostics']
            assert diagnostics and diagnostics[0]['code']=='BLT2000' and diagnostics[0]['range']['start']==position(static_instance,'this'),diagnostics
            passed('this outside an instance method produces an editor diagnostic at the keyword')
            defaults=(
                'class DefaultHolder { public: int Count = default; int Read() { return default; } }\n'
                'static class Program { public: void Main() {\n'
                '    int count = default; bool enabled = default; string label = default;\n'
                '    int* pointer = default; DefaultHolder holder = default;\n'
                '    int[] values = {default, 1}; count = default; values[1] = default;\n'
                '} }\n')
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':defaults}]})
            c.barrier()
            diagnostics=c.latest('textDocument/publishDiagnostics',uri)['diagnostics']
            assert diagnostics==[],diagnostics
            tokens=c.latest('bolt/semanticTokens',uri)['tokens']
            default_types=('int','int','int','bool','string','int*','DefaultHolder','int','int','int')
            for occurrence,type_name in enumerate(default_types):
                at=position(defaults,'default',occurrence)
                assert any(t['line']==at['line'] and t['start']==at['character'] and t['length']==7 and t['kind']=='keyword' for t in tokens),tokens
                hover=c.request('textDocument/hover',{**params,'position':at})['result']
                assert hover and type_name+' default' in hover['contents']['value'],(type_name,hover)
                definition=c.request('textDocument/definition',{**params,'position':at})['result']
                assert definition==[],definition
            passed('default keyword hover reports destination types in fields, returns, locals, arrays and assignments')
            completion=c.request('textDocument/completion',{**params,'position':position(defaults,'default')})['result']
            suggestion=next(item for item in completion['items'] if item['label']=='default')
            assert suggestion['kind']==14 and 'destination type' in suggestion['detail'],suggestion
            passed('default completion describes the destination-type requirement')
            untyped_default='static class Program { public: void Main() { var value = default; } }'
            version+=1
            c.send('textDocument/didChange',{'textDocument':{'uri':uri,'version':version},'contentChanges':[{'text':untyped_default}]})
            c.barrier()
            diagnostics=c.latest('textDocument/publishDiagnostics',uri)['diagnostics']
            assert diagnostics and diagnostics[0]['range']['start']==position(untyped_default,'default') and 'default' in diagnostics[0]['message'],diagnostics
            passed('default without a destination type produces an editor diagnostic at the keyword')
            response=c.request('textDocument/hover',{'textDocument':{'uri':'https://example.invalid/code.bolt'},'position':{'line':0,'character':0}})
            assert response['error']['code']==-32602
            passed('remote URI rejected')
            c.raw(b'{not-json}')
            while True:
                msg=c.receive()
                if msg.get('error',{}).get('code')==-32700:break
            passed('malformed JSON reported without killing the server')
            c.send('textDocument/didClose',params)
            c.barrier()
            assert c.latest('textDocument/publishDiagnostics',uri)['diagnostics']==[]
            passed('close clears diagnostics')
            comments = '/* active\n * continuation\n*/\n#if BOLT_NEVER_DEFINED\n// inactive\n/* inactive */\n#endif\n'
            comments_uri = (root/'Comments.bolt').as_uri()
            c.send('textDocument/didOpen', {'textDocument': {'uri':comments_uri, 'languageId':'bolt', 'version':1, 'text':comments}})
            legend = init['result']['capabilities']['semanticTokensProvider']['legend']
            data = c.request('textDocument/semanticTokens/full', {'textDocument':{'uri':comments_uri}})['result']['data']
            inactive_bit = 1 << legend['tokenModifiers'].index('boltInactive')
            line, flagged = 0, {}
            for delta, _, _, kind, modifiers in zip(*[iter(data)]*5):
                line += delta
                if legend['tokenTypes'][kind] == 'comment':
                    flagged[line] = bool(modifiers & inactive_bit)
            assert flagged == {0:False, 1:False, 2:False, 4:True, 5:True}, flagged
            passed('semantic modifiers distinguish multiline active comments and disabled comment-only lines')
            c.stop()
            passed('shutdown and exit lifecycle')
        finally:
            c.kill()
    # Hard frame limits and truncation must not hang.
    for content in (b'Content-Length: 999999999\r\n\r\n', b'Content-Length: 10\r\n\r\n{}', b'Content-Length: -1\r\n\r\n'):
        r=subprocess.run([str(args.bolt),'lsp'],input=content,capture_output=True,timeout=deadline(5))
        assert r.returncode!=0
    passed('oversized, truncated and negative frame sizes rejected')
    print(f'{count} language-server checks passed')

if __name__=='__main__':main()
