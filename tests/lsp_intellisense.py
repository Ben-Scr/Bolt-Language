#!/usr/bin/env python3
"""Exercise editor IntelliSense against the real Bolt stdio language server.

The buffers deliberately include unfinished expressions, as they would while
typing in an editor. No source is saved after didOpen.
"""
import argparse
from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import uuid

from lsp_protocol import Client, deadline, position


HELPER = '''public interface IValue { int Read(); }
public enum Shade { Red, Blue }
public class Widget : IValue {
public:
    int Value;
    int Read() { return Value; }
    int Add(int amount) { return Value + amount; }
    int Add(int left, int right) { return Value + left + right; }
    static int Twice(int value) { return value * 2; }
private:
    int Secret;
    int Hide() { return Secret; }
}
public class Unrelated {
public:
    int OtherField;
    int OtherMethod() { return OtherField; }
}
'''


def program(body):
    return 'import Math\nstatic class Program { public: void Main() {\n' + body + '\n} }\n'


@contextmanager
def fixture_directory(parent):
    # Python 3.13 creates TemporaryDirectory with an owner-only Windows ACL.
    # Explicitly inherit the selected test directory's ACL so restricted build
    # runners can access fixtures too. Delete only this new, uniquely named child.
    parent = Path(parent or tempfile.gettempdir()).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = parent / ('bolt-intellisense-' + uuid.uuid4().hex)
    root.mkdir()
    try:
        yield root
    finally:
        resolved = root.resolve()
        if root.is_symlink() or resolved.parent != parent or resolved != root:
            raise RuntimeError('Refusing to remove a fixture outside the test directory')
        shutil.rmtree(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', required=True, type=Path)
    parser.add_argument('--temp-root', type=Path)
    args = parser.parse_args()
    count = 0
    failures = []

    def check(name, test):
        nonlocal count
        try:
            test()
        except AssertionError as error:
            failures.append(name)
            print('FAIL ' + name + ': ' + str(error), flush=True)
        else:
            count += 1
            print('PASS ' + name, flush=True)

    def initial_utf8_preamble():
        def frame(message):
            body = json.dumps(message, ensure_ascii=False).encode('utf-8')
            return b'Content-Length: ' + str(len(body)).encode('ascii') + b'\r\n\r\n' + body

        initialize = frame({'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
                            'params': {'processId': None, 'capabilities': {}}})
        initialized = frame({'jsonrpc': '2.0', 'method': 'initialized', 'params': {}})
        shutdown = frame({'jsonrpc': '2.0', 'id': 2, 'method': 'shutdown'})
        exit_message = frame({'jsonrpc': '2.0', 'method': 'exit'})
        # .NET Framework's redirected Console.InputEncoding can prefix the first
        # request with a UTF-8 preamble. Later frames still use ordinary LSP framing.
        result = subprocess.run([str(args.bolt.resolve()), 'lsp'],
                                input=b'\xef\xbb\xbf' + initialize + initialized + shutdown + exit_message,
                                capture_output=True, timeout=deadline(5))
        assert result.returncode == 0, (result.returncode, result.stderr.decode('utf-8', 'replace'))
        output = result.stdout
        responses = []
        while output:
            assert output.startswith(b'Content-Length: '), output[:80]
            header, separator, remaining = output.partition(b'\r\n\r\n')
            assert separator, output[:80]
            headers = dict(line.decode('ascii').split(': ', 1) for line in header.split(b'\r\n'))
            length = int(headers['Content-Length'])
            assert len(remaining) >= length, (length, len(remaining))
            responses.append(json.loads(remaining[:length].decode('utf-8')))
            output = remaining[length:]
        assert [response.get('id') for response in responses] == [1, 2], responses
        assert 'completionProvider' in responses[0]['result']['capabilities'], responses[0]
        assert responses[1]['result'] is None, responses[1]
        # Accepting a transport preamble must not silently change later headers.
        result = subprocess.run([str(args.bolt.resolve()), 'lsp'],
                                input=initialize + b'\xef\xbb\xbf' + shutdown + exit_message,
                                capture_output=True, timeout=deadline(5))
        assert result.returncode != 0, 'A BOM on a later header was incorrectly accepted'
    check('initial .NET UTF-8 preamble is accepted without emitting a BOM or changing later framing', initial_utf8_preamble)

    with fixture_directory(args.temp_root) as directory:
        root = Path(directory)
        (root / 'bolt.project').write_text('{}', encoding='utf-8')
        (root / 'Widget.bolt').write_text(HELPER, encoding='utf-8')
        path = root / 'Program.bolt'
        original = program('Widget widget; Print(widget.Read());')
        path.write_text(original, encoding='utf-8')
        uri = path.as_uri()
        params = {'textDocument': {'uri': uri}}
        c = Client(args.bolt.resolve())
        version = 1
        text = original
        try:
            initialized = c.request('initialize', {
                'processId': None, 'rootUri': root.as_uri(),
                'workspaceFolders': [{'uri': root.as_uri(), 'name': 'IntelliSense'}],
                'initializationOptions': {'boltSemanticNotifications': True},
                'capabilities': {'workspace': {'workspaceEdit': {'documentChanges': True}},
                                 'textDocument': {'completion': {
                    'completionItem': {'snippetSupport': True}}}}})
            capabilities = initialized['result']['capabilities']
            c.send('initialized', {})
            c.send('textDocument/didOpen', {'textDocument': {
                'uri': uri, 'languageId': 'bolt', 'version': version, 'text': text}})
            c.barrier()

            def diagnostics():
                notification = c.latest('textDocument/publishDiagnostics', uri)
                assert notification['version'] == version, notification
                return notification['diagnostics']

            def change(source, wait_for_check=True):
                nonlocal version, text
                assert source.count('|') <= 1, 'Use at most one caret marker'
                before = source.split('|', 1)[0]
                caret = {'line': before.count('\n'),
                         'character': len(before.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}
                text = source.replace('|', '')
                version += 1
                c.send('textDocument/didChange', {'textDocument': {
                    'uri': uri, 'version': version}, 'contentChanges': [{'text': text}]})
                if wait_for_check:
                    c.barrier()
                assert path.read_text(encoding='utf-8') == original, 'LSP modified the source on disk'
                return caret

            def complete(source):
                caret = change(source, wait_for_check=False)
                response = c.request('textDocument/completion', {**params, 'position': caret})
                c.barrier()
                assert 'error' not in response, response
                result = response['result']
                items = result['items'] if isinstance(result, dict) else result
                assert isinstance(items, list), result
                return {item['label']: item for item in items}

            def tokens():
                notification = c.latest('bolt/semanticTokens', uri)
                assert notification['version'] == version, notification
                return notification['tokens']

            def kind(word, occurrence=0):
                at = position(text, word, occurrence)
                return next((token['kind'] for token in tokens()
                             if token['line'] == at['line'] and token['start'] == at['character']
                             and token['length'] == len(word)), None)

            def no_errors():
                assert diagnostics() == [], diagnostics()

            def initial_capabilities():
                assert '.' in capabilities['completionProvider']['triggerCharacters'], capabilities
                assert 'signatureHelpProvider' in capabilities, capabilities
                assert {'(', ','} <= set(capabilities['signatureHelpProvider']['triggerCharacters']), capabilities
                assert 'identifier' in capabilities['semanticTokensProvider']['legend']['tokenTypes'], capabilities
                no_errors()
            check('completion, signature help and plain identifier capabilities', initial_capabilities)

            def compact_completion_items():
                compact = Client(args.bolt.resolve())
                try:
                    compact.request('initialize', {'rootUri': root.as_uri(), 'capabilities': {},
                                    'initializationOptions': {'boltCompactCompletions': True}})
                    compact.send('initialized', {})
                    compact.send('textDocument/didOpen', {'textDocument': {
                        'uri': uri, 'languageId': 'bolt', 'version': version, 'text': text}})
                    compact.barrier()
                    for name, caret in [('widget', position(text, 'Print', inner=1)),
                                        ('Read', position(text, 'widget.Read', inner=len('widget.')))]:
                        query = {**params, 'position': caret}
                        full = c.request('textDocument/completion', query)['result']['items']
                        short = compact.request('textDocument/completion', query)['result']['items']
                        assert name in {item['label'] for item in short}, name
                        assert all(item['filterText'] == item['insertText'] == item['label'] and
                                   item['insertTextFormat'] == 1 and 'bolt' in item['data'] for item in full)
                        assert short == [{k: item[k] for k in ('label', 'detail', 'kind')} for item in full]
                    compact.stop()
                finally:
                    compact.kill()
            check('compact VS Code completions preserve names, insertion text, overloads and order', compact_completion_items)

            def pending_expressions():
                for typed, expected in [('M', {'Math', 'Main'}), ('val', {'value'}),
                                        ('w.', {'Read', 'Value'}), ('Math.', {'Pow', 'Pi'}),
                                        ('items.', {'Add', 'Count'})]:
                    source = 'import System.Collections.Generic\n' + program(
                        'Widget w; int value = 1; List<int> items; /* 😀 */ |')
                    start = change(source)
                    end = dict(start, character=start['character'] + len(typed))
                    edit = {'version': version, 'start': start, 'text': typed}
                    def preview(edit):
                        return c.request('textDocument/boltPendingCompletion',
                                         {**params, 'position': end, 'edit': edit})['result']
                    assert preview(dict(edit, version=version-1)) is None
                    items = {item['label']: item for item in preview(edit)['items']}
                    synced = complete(source.replace('|', typed+'|'))
                    assert expected <= items.keys(), (typed, items)
                    assert all(items[name] == synced[name] for name in expected), typed
                start = change('class | {}')
                end = dict(start, character=start['character'] + 1)
                assert preview({'version': version, 'start': start, 'text': 'T'}) is None
                start = change(program('Widget w; int count; |count;'))
                end = dict(start, character=start['character'] + 2)
                replaced = preview({'version': version, 'start': start,
                                    'end': dict(start, character=start['character'] + 5), 'text': 'w.'})
                assert {'Read', 'Value'} <= {item['label'] for item in replaced['items']}
            check('pending prefix and member suggestions use the current method and document version', pending_expressions)

            def builtin_members():
                for prefix in ('', 'P', 'Po'):
                    items = complete(program('Math.' + prefix + '|'))
                    assert {'Pi', 'E', 'Pow'} <= items.keys(), items
                    assert items['Pi']['kind'] == 21 and items['E']['kind'] == 21, items
                    assert items['Pow']['kind'] == 2, items['Pow']
                    assert '(' in items['Pow']['detail'], items['Pow']
                    assert not {'Main', 'Widget', 'Value', 'parameter'} & items.keys(), items
            check('Math members and signatures remain available during incomplete typing', builtin_members)

            def scoped_names():
                source = '''class EditorScope {
public:
    int Field;
    void Run(int parameter) {
        int local = 1;
        { int finishedBlock = 2; }
        par|
        int futureLocal = 3;
    }
    void Sibling(int siblingParameter) { int siblingOnly = 4; }
}
'''
                items = complete(source)
                assert {'parameter', 'local', 'Field'} <= items.keys(), items
                assert not {'finishedBlock', 'futureLocal', 'siblingParameter', 'siblingOnly',
                            'OtherField', 'OtherMethod', 'Secret', 'Hide', 'amount'} & items.keys(), items
                assert 'int' in items['parameter']['detail'] and 'int' in items['local']['detail'], items
            check('typing names suggests accessible parameters and locals with lexical scope', scoped_names)

            def loop_scope():
                items = complete(program('int outer = 1; for(int i = 0; i < 2; i++) { | }'))
                assert {'outer', 'i'} <= items.keys(), items
                items = complete(program('int outer = 1; for(int i = 0; i < 2; i++) { Print(i); } |'))
                assert 'outer' in items and 'i' not in items, items
            check('for-loop variables appear only within the loop', loop_scope)

            def member_visibility():
                items = complete(program('Widget widget; widget.|'))
                assert {'Value', 'Read', 'Add'} <= items.keys(), items
                assert not {'Secret', 'Hide', 'Twice', 'OtherField', 'OtherMethod', 'Main'} & items.keys(), items
                assert items['Read']['kind'] == 2 and '(' in items['Read']['detail'], items
                assert items['Value']['kind'] in (5, 10), items['Value']
                items = complete(program('Widget widget; var other = widget; other.|'))
                assert {'Value', 'Read', 'Add'} <= items.keys(), items
                items = complete(program('Widget.|'))
                assert 'Twice' in items, items
                assert not {'Value', 'Read', 'Add', 'Secret', 'Hide'} & items.keys(), items
            check('cross-file instance and static members respect access rules', member_visibility)

            def interface_and_enum():
                items = complete(program('Widget widget; IValue& view = widget; view.|'))
                assert 'Read' in items and not {'Value', 'Twice', 'Hide'} & items.keys(), items
                items = complete(program('Shade.|'))
                assert {'Red', 'Blue'} <= items.keys(), items
                assert all(items[name]['kind'] == 20 for name in ('Red', 'Blue')), items
            check('interface receivers and enum members get relevant suggestions', interface_and_enum)

            def collections():
                items = complete(program('List<int> values; values.|'))
                assert {'Add', 'Count', 'Contains', 'Clear'} <= items.keys(), items
                assert 'int' in items['Add']['detail'], items['Add']
                assert not {'Pi', 'Twice', 'OtherField'} & items.keys(), items
                items = complete(program('var values = List<int>(); values.|'))
                assert {'Add', 'Count', 'Contains'} <= items.keys(), items
            check('generic collection completions retain explicit and inferred element types', collections)

            def rectangular_array_completions():
                items = complete(program('int[,] matrix = new int[2,3]; matrix.|'))
                assert {'Length', 'Count', 'Rank', 'GetLength'} <= items.keys(), ('explicit rectangular array', items)
                assert items['GetLength']['kind'] == 2 and 'int' in items['GetLength']['detail'], items
            check('rectangular arrays expose dimension members', rectangular_array_completions)

            def inferred_rectangular_array_completions():
                items = complete(program('var matrix = new int[2,3]; matrix.|'))
                assert {'Rank', 'GetLength'} <= items.keys(), ('inferred rectangular array', items)
            check('inferred rectangular arrays expose dimension members', inferred_rectangular_array_completions)

            def rectangular_array_element_completions():
                items = complete(program('Widget[,] widgets = new Widget[2,3]; widgets[1,0].|'))
                assert {'Value', 'Read', 'Add'} <= items.keys(), ('indexed rectangular element', items)
                assert not {'Secret', 'Hide', 'Rank', 'GetLength'} & items.keys(), items
            check('rectangular array indices expose element members', rectangular_array_element_completions)

            def rectangular_array_hover():
                change(program('int[,] matrix = {{1,2},{3,4}}; Print(matrix.Rank); Print(matrix.GetLength(0));'))
                no_errors()
                result = c.request('textDocument/hover', {
                    **params, 'position': position(text, 'matrix', 1, 1)})['result']
                assert result and 'int[,] matrix' in result['contents']['value'], result
                assert kind('Rank') == 'property' and kind('GetLength') == 'method', tokens()
            check('rectangular array hover preserves rank and dimension access analyzes', rectangular_array_hover)

            def signature(source):
                caret = change(source)
                response = c.request('textDocument/signatureHelp', {**params, 'position': caret})
                assert 'error' not in response and response['result'], response
                return response['result']

            def signature_help():
                result = signature(program('Widget widget; widget.Add(1, |);'))
                assert result['activeParameter'] == 1, result
                assert len(result['signatures']) >= 2, result
                assert any('left' in entry['label'] and 'right' in entry['label']
                           for entry in result['signatures']), result
                result = signature(program('Math.Pow(2.0, |);'))
                assert result['activeParameter'] == 1, result
                assert any('Pow' in entry['label'] and len(entry.get('parameters', [])) == 2
                           for entry in result['signatures']), result
                result = signature(program('Math.Pow<double>(2.0, |);'))
                assert result['activeParameter'] == 1, result
                assert any('Pow' in entry['label'] for entry in result['signatures']), result
                result = signature(program('Widget widget; widget.Add(widget.Add(1, 2), |);'))
                assert result['activeParameter'] == 1, result
            check('signature help exposes overload parameters and tracks nested arguments', signature_help)

            def unknown_methods():
                for expression, name in (('widget.DoesNotExist()', 'DoesNotExist'),
                                         ('MissingMethod()', 'MissingMethod'),
                                         ('Math.NoSuchMath()', 'NoSuchMath')):
                    change(program('Widget widget; widget.Read(); ' + expression + ';'))
                    at = position(text, name)
                    matching = [diagnostic for diagnostic in diagnostics()
                                if diagnostic['range']['start'] == at]
                    assert matching, diagnostics()
                    diagnostic = matching[0]
                    assert diagnostic['severity'] == 1, diagnostic
                    assert diagnostic['range']['end'] == dict(at, character=at['character'] + len(name)), diagnostic
                    assert kind(name) == 'identifier', (name, kind(name), diagnostic)
                    assert kind('Read') == 'method', ('Known call lost its classification after an unrelated error', tokens())
                change(program('Widget widget; widget.Read(); Print(widget.MissingField);'))
                at = position(text, 'MissingField')
                matching = [diagnostic for diagnostic in diagnostics()
                            if diagnostic['range']['start'] == at]
                assert matching and matching[0]['severity'] == 1, diagnostics()
                assert matching[0]['range']['end'] == dict(at, character=at['character'] + len('MissingField')), matching
                assert kind('MissingField') in (None, 'identifier'), tokens()
                change(program('Widget widget; widget.Read(); unknownReceiver.Read();'))
                at = position(text, 'unknownReceiver')
                matching = [diagnostic for diagnostic in diagnostics()
                            if diagnostic['range']['start'] == at]
                assert matching and matching[0]['severity'] == 1, diagnostics()
                assert matching[0]['range']['end'] == dict(at, character=at['character'] + len('unknownReceiver')), matching
                assert kind('Read') == 'method' and kind('Read', 1) == 'identifier', tokens()
                change(program('Widget widget; widget.Read(); Math.Pow<double>(2.0, 3.0);'))
                no_errors()
                assert kind('Read') == 'method' and kind('Pow') == 'method', tokens()
            check('unknown calls stay plain and receive full-name errors which clear after editing', unknown_methods)

            def live_errors():
                change(program('int amount = "wrong";'))
                assert any(diagnostic['severity'] == 1 and diagnostic['code'] == 'BLT2002'
                           for diagnostic in diagnostics()), diagnostics()
                change(program('int amount = 3; Print(amount);'))
                no_errors()
            check('unsaved compiler type errors are published and cleared', live_errors)

            def semantic_colors():
                change('''class Formatter : IStringable {
public:
    int Value;
    string Format(int parameter) {
        int local = 2;
        return $"field:{Value} argument:{parameter} local:{local}";
    }
    string ToString() { return Format(1); }
}
static class Program { public: void Main() {
    Widget widget;
    IValue& view = widget;
    Shade shade = Shade.Red;
    Print($"value:{widget.Read()}");
} }
''')
                no_errors()
                assert kind('Value', 1) == 'field', tokens()
                assert kind('parameter', 1) == 'parameter', tokens()
                assert kind('local', 2) == 'variable', tokens()
                assert kind('Widget') == 'class' and kind('IValue') == 'interface', tokens()
                assert kind('Shade') == 'enum' and kind('Red') == 'enumMember', tokens()
                assert kind('Read') == 'method', tokens()
            check('type, field, parameter and local colors survive inside interpolation', semantic_colors)

            def screenshot_roundtrip():
                sample = ('class Person { public string Name; public int Age; }\n'
                          'struct Point { public int X; public int Y; }\n'
                          'public static class Program { public static void Main(string[] args) {\n'
                          '  Print(Random.Next<int>(0, 5));\n'
                          '  Console.Write("Hello World");\n'
                          '  Person person; Point point; point.X = 1;\n'
                          '  Print(person.Name); Print(args.Length);\n} }\n')
                sample = sample.replace('\n', '\r\n')
                change(sample)
                actual = diagnostics()
                assert len(actual) == 1 and actual[0]['message'] == 'missing import Random', actual
                def request(method, word, occurrence=0):
                    at = position(text, word, occurrence)
                    return c.request('textDocument/' + method, {**params, 'position': at})['result']

                def action_params(span):
                    return {**params, 'range': span, 'context': {'diagnostics': diagnostics(), 'only': ['quickfix']}}
                reply = c.request('textDocument/codeAction', action_params(actual[0]['range']))['result']
                assert len(reply) == 1 and reply[0]['kind'] == 'quickfix', reply
                edit = reply[0]['edit']['documentChanges'][0]
                assert edit['textDocument'] == {'uri': uri, 'version': version}, edit
                addition = edit['edits'][0]
                origin = {'line': 0, 'character': 0}
                assert addition == {'range': {'start': origin, 'end': origin}, 'newText': 'import Random\r\n'}, addition
                assert c.request('textDocument/codeAction', action_params({'start': origin, 'end': origin}))['result'] == []
                for word, label, path_end in [('Next', 'T min', '/System/Random.bolt'),
                                              ('Write', 'Write<', '/System/Console.bolt')]:
                    assert label in request('hover', word)['contents']['value']
                    definitions = request('definition', word)
                    assert len(definitions) == 1 and definitions[0]['uri'].endswith(path_end), definitions
                change(addition['newText'] + sample)
                no_errors()
                assert c.request('textDocument/codeAction', action_params(actual[0]['range']))['result'] == []

                change(addition['newText'] + sample.replace('point.X = 1;', 'int unfinished = ; point.X = 1;'))
                assert diagnostics(), 'The malformed initializer must keep its compiler diagnostic'
                for use, decl, occurrence, label in [('point.X', 'point;', 0, 'Point point'),
                                                     ('X =', 'X;', 0, 'int X'),
                                                     ('person.Name', 'person;', 0, 'Person person'),
                                                     ('Name)', 'Name;', 0, 'string Name'),
                                                     ('args.Length', 'args)', 0, 'string[] args')]:
                    definitions = request('definition', use, occurrence)
                    assert len(definitions) == 1 and definitions[0]['uri'] == uri, (use, definitions)
                    assert definitions[0]['range']['start'] == position(text, decl), (use, definitions)
                    assert label in request('hover', use, occurrence)['contents']['value']
                assert 'int Length' in request('hover', 'Length')['contents']['value']
                assert request('definition', 'Length') == []
                target = request('definition', 'Next')[0]
                virtual = c.request('bolt/virtualDocument', {'uri': target['uri']})['result']
                line = virtual['text'].splitlines()[target['range']['start']['line']]
                assert line[target['range']['start']['character']:].startswith('Next<T>(T min'), line
                change(text.replace('person.Name', 'person.NotAField'))
                assert request('definition', 'NotAField') == []
                assert request('hover', 'NotAField') is None
                change(text.replace('person.NotAField', 'person.Renamed').replace('string Name', 'string Renamed'))
                assert request('definition', 'Renamed)')[0]['range']['start'] == position(text, 'Renamed;')
            check('the screenshot supports import quick fix, standard definitions and current-buffer navigation during errors', screenshot_roundtrip)

            def expression_namespaces():
                for source, expected in [
                    (program('Widget w; ((w)).|'), {'Value', 'Read'}),
                    (program('"literal".|'), {'Trim', 'Length', 'ToUtf8'}),
                    (program('System.IO.|'), {'File', 'FileStream'}),
                    (program('System.|'), {'IO', 'Console'}),
                    ('import System\n' + program('Random.|'), {'Seed', 'Next'}),
                ]:
                    members = complete(source)
                    assert expected <= members.keys(), (source, expected - members.keys())
            check('grouped expressions, string literals, namespace chains and imported Random have member completion', expression_namespaces)



            def completion_during_check():
                nonlocal version, text
                # Give the real semantic checker enough work to observe overlap.
                # Synchronize on its trace message and assert reply order, not a timer.
                load = root / 'Load.bolt'
                load.write_text('class Load { public: ' + ''.join(
                    f'int M{i}(int value) {{ return value + {i}; }} ' for i in range(600)) + '}')
                prefix = ('import Math\nimport System.Collections.Generic\n'
                          'public class ZoneWidget { public: int Speed; int Boost() { return Speed; } '
                          'static int Spawn() { return 1; } }\n'
                          'static class Program { public: void Main() {\n'
                          'ZoneWidget widget; ZoneWidget actor; List<int> values; int velocity = ')
                c.send('$/setTrace', {'value': 'messages'})
                try:
                    for expression, label in [('M', 'Math'), ('Math.', 'Abs'), ('v', 'velocity'),
                                              ('Z', 'ZoneWidget'), ('widget.', 'Boost'), ('actor.', 'Speed'),
                                              ('ZoneWidget.', 'Spawn'), ('values.', 'Add')]:
                        version += 1
                        text = prefix + str(version) + ';\n    \n} }'
                        start = position(text, '    \n', inner=4)
                        c.send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': version},
                                                          'contentChanges': [{'text': text}]})
                        while True:
                            event = c.receive()
                            if event.get('method') == '$/logTrace' and event['params']['message'] == 'Checking Bolt project':
                                break
                        check_start = len(c.history)
                        c.seq += 1
                        hover_id = c.seq
                        c.send('textDocument/hover', {**params, 'position': start}, hover_id)
                        response = c.request('textDocument/boltPendingCompletion', {
                            **params, 'position': dict(start, character=start['character'] + len(expression)),
                            'edit': {'version': version, 'start': start, 'end': start, 'text': expression}})
                        assert label in {item['label'] for item in response['result']['items']}, expression
                        before_reply = c.history[check_start:-1]
                        assert not any(m.get('id') == hover_id or
                                       (m.get('method') == 'textDocument/publishDiagnostics' and
                                        m['params'].get('version') == version) for m in before_reply), expression
                        c.barrier()
                        assert diagnostics() == []
                finally:
                    c.send('$/setTrace', {'value': 'off'})
                    load.unlink()
            check('all eight typing completions overtake a running check and a queued hover', completion_during_check)
            c.stop()
        finally:
            c.kill()
    print(f'{count} IntelliSense checks passed; {len(failures)} failed')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
