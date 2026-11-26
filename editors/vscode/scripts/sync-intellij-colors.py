"""Regenerate the two VS Code themes from the shipped IntelliJ Bolt schemes."""
import argparse
import json
from pathlib import Path
from xml.etree import ElementTree

ROOT = Path(__file__).resolve().parents[1]
SCHEMES = ROOT.parent / 'intellij/src/main/resources/colorSchemes'
# Each row names the IntelliJ key, semantic token kinds, and TextMate fallback scopes.
CATEGORIES = (
    ('KEYWORD', 'keyword', 'keyword.other.bolt'),
    ('CONTROL_FLOW', 'boltControlFlow', 'keyword.control.bolt'),
    ('PRIMITIVE_TYPE', 'boltPrimitive', 'support.type.primitive.bolt'),
    ('TYPE_PARAMETER', 'typeParameter', ''),
    ('CLASS', 'type class', ''),
    ('STRUCT', 'struct', ''),
    ('INTERFACE', 'interface', ''),
    ('ENUM', 'enum', ''),
    ('METHOD', 'method function', ''),
    ('FIELD', 'field event', ''),
    ('PROPERTY', 'property', ''),
    ('CONSTANT', 'enumMember', ''),
    ('PARAMETER', 'parameter', ''),
    ('LOCAL_VARIABLE', 'variable', ''),
    ('NAMESPACE', 'namespace', ''),
    ('IDENTIFIER', 'identifier', 'variable.other.bolt punctuation.separator.bolt'),
    ('STRING', 'string', 'string.quoted.double.bolt string.quoted.double.interpolated.bolt'),
    ('CHARACTER', 'character', 'string.quoted.single.bolt'),
    ('NUMBER', 'number', 'constant.numeric.bolt'),
    ('LINE_COMMENT', 'comment', 'comment.line.double-slash.bolt'),
    ('BLOCK_COMMENT', '', 'comment.block.bolt'),
    ('OPERATOR', 'operator', 'keyword.operator.bolt'),
    ('BRACES', '', 'punctuation.section.braces.bolt'),
    ('PARENTHESES', '', 'punctuation.section.parentheses.bolt'),
    ('BRACKETS', '', 'punctuation.section.brackets.bolt'),
    ('ATTRIBUTE', 'decorator', ''),
    ('CONCEPT', 'concept', ''),
    ('INACTIVE_CODE', 'boltInactiveCode inactiveCode', ''),
    ('PREPROCESSOR', 'macro', 'meta.preprocessor.bolt'),
)


def palette(path):
    nodes = ElementTree.parse(path).getroot().findall('.//option[@name]')
    return {node.get('name')[5:]: '#' + node.find('value/option[@name="FOREGROUND"]').get('value')
            for node in nodes if node.get('name').startswith('BOLT_')}


def theme(mode):
    prefix = 'BoltCSharp' + mode
    colors = palette(SCHEMES / (prefix + '.xml'))
    if colors != palette(SCHEMES / (prefix + 'Attributes.xml')):
        raise ValueError(f'{mode} bundled scheme and default IntelliJ attributes differ')
    if colors.keys() != {key for key, _, _ in CATEGORIES}:
        raise ValueError(f'Unmapped {mode} IntelliJ attributes')
    semantic, lexical = {}, []
    for key, kinds, scopes in CATEGORIES:
        for kind in kinds.split():
            semantic[kind + ':bolt'] = colors[key]
        if scopes:
            lexical.append({'scope': scopes.split(), 'settings': {'foreground': colors[key], 'fontStyle': ''}})
    return {'name': 'Bolt C# ' + mode, 'include': f'./base/{mode.lower()}_plus.json',
            'semanticHighlighting': True, 'semanticTokenColors': semantic, 'tokenColors': lexical}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    for mode in ('Dark', 'Light'):
        path = ROOT / 'themes' / (mode.lower() + '.json')
        data = json.dumps(theme(mode), indent=2) + '\n'
        if args.check:
            if path.read_bytes() != data.encode():
                raise SystemExit(f'{path.name} differs from the IntelliJ scheme')
        else:
            path.write_bytes(data.encode())
