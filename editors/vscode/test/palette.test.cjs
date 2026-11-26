const test = require('node:test');
const assert = require('node:assert/strict');
const { editorPalette, scanUntil, rewindLexical, semanticViewport } = require('../src/editor-palette.cjs');
const theme = require('../themes/dark.json');

class Position {
    constructor(line, character) { Object.assign(this, { line, character }); }
}
class Range {
    constructor(start, end) { Object.assign(this, { start, end }); }
    isEqual(other) { return this.start.line === other.start.line && this.end.line === other.end.line &&
        this.start.character === other.start.character && this.end.character === other.end.character; }
}
const range = (l, a, b) => new Range(new Position(l, a), new Position(l, b));

test('coalesced edits preserve the lexical prefix and extend a multiline token at EOF', () => {
    let text = 'int x;\r\n/* first\r\nsecond\r\n', lexical = [];
    const entry = { lexical, cursor: scanUntil(text, 0, text.length, lexical) };
    const prefix = lexical[0];
    rewindLexical(entry, text.length);
    text += 'last';
    rewindLexical(entry, text.lastIndexOf('\n') + 1);
    text += ' */ return';
    entry.cursor = scanUntil(text, entry.cursor, text.length, lexical);
    const fresh = [];
    scanUntil(text, 0, text.length, fresh);
    assert.equal(lexical[0], prefix);
    assert.deepEqual(lexical, fresh);
    assert.equal(text.slice(lexical.at(-2).start, lexical.at(-2).end), '/* first\r\nsecond\r\nlast */');
});

test('semantic viewport decodes skipped lines, UTF-16 deltas and overlapping scrolls', () => {
    const tokens = new Uint32Array([
        0, 2, 3, 1, 0,  0, 5, 2, 0, 0,
        3, 1, 2, 0, 0,  0, 3, 5, 0, 0,
        2, 4, 1, 0, 0,
    ]);
    const called = [];
    const visible = semanticViewport(tokens, ['method', 'ignored'], new Set(['method']), line => {
        called.push(line);
        return [0, 14, 15, 28, 40, 41][line];
    });
    const view = (a, b) => ({ start: { line: a }, end: { line: b } });
    const first = visible(view(2, 3));
    assert.deepEqual(called, [3]);
    assert.deepEqual(first, [{ start: 29, end: 31, kind: 'method' }, { start: 32, end: 37, kind: 'method' }]);
    const scrolled = visible(view(3, 5));
    assert.equal(scrolled[0], first[0]);
    assert.deepEqual(scrolled[2], { start: 45, end: 46, kind: 'method' });
    assert.deepEqual(visible(view(0, 1)), [{ start: 7, end: 9, kind: 'method' }]);
    assert.deepEqual(called, [3, 5, 0]);
});

test('split editors keep their clipped colors through unsaved edits, visibility and profile changes', t => {
    const literal = 'var s = $"😀 {Run()}";';
    const col = literal.indexOf('Run'), lines = ['/* 😀 */', literal, 'return s;', ''];
    const doc = {
        languageId: 'bolt', version: 1, getText: () => lines.join('\r\n'),
        offsetAt: p => lines.slice(0, p.line).reduce((n, line) => n + line.length + 2, 0) + p.character,
        positionAt(n) {
            for (let i = 0; i < lines.length; i++) {
                if (n <= lines[i].length) return new Position(i, n);
                n -= lines[i].length + 2;
            }
            throw Error('Offset beyond fixture');
        },
    };
    const editor = r => ({
        document: doc, visibleRanges: [r], groups: new Map(),
        setDecorations(style, ranges) { this.groups.set(style, ranges); },
    });
    const full = editor(range(1, 0, literal.length)), clipped = editor(range(1, col + 1, col + 3));
    let render, onEdit, onVisibility, onTheme;
    t.mock.method(global, 'setTimeout', callback => { render = callback; return 1; });
    const window = {
        visibleTextEditors: [clipped, full], activeColorTheme: { kind: 2 },
        createTextEditorDecorationType: options => options.dark.color,
        onDidChangeVisibleTextEditors(f) { onVisibility = f; },
        onDidChangeActiveColorTheme(f) { onTheme = f; },
        onDidChangeTextEditorVisibleRanges() {},
    };
    const workspace = {
        getConfiguration: () => ({ get: () => undefined }),
        onDidChangeTextDocument(f) { onEdit = f; }, onDidChangeConfiguration() {},
    };
    const palette = editorPalette({ workspace, window, Range, Position, ColorThemeKind: { Light: 1, Dark: 2 } }, []);
    const method = theme.semanticTokenColors['method:bolt'];
    const type = theme.semanticTokenColors['class:bolt'];
    function semantic(line, kind) {
        palette.setSemantic(doc, Uint32Array.of(line, col, 3, 0, 0), [kind]);
        render();
    }
    semantic(1, 'method');
    assert.deepEqual(full.groups.get(method), [range(1, col, col + 3)]);
    assert.deepEqual(clipped.groups.get(method), [range(1, col + 1, col + 3)]);
    window.visibleTextEditors = [full];
    onVisibility();
    lines.unshift('// inserted');
    doc.version++;
    onEdit({ document: doc, contentChanges: [{ range: range(0, 0, 0), rangeOffset: 0, text: '// inserted\r\n' }] });
    full.visibleRanges = [range(2, 0, literal.length)];
    render();
    assert.deepEqual(full.groups.get(method), []);

    semantic(2, 'class');
    window.visibleTextEditors = [full, clipped];
    clipped.visibleRanges = [range(2, col, col + 1)];
    onVisibility();
    render();
    assert.deepEqual(clipped.groups.get(method), []);
    assert.deepEqual(full.groups.get(type), [range(2, col, col + 3)]);
    assert.deepEqual(clipped.groups.get(type), [range(2, col, col + 1)]);
    palette.reset();
    render();
    assert.deepEqual(clipped.groups.get(type), []);
    assert.deepEqual(full.groups.get(type), []);
    window.activeColorTheme.kind = 3;
    onTheme();
    render();
    assert.ok([...full.groups.values(), ...clipped.groups.values()].every(rs => rs.length === 0));
});

test('ordinary completion edits yield painting time and newly colored words preempt that delay', t => {
    let line = '    ', onEdit, now = 0;
    t.mock.method(performance, 'now', () => now);
    t.mock.timers.enable({ apis: ['setTimeout'] });
    function tick(ms) { now += ms; t.mock.timers.tick(ms); }
    const doc = {
        languageId: 'bolt', version: 1, lineAt: () => ({ text: line }), getText: () => line,
        offsetAt: p => p.character, positionAt: n => new Position(0, n),
    };
    const painted = [];
    const editor = {
        document: doc, get visibleRanges() { return [range(0, 0, line.length)]; },
        setDecorations(style, ranges) { painted.push([style, ranges.map(r => [r.start.character, r.end.character])]); },
    };
    const noop = () => {};
    const window = {
        visibleTextEditors: [editor], activeColorTheme: { kind: 2 },
        createTextEditorDecorationType: opts => opts.dark.color,
        onDidChangeVisibleTextEditors: noop, onDidChangeActiveColorTheme: noop, onDidChangeTextEditorVisibleRanges: noop,
    };
    const workspace = {
        getConfiguration: () => ({ get: noop }),
        onDidChangeTextDocument(f) { onEdit = f; }, onDidChangeConfiguration: noop,
    };
    editorPalette({ workspace, window, Range, Position, ColorThemeKind: { Light: 1, Dark: 2 } }, []);
    function edit(start, end, text) {
        line = line.slice(0, start) + text + line.slice(end);
        doc.version++;
        onEdit({ document: doc, contentChanges: [{ range: range(0, start, end), rangeOffset: start, text }] });
    }
    tick(50);
    edit(4, 4, 'M');
    tick(39);
    assert.deepEqual(painted, []);
    tick(1);
    assert.deepEqual(painted.pop(), ['#D4D4D4', [[4, 5]]]);
    edit(5, 5, '.');
    tick(39);
    assert.deepEqual(painted, []);
    tick(1);
    assert.deepEqual(painted.pop(), ['#D4D4D4', [[4, 5], [5, 6]]]);

    edit(4, 6, 'in');
    tick(50);
    painted.length = 0;
    edit(6, 6, 't');
    tick(16);
    assert.deepEqual(painted.pop(), [theme.semanticTokenColors['boltPrimitive:bolt'], [[4, 7]]]);
    edit(7, 7, 'x'); // A formerly colored keyword becomes an ordinary identifier.
    tick(16);
    assert.deepEqual(painted.pop(), ['#D4D4D4', [[4, 8]]]);

    edit(4, 8, '');
    tick(50);
    painted.length = 0;
    edit(4, 4, 'i');
    tick(6);
    edit(5, 5, 'f'); // 'i' has not been painted yet.
    tick(10);
    assert.deepEqual(painted.pop(), [theme.semanticTokenColors['boltControlFlow:bolt'], [[4, 6]]]);

    edit(4, 6, '/* a */');
    tick(50);
    painted.length = 0;
    edit(8, 8, 'M');
    tick(16);
    assert.deepEqual(painted.pop(), [theme.semanticTokenColors['comment:bolt'], [[4, 12]]]);
});
