const test = require('node:test'),
    assert = require('node:assert/strict');
const path = require('node:path'),
    fs = require('node:fs');
const { tokens, format, foldPairs, className } = require('../src/editing.cjs');
const { sourceHash, invocation } = require('../src/sdk.cjs');
const { batchDocumentChanges } = require('../src/document-sync.cjs');
const { memberPreview, predictMember } = require('../src/member-preview.cjs');

test('member lookahead leaves ambiguous identifiers and existing suffixes to the primary server', () => {
    const prefix = 'void F(List<int> values, int valueCount) { return ';
    assert.equal(predictMember(prefix + 'val;', prefix.length + 3), undefined);
    assert.equal(predictMember(prefix + 'values;', prefix.length + 4), undefined);
    assert.deepEqual(predictMember(prefix + 'valueC;', prefix.length + 6),
        { text: prefix + 'valueCount.;', extra: 5 });
});

test('a dot preview requires the exact document, caret and project inputs', async t => {
    const prefix = 'void F(Thing alphaItem) { /* 😀 */ return ';
    function fixture(dirtyPeer = false) {
        let text = prefix + 'alp; }';
        const doc = { uri: { scheme: 'file', toString: () => 'file:///P/a.bolt' }, languageId: 'bolt',
            getText: () => text, lineCount: 1, lineAt: () => ({ range: { end: text.length } }),
            offsetAt: n => n, positionAt: n => ({ line: 0, character: n }) };
        const other = { ...doc, uri: { scheme: 'file', toString: () => 'file:///P/b.bolt' },
            version: 7, isDirty: dirtyPeer, getText: () => 'class NewType {}' };
        let edit, signal, resolve, overlays;
        const disposable = { dispose() {} };
        const workspace = { textDocuments: [doc, other], getWorkspaceFolder: () => ({}),
            onDidChangeTextDocument(fn) { edit = fn; return disposable; },
            onDidCloseTextDocument: () => disposable, onDidSaveTextDocument: () => disposable };
        const api = { workspace, window: { activeTextEditor: { document: doc } } };
        const result = { isIncomplete: false, items: [{ label: 'Member' }] };
        const preview = memberPreview(api, 'sdk', () => ({ name: 'Bolt Language Server' }),
            (_exe, _cwd, _init, snapshot, abort) => {
                assert.equal(snapshot.text, prefix + 'alphaItem.; }');
                overlays = snapshot.overlays;
                signal = abort;
                return new Promise(done => { resolve = () => done(result); });
            });
        preview.feature.initialize({ completionProvider: {} });
        t.after(() => preview.feature.clear());
        edit({ document: doc, contentChanges: [{ rangeOffset: prefix.length + 2, rangeLength: 0, text: 'p' }] });
        const params = { context: { triggerKind: 2, triggerCharacter: '.' }, textDocument: { uri: doc.uri.toString() },
            position: { line: 0, character: prefix.length + 'alphaItem.'.length } };
        const take = () => preview.take(params);
        const enterDot = () => { text = prefix + 'alphaItem.; }'; };
        return { take, enterDot, other, params, finish: async () => { resolve(); await Promise.resolve(); },
            invalidate: preview.clear, signal, overlays, result };
    }
    async function completed(dirtyPeer = false) {
        const f = fixture(dirtyPeer);
        await f.finish();
        return f;
    }
    const differentText = await completed();
    assert.equal(differentText.take(), undefined, 'text differs');
    const differentCaret = await completed();
    differentCaret.enterDot();
    differentCaret.params.position.character--;
    assert.equal(differentCaret.take(), undefined, 'caret differs');
    const newOverlay = await completed();
    newOverlay.enterDot();
    newOverlay.other.isDirty = true;
    assert.equal(newOverlay.take(), undefined, 'new unsaved dependency');
    const ready = await completed();
    ready.enterDot();
    assert.deepEqual(ready.take(), ready.result);
    assert.equal(ready.take(), undefined, 'a result is consumed once');

    const stale = fixture();
    stale.enterDot();
    assert.equal(stale.take(), undefined, 'unfinished lookahead never delays normal completion');
    assert.ok(stale.signal.aborted);
    await stale.finish();
    assert.equal(stale.take(), undefined, 'a late reply cannot reappear');
    const invalidated = fixture();
    invalidated.invalidate();
    invalidated.enterDot();
    await invalidated.finish();
    assert.equal(invalidated.take(), undefined, 'workspace changes discard in-flight results');

    const peer = fixture(true);
    assert.deepEqual(peer.overlays, [{ uri: 'file:///P/b.bolt', languageId: 'bolt', version: 7, text: 'class NewType {}' }]);
    peer.enterDot();
    await peer.finish();
    peer.other.version++;
    assert.equal(peer.take(), undefined, 'the dependency changed after its snapshot');
});

test('consecutive incremental edits reach the server in order before requests and saves', async () => {
    const sync = batchDocumentChanges();
    const sent = [];
    const write = async (type, params) => { sent.push([type, params]); };
    function change(uri, version, contentChanges) {
        return sync.sendNotification('textDocument/didChange', write, { textDocument: { uri, version }, contentChanges });
    }
    const first = { text: '😀x' };
    const second = { range: { start: { line: 0, character: 2 }, end: { line: 0, character: 3 } }, text: 'y' };
    const edits = [change('a', 2, [first]), change('a', 3, [second]), change('b', 2, [first])];
    assert.equal(sent.length, 1, 'switching documents flushes the preceding edits');
    const request = sync.sendRequest('textDocument/completion', {}, undefined, write);
    const third = change('b', 3, [second]);
    await sync.sendNotification('textDocument/didSave', write, { textDocument: { uri: 'b' } });
    await Promise.all([...edits, request, third]);
    assert.deepEqual(sent.map(([type]) => type), ['textDocument/didChange','textDocument/didChange',
        'textDocument/completion','textDocument/didChange','textDocument/didSave']);
    assert.deepEqual(sent[0][1], { textDocument: { uri: 'a', version: 3 }, contentChanges: [first, second] });
    assert.deepEqual(sent[1][1].textDocument, { uri: 'b', version: 2 });
});

test('pending completion sees contiguous typing before analysis and flushes before a fallback', async () => {
    const sync = batchDocumentChanges(() => true), sent = [];
    const doc = { uri: 'file:///p.bolt' };
    const write = async (type, params) => { sent.push([type, params]); };
    sync.sendNotification('textDocument/didOpen', write, { textDocument: { ...doc, version: 9 } });
    const pos = character => ({ line: 2, character });
    function insert(text, character, version) {
        return sync.sendNotification('textDocument/didChange', write, { textDocument: { ...doc, version },
            contentChanges: [{ text, range: { start: pos(character), end: pos(character) } }] });
    }
    const a = insert('widget', 4, 12), b = insert('.', 10, 13);
    const params = { textDocument: doc, position: pos(11) };
    const preview = await sync.sendRequest('textDocument/completion', params, {}, async (method, p) => {
        assert.equal(method, 'textDocument/boltPendingCompletion');
        assert.deepEqual(p.edit, { version: 9, start: pos(4), end: pos(4), text: 'widget.' });
        return { items: ['Boost'] };
    });
    assert.deepEqual(preview.items, ['Boost']);
    assert.equal(sent.length, 1);
    await sync.sendRequest('textDocument/completion', params, {}, async (method, p) => {
        sent.push([method, p]); return null;
    });
    await Promise.all([a, b]);
    assert.deepEqual(sent.map(x => x[0]), ['textDocument/didOpen', 'textDocument/boltPendingCompletion',
        'textDocument/didChange', 'textDocument/completion']);
});

test('backspacing and retyping a member keeps the replacement anchored to the synced version', async () => {
    const sync = batchDocumentChanges(() => true);
    const pos = character => ({ line: 1, character });
    const doc = { uri: 'file:///p.bolt' };
    const write = async () => {};
    sync.sendNotification('textDocument/didOpen', write, { textDocument: { ...doc, version: 2 } });
    // "old" occupies columns 5..8. Replace it with "items." using several edits.
    const edit = (a, b, text) => ({ range: { start: pos(a), end: pos(b) }, text });
    const delivery = sync.sendNotification('textDocument/didChange', write, { textDocument: { ...doc, version: 7 },
        contentChanges: [edit(7, 8, ''), edit(5, 7, 'itemz'), edit(9, 10, 's'), edit(10, 10, '.')] });
    const params = { textDocument: doc, position: pos(11) };
    await sync.sendRequest('textDocument/completion', params, {}, async (method, p) => {
        assert.equal(method, 'textDocument/boltPendingCompletion');
        assert.deepEqual(p.edit, { version: 2, start: pos(5), end: pos(8), text: 'items.' });
        return { items: [] };
    });
    sync.flush();
    await delivery;
});

test('completion read-ahead uses the actual first letter and the exact following dot without delivering drafts', async () => {
    const requests = [], writes = [];
    const uri = 'file:///lookahead.bolt';
    let text = '', version = 4;
    const pos = character => ({ line: 2, character });
    const scope = { items: [{ label: 'velocity' }, { label: 'values' }] };
    const members = { items: [{ label: 'Add' }] };
    const sync = batchDocumentChanges(() => true, {
        word: () => text,
        query: async params => {
            requests.push(params);
            return params.edit.text.endsWith('.') ? members : scope;
        },
    });
    const send = async type => writes.push(type);
    sync.sendNotification('textDocument/didOpen', send, { textDocument: { uri, version } });
    function type(char) {
        const start = pos(7 + text.length);
        text += char;
        sync.sendNotification('textDocument/didChange', send, { textDocument: { uri, version: ++version },
            contentChanges: [{ text: char, range: { start, end: start } }] });
    }
    const take = () => sync.takeCompletion(uri, version, pos(7 + text.length));
    type('v');
    assert.deepEqual(await take(), scope);
    assert.equal(sync.takeCompletion(uri, version, pos(7)), undefined);
    for (const c of 'alues') type(c);
    assert.equal(take(), undefined, 'member prediction belongs only to the next dot');
    type('.');
    assert.deepEqual(await take(), members);
    assert.deepEqual(requests.map(r => [r.edit.version, r.edit.text, r.position.character]), [[4, 'v', 8], [4, 'values.', 14]]);
    assert.deepEqual(writes, ['textDocument/didOpen']);
    sync.invalidateCompletion();
    assert.equal(take(), undefined);
    sync.flush();
});

test('read-ahead drops obsolete replies, bounds concurrent work and leaves failed predictions to normal completion', async () => {
    let resolve, requests = 0;
    const sync = batchDocumentChanges(() => true, { word: () => 'a', query: () => {
        requests++;
        return new Promise(done => { resolve = done; });
    } });
    const send = async () => {};
    for (const uri of ['a', 'b']) sync.sendNotification('textDocument/didOpen', send, { textDocument: { uri, version: 1 } });
    function insert(uri, version, char) {
        const p = { line: 0, character: 4 };
        sync.sendNotification('textDocument/didChange', send, { textDocument: { uri, version },
            contentChanges: [{ text: char, range: { start: p, end: p } }] });
    }
    const caret = { line: 0, character: 5 };
    insert('a', 2, 'a');
    const stale = sync.takeCompletion('a', 2, caret);
    await Promise.resolve();
    insert('b', 2, 'a');
    resolve({ items: [{ label: 'Old' }] });
    assert.equal(await stale, undefined);
    assert.equal(sync.takeCompletion('b', 2, caret), undefined);
    assert.equal(requests, 1);
    sync.flush();
    insert('a', 3, 'a');
    const miss = sync.takeCompletion('a', 3, caret);
    await Promise.resolve();
    resolve(null);
    assert.equal(await miss, null, 'the compiler requested a synced document for this exact edit');
    sync.flush();
    const methods = [];
    await sync.sendRequest('textDocument/completion', { textDocument: { uri: 'a' }, position: caret }, {}, async method => {
        methods.push(method);
        return null;
    });
    assert.deepEqual(methods, ['textDocument/completion']);
});

test('idle edits are delivered and failed writes reject every edit in a batch', async () => {
    const sync = batchDocumentChanges();
    const type = { method: 'textDocument/didChange' };
    const change = version => ({ textDocument: { uri: 'a', version }, contentChanges: [{ text: 'x' }] });
    let deliveries = 0;
    await sync.sendNotification(type, async () => { deliveries++; }, change(1));
    assert.equal(deliveries, 1);
    const fail = async () => { throw Error('write failed'); };
    const a = sync.sendNotification(type, fail, change(2));
    const b = sync.sendNotification(type, fail, change(3));
    sync.flush();
    await Promise.all([assert.rejects(a, /write failed/), assert.rejects(b, /write failed/)]);
});

test('rapid edits reset the idle timer but still reach the compiler during continuous typing', async t => {
    t.mock.timers.enable({ apis: ['setTimeout'] });
    const sync = batchDocumentChanges();
    const versions = [];
    const write = async (_type, params) => versions.push(params.textDocument.version);
    function change(version) {
        return sync.sendNotification('textDocument/didChange', write, {
            textDocument: { uri: 'a', version }, contentChanges: [{ text: 'x' }],
        });
    }
    const sent = change(1);
    for (let version = 2; version <= 10; version++) {
        t.mock.timers.tick(60);
        change(version);
    }
    assert.deepEqual(versions, []);
    t.mock.timers.tick(60);
    await sent;
    assert.deepEqual(versions, [10]);
    const last = change(11);
    t.mock.timers.tick(120);
    await last;
    assert.deepEqual(versions, [10, 11]);
});

test('highlighting shares pending edits with dot completion and discards obsolete work', async () => {
    const { intellijColors } = require('../src/intellij-colors.cjs');
    const sync = batchDocumentChanges();
    const colors = intellijColors(undefined, undefined, sync.pendingWrite);
    const provide = colors.middleware.provideDocumentSemanticTokens;
    const sent = [];
    const send = async type => { sent.push(type); };
    const doc = { version: 1 };
    const change = () => sync.sendNotification('textDocument/didChange', send, {
        textDocument: { uri: 'a', version: doc.version }, contentChanges: [{ text: 'x' }],
    });
    const highlight = () => provide(doc, { isCancellationRequested: false },
        () => send('textDocument/semanticTokens/full'));
    const first = change();
    const obsolete = highlight();
    doc.version++;
    const second = change();
    const current = highlight();
    assert.deepEqual(sent, []);
    const completion = sync.sendRequest('textDocument/completion', {}, undefined, send);
    await Promise.all([first, second, obsolete, current, completion]);
    assert.deepEqual(sent, ['textDocument/didChange', 'textDocument/completion', 'textDocument/semanticTokens/full']);
});
test('format matches common Allman layout and leaves tokens, literals, preprocessor branches intact', () => {
    const original = 'static class Program{public: void Main(){Print($"brace {ToString("}")}"); /* } */}}';
    const expected =
        'static class Program\n{\npublic:\n    void Main()\n    {\n        Print($"brace {ToString("}")}"); /* } */\n    }\n}\n';
    assert.equal(format(original), expected);
    for (const x of [
        expected,
        '// comment {\r\n#define X\r\nstruct S{\r\n#if X\r\nint a;\r\n#else\r\nint b;\r\n#endif\r\n}',
        'if(x)if(y) f();else g();',
        'int x = $"abc {foo(/* } */ 1)}";',
        'class A{ /* unfinished',
    ]) {
        const result = format(x);
        assert.equal(format(result), result);
        assert.deepEqual(
            tokens(result).map((t) => t.word),
            tokens(x).map((t) => t.word),
        );
    }
    assert.equal(foldPairs(original).length, 2);
});
test('saved entrypoint hashes match the compiler for raw BOM, UTF-8 and binary bytes', () => {
    function reference(bytes) {
        let hash = 14695981039346656037n;
        for (const byte of bytes) hash = BigInt.asUintN(64, (hash ^ BigInt(byte)) * 1099511628211n);
        return hash.toString();
    }
    const sequence = Uint8Array.from({ length: 8192 }, (_, i) => (i * 157 + (i >>> 4)) & 255);
    for (const bytes of [Buffer.alloc(0), Buffer.from('\uFEFFGrüße 🌍\r\n'), sequence, sequence.subarray(39, 4111)]) {
        assert.equal(sourceHash(bytes), reference(bytes));
    }
});
test('class name validation and arguments with whitespace survive without shell parsing', () => {
    assert.equal(className('Player.bolt'), 'Player');
    for (const s of ['a/b', 'class', '../tmp', '']) assert.equal(className(s), null);
    assert.deepEqual(
        invocation('run', '/space here/file.bolt', { profile: 'Release', args: ['a & b', ''] }),
        [
            'run',
            '/space here/file.bolt',
            '--diagnostics',
            'msbuild',
            '--profile',
            'Release',
            '--',
            'a & b',
            '',
        ],
    );
    assert.equal(sourceHash(Buffer.from('')), '14695981039346656037');
});
test('TextMate grammar tokenizes Bolt code, escapes, comments and nested interpolation with Oniguruma', async () => {
    const tm = require('vscode-textmate'),
        onig = require('vscode-oniguruma');
    const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm'));
    await onig.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
    const registry = new tm.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: (v) => new onig.OnigScanner(v),
            createOnigString: (v) => new onig.OnigString(v),
        }),
        loadGrammar: async () =>
            JSON.parse(fs.readFileSync(path.resolve(__dirname, '../syntaxes/bolt.tmLanguage.json'), 'utf8')),
    });
    const grammar = await registry.loadGrammar('source.bolt');
    function scopesAt(line, word) {
        const index = line.indexOf(word);
        assert.notEqual(index, -1);
        return grammar.tokenizeLine(line).tokens.find((t) => t.startIndex <= index && index < t.endIndex).scopes;
    }
    for (const [line, word, scope] of [
        ['if (x) return x;', 'return', 'keyword.control.bolt'],
        ['  #if Release && Linux', 'Linux', 'meta.preprocessor.bolt'],
        ['public struct Point {', 'struct', 'keyword.other.bolt'],
        ['float v = .125e-2f;', '.125e-2f', 'constant.numeric.bolt'],
        ['int[] a;', ']', 'punctuation.section.brackets.bolt'],
        ['Print($"value {Format("q}", 2)}");', '2', 'string.quoted.double.interpolated.bolt'],
        ['Print($"value {Format("q}", 2)}");', ');', 'punctuation.section.parentheses.bolt'],
        ['foreach in sbyte', 'sbyte', 'variable.other.bolt'],
    ]) assert.ok(scopesAt(line, word).includes(scope), line + ' → ' + word);
    // IntelliJ's lexer gives the whole interpolated literal the string color.
    const nested = 'Print($"{{9}} {value + 17 /* } */ + $"inner {3}"} tail");';
    for (const word of ['9', 'value', '+', '17', '/* } */', 'inner', '3', 'tail']) {
        assert.equal(scopesAt(nested, word).at(-1), 'string.quoted.double.interpolated.bolt', word);
    }
    const continued = grammar.tokenizeLine('Print($"sum {1 + // }');
    const continuation = grammar.tokenizeLine('2} text");', continued.ruleStack);
    assert.equal(continuation.tokens[0].scopes.at(-1), 'string.quoted.double.interpolated.bolt');
    assert.equal(continuation.tokens.at(-1).scopes.at(-1), 'punctuation.separator.bolt');
    const first = grammar.tokenizeLine('/* multiline');
    assert.ok(grammar.tokenizeLine('still comment */', first.ruleStack).tokens[0].scopes.includes('comment.block.bolt'));
    for (const [line, scope] of [
        ['public class Ship {', 'variable.other.bolt'],
        ['#if Debug', 'meta.preprocessor.bolt'],
        ['int x = 42;', 'constant.numeric.bolt'],
        ["char c = 'a';", 'string.quoted.single.bolt'],
        ['Print($"value {1+2}");', 'string.quoted.double.interpolated.bolt'],
        ['// next', 'comment.line.double-slash.bolt'],
    ]) {
        assert.ok(
            grammar.tokenizeLine(line).tokens.some((t) => t.scopes.some((s) => s.includes(scope))),
            line,
        );
    }
});


test('IntelliJ semantic refinement uses the server legend and UTF-16 positions, including after unsaved edits', async () => {
    const { intellijColors } = require('../src/intellij-colors.cjs');
    const colors = intellijColors();
    const original = ['method', 'identifier', 'type', 'comment', 'keyword', 'variable'];
    const capabilities = { semanticTokensProvider: { full: true, legend: { tokenTypes: original, tokenModifiers: [] } } };
    colors.feature.preInitialize(capabilities);
    assert.equal(original.length, 6);
    const legend = capabilities.semanticTokensProvider.legend.tokenTypes;
    const lines = ['// 😀', 'if (int x) return Type;', '  /* active', '    second */'];
    const spans = [[0,0,5,3], [1,0,2,4], [0,4,3,2], [0,4,1,5], [0,3,6,4], [0,7,4,2], [1,2,9,3], [1,0,13,3]];
    const raw = { data: new Uint32Array(spans.flatMap(t => [...t, 0])) };
    const doc = { version: 2, lineAt: n => ({ text: lines[n] }) };
    const token = { isCancellationRequested: false };
    const provider = colors.middleware.provideDocumentSemanticTokens;
    const result = await provider(doc, token, async () => raw);
    assert.deepEqual(spans.map((_, i) => legend[result.data[i * 5 + 3]]),
        ['comment', 'boltControlFlow', 'boltPrimitive', 'variable', 'boltControlFlow', 'type', 'comment', 'comment']);
    assert.equal(await provider(doc, token, async () => { doc.version++; return raw; }), undefined);
    assert.equal(await provider(doc, { isCancellationRequested: true }, async () => raw), undefined);
});

test('inactive modifier distinguishes active block continuations from disabled commented lines', async () => {
    const { intellijColors } = require('../src/intellij-colors.cjs');
    const colors = intellijColors();
    const capabilities = { semanticTokensProvider: { full: true, legend: { tokenTypes: ['comment'], tokenModifiers: ['declaration', 'boltInactive'] } } };
    colors.feature.preInitialize(capabilities);
    const doc = { version: 1, lineAt() { throw Error('explicit modifier needs no document scan'); } };
    const raw = { data: new Uint32Array([0,0,7,0,0, 1,0,9,0,0, 1,0,11,0,2]) };
    const result = await colors.middleware.provideDocumentSemanticTokens(doc, { isCancellationRequested: false }, async () => raw);
    const types = capabilities.semanticTokensProvider.legend.tokenTypes;
    assert.deepEqual([3,8,13].map(i => types[result.data[i]]), ['comment','comment','boltInactiveCode']);
});

test('legacy comment colors follow compiler directives, project symbols and file-local defines', async () => {
    const { intellijColors } = require('../src/intellij-colors.cjs');
    const colors = intellijColors(undefined, async () => ({ ProjectSymbol: true, Debug: true }));
    const caps = { semanticTokensProvider: { full: true, legend: { tokenTypes: ['comment', 'macro'], tokenModifiers: [] } } };
    colors.feature.preInitialize(caps);
    const lines = ['#define OFF false', '#if OFF', '// disabled', '#elif defined(OFF) && (!OFF || Missing) && ProjectSymbol',
        '/* active', '#if ignored inside comment', ' */', '#ifndef Debug', '/* disabled */', '#else', '// active',
        '#endif', '#else', '// disabled', '#endif', '#undef OFF', '#ifdef OFF', '// disabled', '#endif'];
    const comments = new Map([[2, false], [4, true], [5, true], [6, true], [8, false], [10, true], [13, false], [17, false]]);
    const data = new Uint32Array(lines.flatMap((line, i) => [i ? 1 : 0, 0, line.length, comments.has(i) ? 0 : 1, 0]));
    const doc = { version: 1, lineAt: i => ({ text: lines[i] }) };
    const result = await colors.middleware.provideDocumentSemanticTokens(doc, {}, async () => ({ data }));
    for (const [i, active] of comments)
        assert.equal(caps.semanticTokensProvider.legend.tokenTypes[result.data[i*5+3]], active ? 'comment' : 'boltInactiveCode', lines[i]);
});

test('IntelliJ colors survive partial views, semantic overrides and multiline literals', () => {
    const { scanUntil, paintRange } = require('../src/editor-palette.cjs');
    const source = 'if (ifé) Print($"left {x + 2} right");\r\n#if Debug\r\n/* two\r\nlines */';
    const lexical = [];
    const cursor = scanUntil(source, 0, source.indexOf('left'), lexical);
    scanUntil(source, cursor, source.length, lexical);
    const start = source.indexOf('left'), x = source.indexOf('x +'), end = source.indexOf('right') + 2;
    const tokens = [];
    paintRange(lexical, [{ start: x, end: x + 1, kind: 'parameter' }], start, end,
        (a, b, kind) => tokens.push([source.slice(a, b), kind]));
    assert.deepEqual(tokens, [
        ['left {', 'string.quoted.double.interpolated.bolt'], ['x', 'parameter'],
        [' + 2} ri', 'string.quoted.double.interpolated.bolt'],
    ]);
    assert.equal(lexical.find(span => span.start === 4).kind, 'variable.other.bolt');
    assert.equal(lexical.at(-2).kind, 'meta.preprocessor.bolt');
    assert.equal(source.slice(lexical.at(-1).start, lexical.at(-1).end), '/* two\r\nlines */');
});

test('unchanged decoration groups can skip rendering only outside rebased lines', () => {
    const { unchangedRanges } = require('../src/editor-palette.cjs');
    const range = (start, end) => ({
        start: { line: start }, end: { line: end },
        isEqual(other) { return other.start.line === start && other.end.line === end; },
    });
    const previous = [range(1, 2), range(8, 8)];
    const same = [range(1, 2), range(8, 8)];
    assert.ok(unchangedRanges(previous, same, { first: 4, last: 4 }));
    assert.ok(unchangedRanges(previous, same));
    assert.ok(!unchangedRanges(previous, same, { first: 4, last: Infinity }));
    assert.ok(!unchangedRanges(previous, same, { first: 2, last: 3 }));
    assert.ok(!unchangedRanges(previous, [range(1, 2), range(9, 9)]));
    assert.ok(!unchangedRanges(undefined, same));
});
