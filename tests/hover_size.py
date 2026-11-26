#!/usr/bin/env python3
"""Compiler-backed hover sizes use the same Bolt value layout as sizeof."""
import argparse
from pathlib import Path
import re
import tempfile

from lsp_protocol import Client, position


SOURCE = '''enum Mode:ushort { Ready=1 }
interface View { int Read(); }
class Vec2 { public: float X; float Y; }
struct Pair { public: byte Tag; long Value; }
class Base { public: int Count; }
class Derived:Base { public: byte Tag; long Value; }
class StaticStorage { public: static Vec2 Cache; byte Flag; }
class Box<T> { public: byte Tag; T Value; }
class Host {
public:
    Vec2 Field;
    Vec2 Property { get { return Field; } }
    int Take(Vec2 argument) { return 1; }
}
static class Program {
public:
    void Main() {
        Vec2 vector;
        vector.X=1;
        Vec2& reference=vector;
        Vec2* pointer=&vector;
        Vec2? optionalVector=vector;
        Vec2[] array;
        int? optionalInt=1;
        Box<int> intBox;
        Box<long> longBox;
        Mode mode=Mode.Ready;
        Host host;
        Vec2 copied=host.Property;
        host.Take(vector);
        bool boolean=false;
        byte small=0;
        short signedShort=0;
        ushort unsignedShort=0;
        int integer=0;
        uint unsignedInt=0;
        long signedLong=0;
        ulong unsignedLong=0;
        float single=0;
        double real=0;
        char character='a';
        string text="hello";
    }
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bolt', type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='bolt-hover-size-') as directory:
        root = Path(directory)
        (root / 'bolt.project').write_text('{}')
        path = root / 'Main.bolt'
        path.write_text(SOURCE)
        uri = path.as_uri()
        client = Client(args.bolt.resolve())
        version = 0
        source = SOURCE
        snapshot = None
        try:
            client.request('initialize', {'rootUri': root.as_uri(), 'capabilities': {}})
            client.send('initialized', {})

            def update(text, clean=True):
                nonlocal version, source, snapshot
                version += 1
                source = text
                if version == 1:
                    client.send('textDocument/didOpen', {'textDocument': {
                        'uri': uri, 'languageId': 'bolt', 'version': version, 'text': text}})
                else:
                    client.send('textDocument/didChange', {
                        'textDocument': {'uri': uri, 'version': version},
                        'contentChanges': [{'text': text}]})
                client.barrier()
                if clean:
                    diagnostics = client.latest('textDocument/publishDiagnostics', uri)['diagnostics']
                    assert not diagnostics, diagnostics
                snapshot = client.request('textDocument/boltSymbolIndex', {
                    'textDocument': {'uri': uri}})['result']

            def hovers(word, inner=0):
                at = position(source, word, inner=inner)
                hover = client.request('textDocument/hover', {
                    'textDocument': {'uri': uri}, 'position': at})['result']
                point = (at['line'], at['character'])
                hit = next((item for item in snapshot['occurrences']
                            if (item['range']['start']['line'], item['range']['start']['character'])
                            <= point < (item['range']['end']['line'], item['range']['end']['character'])), None)
                indexed = snapshot['symbols'][hit['symbol']].get('hover') if hit else None
                return hover, indexed

            def size(word, expected, inner=0):
                hover, indexed = hovers(word, inner)
                assert hover and indexed, (word, hover, indexed)
                for result in (hover, indexed):
                    value = result['bolt'].get('sizeBytes')
                    assert type(value) is int and value == expected, (word, expected, result)
                    unit = 'byte' if expected == 1 else 'bytes'
                    assert f'Size: {expected} {unit}' in result['contents']['value'], (word, result)
                assert hover['bolt'].get('sizeNote') == indexed['bolt'].get('sizeNote'), (word, hover, indexed)
                print('PASS hover and symbol index size', word, expected, flush=True)

            def no_size(word, require_hover=False):
                results = hovers(word)
                if require_hover:
                    assert all(results), (word, results)
                for result in results:
                    if result:
                        assert 'sizeBytes' not in result['bolt'], (word, result)
                        assert not re.search(r'Size: \d+ bytes?', result['contents']['value']), (word, result)
                print('PASS no fabricated size', word, flush=True)

            update(SOURCE)
            for word, expected in [
                ('Vec2 {', 16), ('Vec2 vector', 16), ('vector;', 16), ('vector.X', 16),
                ('X;', 4), ('Field;', 16), ('Property {', 16), ('Property;', 16), ('argument)', 16),
                ('Pair {', 16), ('Base {', 16), ('Derived:Base', 32), ('StaticStorage {', 16),
                ('Mode:ushort', 2), ('mode=', 2), ('View {', 16),
                ('reference=', 16), ('pointer=', 8), ('optionalVector=', 24), ('array;', 8),
                ('optionalInt=', 16), ('intBox;', 16), ('longBox;', 24),
                ('boolean=', 1), ('small=', 1), ('signedShort=', 2), ('unsignedShort=', 2),
                ('integer=', 4), ('unsignedInt=', 4), ('signedLong=', 8), ('unsignedLong=', 8),
                ('single=', 4), ('real=', 8), ('character=', 4), ('text=', 8),
            ]:
                size(word, expected)
            no_size('Box<T>', require_hover=True)
            no_size('Take(Vec2', require_hover=True)
            no_size('Take(vector)', require_hover=True)

            # The new field changes both the inline value and its optional wrapper.
            edited = SOURCE.replace('float Y;', 'float Y; long Z;')
            update(edited)
            assert path.read_text() == SOURCE, 'hover edits must remain unsaved'
            for word, expected in [('Vec2 {', 24), ('Vec2 vector', 24), ('vector.X', 24),
                                   ('reference=', 24), ('pointer=', 8), ('optionalVector=', 32)]:
                size(word, expected)

            update(SOURCE + '\nclass Unresolved { public: MissingType Field; }\n', clean=False)
            no_size('Unresolved {')
            no_size('MissingType Field')
            update(SOURCE + '\nclass Partial { public: int ', clean=False)
            no_size('Partial {')
            client.stop()
        finally:
            client.kill()
    print('Hover size checks passed')


if __name__ == '__main__':
    main()
