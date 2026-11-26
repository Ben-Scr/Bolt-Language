#!/usr/bin/env python3
"""Real LSP contract for file-private and nested types, shared by IntelliJ."""
import argparse
from pathlib import Path
import tempfile
from lsp_protocol import Client, position


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bolt', type=Path, required=True)
    args = parser.parse_args()
    passed = 0
    failures = []

    def check(condition, message):
        nonlocal passed
        if not condition:
            failures.append(message)
            print('FAIL ' + message, flush=True)
            return
        passed += 1
        print('PASS ' + message, flush=True)

    with tempfile.TemporaryDirectory(prefix='bolt-type-editor-') as directory:
        root = Path(directory)
        (root / 'bolt.project').write_text('{}')
        types = root / 'Animals.bolt'
        declarations = '''namespace Animals;
private class HiddenClass {}
private struct HiddenStruct {}
private enum HiddenEnum { Idle }
private interface HiddenInterface {}
public class PublicAnimal {}
public class Outer {
private:
    class Secret {}
    int OuterValue;
public:
    class Inner { public: int Value; class Deep {} }
    struct Point { public: int X; }
    enum State { Idle, Running }
    interface IHelper {}
    class Box<T> { public: T Value; }
    void Inspect() {}
}
public class GenericOuter<T> {
public:
    class Child<U> { public: T OuterData; U InnerData; }
    enum GenericState { Idle }
}
'''
        types.write_text(declarations)
        program = root / 'Program.bolt'
        program.write_text('static class Program { public: void Main() {} }')
        client = Client(args.bolt.resolve())
        versions = {}
        texts = {}
        client.request('initialize', {
            'processId': None, 'rootUri': root.as_uri(),
            'initializationOptions': {'boltSemanticNotifications': True}, 'capabilities': {}})
        client.send('initialized', {})

        def update(path, text):
            uri = path.as_uri()
            caret = text.find('|')
            text = text.replace('|', '')
            texts[path] = text
            versions[path] = versions.get(path, 0) + 1
            if versions[path] == 1:
                client.send('textDocument/didOpen', {'textDocument': {
                    'uri': uri, 'languageId': 'bolt', 'version': versions[path], 'text': text}})
            else:
                client.send('textDocument/didChange', {
                    'textDocument': {'uri': uri, 'version': versions[path]},
                    'contentChanges': [{'text': text}]})
            client.barrier()
            prefix = text[:caret] if caret >= 0 else text
            return {'line': prefix.count('\n'),
                    'character': len(prefix.rsplit('\n', 1)[-1].encode('utf-16-le')) // 2}

        def request(method, path, pos, **extra):
            response = client.request('textDocument/' + method, {
                'textDocument': {'uri': path.as_uri()}, 'position': pos, **extra})
            assert 'error' not in response, response
            return response['result']

        def completion(path, text):
            return request('completion', path, update(path, text))['items']

        def names(path, text):
            return {item['label'] for item in completion(path, text)}

        def main_code(body):
            return 'import Animals;\nstatic class Program { public: void Main() { ' + body + ' } }'

        def diagnostics(path):
            return client.latest('textDocument/publishDiagnostics', path.as_uri())['diagnostics']

        def kind(path, pos):
            return next((token['kind'] for token in client.latest('bolt/semanticTokens', path.as_uri())['tokens']
                         if token['line'] == pos['line'] and token['start'] == pos['character']), None)

        try:
            hidden = {'HiddenClass', 'HiddenStruct', 'HiddenEnum', 'HiddenInterface'}
            items = names(program, main_code('|'))
            check('PublicAnimal' in items and not items & hidden,
                  'cross-file completion excludes private class, struct, enum and interface')
            items = names(program, main_code('Animals.|'))
            check('PublicAnimal' in items and not items & hidden,
                  'namespace completion respects file-private types')
            items = names(types, declarations + '\nclass Local { public: void Test() { | } }')
            check(hidden <= items, 'same-file completion retains all private types')
            update(types, declarations)

            items = names(program, main_code('Outer.|'))
            check({'Inner', 'Point', 'State', 'IHelper', 'Box'} <= items and 'Secret' not in items,
                  'qualified completion includes public nested type kinds and excludes private types')
            check('Deep' in names(program, main_code('Outer.Inner.|')),
                  'completion follows multiple nested type levels')
            check({'Idle', 'Running'} <= names(program, main_code('Outer.State.|')),
                  'nested enum completion retains its static receiver')
            box = completion(program, main_code('Outer.|'))
            check(next(item for item in box if item['label'] == 'Box')['data']['bolt']['typeParameters'] == ['T'],
                  'nested generic completion carries type parameters')
            check('Value' in names(program, main_code('Outer.Box<int> box; box.|')),
                  'nested generic instance completion resolves members')
            check({'Child', 'GenericState'} <= names(program, main_code('GenericOuter<int>.|')),
                  'nested completion resolves an enclosing generic type')
            generic_items = {item['label']: item['data']['bolt']['type'] for item in
                             completion(program, main_code('GenericOuter<int>.Child<string> child; child.|'))}
            check(generic_items.get('OuterData') == 'int' and generic_items.get('InnerData') == 'string',
                  'incomplete-source completion substitutes enclosing and nested generic arguments')
            items = names(program, main_code('|'))
            check(not items & {'Inner', 'Point', 'State', 'IHelper', 'Box', 'Secret'},
                  'nested short names do not leak into unrelated scopes')
            local = declarations.replace('void Inspect() {}', 'void Inspect() { | }')
            items = names(types, local)
            check({'Secret', 'Inner', 'Point', 'State', 'IHelper', 'Box'} <= items,
                  'declaring outer scope can complete its private nested types')
            local = declarations.replace('class Deep {}', 'class Deep { public: void Test() { | } }')
            items = names(types, local)
            check('OuterValue' not in items, 'nested scope does not acquire outer instance members')
            update(types, declarations)

            source = main_code('Outer.Inner inner; Outer.Inner.Deep deep; Outer.State state = Outer.State.Idle;')
            update(program, source)
            check(not diagnostics(program), 'nested type references analyze successfully')
            for word, category, declaration_word in [
                ('Inner', 'class', 'Inner'), ('Deep', 'class', 'Deep'), ('State', 'enum', 'State')]:
                pos = position(source, word)
                definition = request('definition', program, pos)
                check(len(definition) == 1 and definition[0]['uri'] == types.as_uri()
                      and definition[0]['range']['start'] == position(declarations, declaration_word),
                      'nested ' + word + ' definition targets its physical declaration')
                check(kind(program, pos) == category, 'nested ' + word + ' has semantic color')
                hover = request('hover', program, pos)
                check('Outer.' in hover['contents']['value'], 'nested ' + word + ' hover has qualified identity')
            outer = request('definition', program, position(source, 'Outer'))
            check(outer[0]['range']['start'] == position(declarations, 'Outer'),
                  'outer qualifier navigates independently of nested leaf')
            references = request('references', program, position(source, 'Inner'), context={'includeDeclaration': True})
            check(len(references) == 3, 'nested type references include declaration and both qualified uses')
            rename = request('rename', program, position(source, 'Inner'), newName='RenamedInner')
            edits = [edit for document in rename.get('documentChanges', []) for edit in document['edits']]
            if not edits:
                edits = [edit for document in rename.get('changes', {}).values() for edit in document]
            check(len(edits) == 3 and all(edit['newText'] == 'RenamedInner' for edit in edits),
                  'nested rename edits only the selected type tokens')
            index = request('boltSymbolIndex', program, position(source, 'Inner'))
            check(any(symbol['name'] == 'Inner' and symbol['hover']['bolt']['owner'] == 'Animals.Outer'
                      for symbol in index['symbols'].values()),
                  'IntelliJ symbol index exposes enclosing type identity')

            generic_source = main_code('Outer.Box<int> numbers; Outer.Box<string> words;')
            update(program, generic_source)
            check(not diagnostics(program), 'multiple nested generic instantiations analyze successfully')
            generic_definition = request('definition', program, position(generic_source, 'Box'))
            check(len(generic_definition) == 1 and generic_definition[0]['range']['start'] == position(declarations, 'Box'),
                  'nested generic navigation targets its source template')
            generic_references = request('references', program, position(generic_source, 'Box'),
                                         context={'includeDeclaration': True})
            check(len(generic_references) == 3, 'nested generic usages group distinct type instantiations')

            collisions = declarations + '\npublic class Inner { public: int TopLevel; }'
            update(types, collisions)
            collision_source = main_code('Inner top; Outer.Inner nested;')
            update(program, collision_source)
            top_definition = request('definition', program, position(collision_source, 'Inner'))
            nested_definition = request('definition', program, position(collision_source, 'Inner', 1))
            check(not diagnostics(program) and top_definition != nested_definition,
                  'same-named top-level and nested types retain separate navigation identities')
            collision_references = request('references', program, position(collision_source, 'Inner', 1),
                                           context={'includeDeclaration': True})
            check(len(collision_references) == 2,
                  'nested type usages exclude the same-named top-level declaration')
            update(types, declarations)

            source = main_code('HiddenClass denied;')
            update(program, source)
            problems = diagnostics(program)
            check(any('private to its declaring file' in problem['message'] for problem in problems),
                  'LSP reports shared file-private semantic diagnosis')
            check(kind(program, position(source, 'HiddenClass')) == 'class',
                  'inaccessible but known type retains semantic color')
            source = main_code('Outer.Secret denied;')
            update(program, source)
            check(bool(diagnostics(program)) and kind(program, position(source, 'Secret')) == 'class',
                  'private nested type error preserves known class color')

            same_file = declarations + '\nclass Local { public: void Test() { HiddenClass value; } }'
            update(program, main_code(''))
            update(types, same_file)
            definition = request('definition', types, position(same_file, 'HiddenClass', 1))
            check(not diagnostics(types) and definition[0]['range']['start'] == position(declarations, 'HiddenClass'),
                  'same-file private type navigation remains available')
        finally:
            client.stop()
    print(f'{passed} type visibility/editor checks passed')
    assert not failures, failures


if __name__ == '__main__':
    main()
