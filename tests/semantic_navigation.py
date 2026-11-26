#!/usr/bin/env python3
"""Exact semantic destinations, including compiler-embedded attribute sources."""
import argparse
from pathlib import Path
import tempfile
from lsp_protocol import Client, position

parser = argparse.ArgumentParser()
parser.add_argument('--bolt', type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='bolt-navigation-') as directory:
    root = Path(directory)
    (root / 'bolt.project').write_text('{}')
    models = '''namespace Models;
public class FancyAttribute : System.Attribute {}
public interface View { int Read(); }
public struct Pair { public: int X; }
public enum Mode { First, Second }
public class Player { public: int X; }
public class Box<T> { public: T Value; }
concept Capable<T> = Class<T>;
'''
    helper = root / 'Models.bolt'
    helper.write_text(models)
    source = '''import Models
import System.Collections.Generic
[Reflect, Fancy] public class Host {
public:
 [JsonIgnore] Player Field;
 Mode State;
 Pair PairValue;
 View* InterfaceValue;
 Box<Player> Nested;
 Player Property { get { return Field; } set { Field=value; } }
 [ReadOnly] int Read() { return 1; }
 Player Copy(Player value) { Player local=value; return local; }
 T Constrained<T>(T value) : Capable<T> { return value; }
 void Use() { Player p; p.X=1; Field=p; Property=p; Read(); Copy(p); }
}
[Header("native.h"), Native("Bridge")] public class Bridge { public: void Ping(); }
'''
    file = root / 'Main.bolt'
    file.write_text(source)
    uri = file.as_uri()
    client = Client(args.bolt.resolve())
    try:
        client.request('initialize', {'rootUri': root.as_uri(), 'capabilities': {}})
        client.send('initialized', {})
        version = 0

        def update(text):
            global version
            version += 1
            if version == 1:
                client.send('textDocument/didOpen', {'textDocument': {'uri': uri, 'languageId': 'bolt', 'version': version, 'text': text}})
            else:
                client.send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': version}, 'contentChanges': [{'text': text}]})
            client.barrier()

        def request(method, word, inner=0, text=source):
            return client.request(method, {'textDocument': {'uri': uri}, 'position': position(text, word, 0, inner)})['result']

        update(source)
        diagnostics = client.latest('textDocument/publishDiagnostics', uri)['diagnostics']
        assert not diagnostics, diagnostics
        snapshot = client.request('textDocument/boltSymbolIndex', {'textDocument': {'uri': uri}})['result']

        def exact(word, target_word, target_text=models, target_uri=helper.as_uri(), inner=0, target_occurrence=0):
            definitions = request('textDocument/definition', word, inner)
            assert len(definitions) == 1, (word, definitions)
            target = definitions[0]
            assert target['uri'] == target_uri, (word, target)
            assert target['range']['start'] == position(target_text, target_word, target_occurrence), (word, target)
            hover = request('textDocument/hover', word, inner)
            assert hover['bolt']['definition'] == target, (word, hover, target)
            at = position(source, word, 0, inner)
            occurrence = next(hit for hit in snapshot['occurrences'] if (hit['range']['start']['line'], hit['range']['start']['character']) <= (at['line'], at['character']) < (hit['range']['end']['line'], hit['range']['end']['character']))
            assert snapshot['symbols'][occurrence['symbol']]['definition'] == target
            print('PASS exact compiler binding', word, flush=True)

        for word, target in [('Fancy]', 'FancyAttribute'), ('Player Field', 'Player {'), ('Mode State', 'Mode {'),
                             ('Pair PairValue', 'Pair {'), ('View* InterfaceValue', 'View {'), ('Box<Player>', 'Box<T>'),
                             ('Player> Nested', 'Player {'), ('Player Property', 'Player {'), ('Player Copy', 'Player {'),
                             ('Player value', 'Player {'), ('Player local', 'Player {'), ('Player p', 'Player {'),
                             ('Capable<T>', 'Capable<T>')]:
            exact(word, target)
        for word, declaration in [('Field=p', 'Field;'), ('Property=p', 'Property {'), ('Read();', 'Read()'),
                                   ('Copy(p)', 'Copy(Player'), ('local;', 'local=value'), ('p.X', 'p;')]:
            exact(word, declaration, source, uri)
        exact('p.X', 'X;', models, helper.as_uri(), 2, 1)
        for name in ('Reflect', 'ReadOnly', 'JsonIgnore', 'Native', 'Header'):
            result = request('textDocument/definition', name)[0]
            assert result['uri'].endswith('/System/CompilerAttributes.bolt'), result
            virtual = client.request('bolt/virtualDocument', {'uri': result['uri']})['result']
            embedded = snapshot['virtualDocuments'][result['uri']]
            assert name + 'Attribute' in embedded and (virtual.get('text') == embedded if isinstance(virtual, dict) else virtual == embedded), virtual
            assert result['range']['start'] == position(embedded, name + 'Attribute'), (name, result)
            assert request('textDocument/hover', name)['bolt']['definition'] == result
            print('PASS actual embedded attribute source', name, flush=True)
        for text, word in [(source + '\n[Unknown] class Unknown {}', 'Unknown]'),
                           (source + '\nclass Unrelated { public: void Missing() {} }\nclass Test { public: void Go(){Missing();} }', 'Missing();'),
                           (source + '\nclass Bogus { public: MissingType Field; }', 'MissingType')]:
            update(text)
            assert request('textDocument/definition', word, text=text) == [], word
            assert request('textDocument/hover', word, text=text) is None, word
            print('PASS unresolved symbol never guesses', word, flush=True)
        client.stop()
    finally:
        client.kill()
