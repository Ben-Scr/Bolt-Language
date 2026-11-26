#!/usr/bin/env python3
"""Extension methods and structural iteration in the real Bolt language server."""
import argparse
from pathlib import Path

from lsp_intellisense import fixture_directory
from lsp_protocol import Client, position


MODELS = '''namespace Models;
public class Widget {
public:
    int Value;
    int Read() { return Value; }
}
public class WidgetEnumerator {
public:
    Widget Current;
    bool MoveNext() { return false; }
}
public class WidgetRange {
public:
    WidgetEnumerator GetEnumerator() { return WidgetEnumerator(); }
}
public class ForeignRange { }
'''

EXTENSIONS = '''import Models
import System.Collections.Generic
namespace Tools;
public static class Extensions {
public:
    int Bump(this Widget& target, int amount, int factor = 1) {
        target.Value += amount * factor;
        return target.Value;
    }
    T Echo<T>(this T target) { return target; }
    T First<T>(this T[] values) { return values[0]; }
    T Head<T>(this List<T>& values) { return values[0]; }
    string OnlyText(this string target) { return target; }
    WidgetEnumerator GetEnumerator(this ForeignRange source) { return WidgetEnumerator(); }
private:
    int SecretExtension(this Widget target) { return target.Value; }
}
'''


def program(body, imports='import Tools\n'):
    return 'import Models\n' + imports + 'static class Program { public: void Main() {\n' + body + '\n} }\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', required=True, type=Path)
    parser.add_argument('--temp-root', type=Path)
    args = parser.parse_args()
    checks = 0
    with fixture_directory(args.temp_root) as root:
        (root / 'bolt.project').write_text('{}', encoding='utf-8')
        (root / 'Models.bolt').write_text(MODELS, encoding='utf-8')
        helpers = root / 'Extensions.bolt'
        helpers.write_text(EXTENSIONS, encoding='utf-8')
        (root / 'Hidden.bolt').write_text('''import Models
namespace Hidden;
public static class HiddenExtensions { public: int Invisible(this Widget target) { return 0; } }
''', encoding='utf-8')
        path = root / 'Program.bolt'
        original = program('Widget widget; int result = widget.Bump(2);')
        path.write_text(original, encoding='utf-8')
        uri = path.as_uri()
        params = {'textDocument': {'uri': uri}}
        client = Client(args.bolt.resolve())
        version = 0
        text = original
        try:
            client.request('initialize', {'processId': None, 'rootUri': root.as_uri(),
                                           'capabilities': {}})
            client.send('initialized', {})
            client.send('textDocument/didOpen', {'textDocument': {
                'uri': uri, 'languageId': 'bolt', 'version': version, 'text': original}})
            client.barrier()

            def change(source):
                nonlocal version, text
                assert source.count('|') <= 1
                prefix = source.split('|')[0]
                caret = {'line': prefix.count('\n'),
                         'character': len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}
                text = source.replace('|', '')
                version += 1
                client.send('textDocument/didChange', {'textDocument': {'uri': uri, 'version': version},
                                                       'contentChanges': [{'text': text}]})
                client.barrier()
                return caret

            def request(method, caret):
                response = client.request('textDocument/' + method, {**params, 'position': caret})
                assert 'error' not in response, response
                return response['result']

            def complete(source):
                response = request('completion', change(source))
                return {item['label']: item for item in response['items']}

            def passed(name):
                nonlocal checks
                checks += 1
                print('PASS ' + name, flush=True)

            errors = client.latest('textDocument/publishDiagnostics', uri)['diagnostics']
            assert not errors, errors
            passed('extension fixtures bind without diagnostics')

            items = complete(program('Widget widget; widget.|'))
            assert 'Bump' in items and 'Echo' in items, items
            assert not {'OnlyText', 'SecretExtension', 'Invisible', 'First'} & items.keys(), items
            bump = items['Bump']
            assert bump['data']['bolt']['parameters'] == ['int amount', 'int factor = 1'], bump
            assert 'target' not in bump['detail'] and not bump['data']['bolt']['static'], bump
            passed('member completion filters receiver, access and namespace and hides this parameter')

            items = complete(program('Widget widget; widget.|', imports=''))
            assert 'Bump' not in items and 'Echo' not in items, items
            items = complete(program('Widget widget; widget.|', imports='import Tools.Extensions\n'))
            assert 'Bump' in items, items
            passed('extensions require a visible namespace or explicitly imported owner')

            items = complete(program('Widget widget; widget.Echo().|'))
            assert 'Read' in items and 'Value' in items, items
            items = complete(program('Widget widget; widget.Echo<Widget>().|'))
            assert 'Read' in items and 'Value' in items, items
            items = complete(program('Widget[] values; values.First().|'))
            assert 'Read' in items and 'Value' in items, items
            items = complete(program('List<Widget> values; values.Head().|'))
            assert 'Read' in items and 'Value' in items, items
            passed('generic receiver inference preserves chained result members')

            signature = request('signatureHelp', change(program('Widget widget; widget.Bump(2, |);')))
            assert signature and signature['activeParameter'] == 1, signature
            selected = signature['signatures'][signature['activeSignature']]
            assert [parameter['label'] for parameter in selected['parameters']] == ['int amount', 'int factor = 1'], selected
            signature = request('signatureHelp', change(program('Widget widget; Extensions.Bump(widget, |);')))
            assert signature and signature['activeParameter'] == 1, signature
            selected = signature['signatures'][signature['activeSignature']]
            assert len(selected['parameters']) == 3 and selected['parameters'][0]['label'].startswith('this '), selected
            passed('member and static signature help display their respective argument lists')

            change(original)
            at = position(text, 'Bump')
            definition = request('definition', at)
            assert definition and definition[0]['uri'] == helpers.as_uri(), definition
            assert definition[0]['range']['start'] == position(EXTENSIONS, 'Bump'), definition
            hover = request('hover', at)
            assert hover and 'Bump' in hover['contents']['value'], hover
            assert 'target' not in hover['bolt']['label'], hover
            passed('extension hover and definition use the selected physical method')

            for source_type in ['WidgetRange', 'ForeignRange', 'List<Widget>']:
                items = complete(program(source_type + ' values; for (value : values) { value.| }'))
                assert 'Read' in items and 'Value' in items, (source_type, items)
            passed('range-for inference follows ordinary and extension GetEnumerator contracts')

            assert path.read_text(encoding='utf-8') == original
            client.stop()
        finally:
            client.kill()
    print(str(checks) + ' extension IntelliSense checks passed')


if __name__ == '__main__':
    main()
