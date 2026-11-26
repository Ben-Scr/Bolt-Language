// Run against the adapter extracted from the VSIX or produced by the build.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { spawn, execFileSync } = require('node:child_process');
const { pathToFileURL, fileURLToPath } = require('node:url');
const { createMessageConnection, StreamMessageReader, StreamMessageWriter } = require('../vscode/node_modules/vscode-jsonrpc/node');
const format = require('../vscode/src/editing.cjs').format;
const bolt = path.resolve(process.env.BOLT_COMPILER || path.join(__dirname, '../../bin/bolt.exe'));
const adapter = process.env.BOLT_VS_ADAPTER || path.join(__dirname, 'obj/adapter/adapter.cjs');
const node = process.env.BOLT_VS_NODE || process.execPath;

async function session(t, compiler, root) {
    const child = spawn(node, [adapter, 'lsp'], { windowsHide: true, stdio: ['pipe','pipe','inherit'],
        env: { ...process.env, BOLT_VS_OPTIONS: JSON.stringify({ compilerPath: compiler }) } });
    const rpc = createMessageConnection(new StreamMessageReader(child.stdout), new StreamMessageWriter(child.stdin));
    t.after(() => { rpc.dispose(); child.kill(); });
    rpc.listen();
    const result = await rpc.sendRequest('initialize', { processId: process.pid, rootUri: pathToFileURL(root).href,
        capabilities: { workspace: { workspaceEdit: { documentChanges: true } } } });
    await rpc.sendNotification('initialized', {});
    return { rpc, capabilities: result.capabilities };
}
async function open(rpc, uri, text) {
    await rpc.sendNotification('textDocument/didOpen', { textDocument: { uri, languageId: 'bolt', text, version: 1 } });
}
function editorEvent(rpc) {
    return new Promise(resolve => {
        const subscription = rpc.onNotification('bolt/editor', value => { subscription.dispose(); resolve(value); });
    });
}
function colorRuns(data, text) {
    assert.equal(data.hash, require('node:crypto').createHash('sha256').update(text, 'utf16le').digest('base64'));
    assert.equal(data.runs.length % 3, 0);
    let end = 0;
    const runs = [];
    for (let i = 0; i < data.runs.length; i += 3) {
        const [gap, length, index] = data.runs.slice(i, i + 3);
        assert.ok(gap >= 0 && length > 0 && end + gap + length <= text.length);
        const colors = data.palette[index];
        assert.equal(colors.length, 2);
        for (const color of colors) assert.match(color, /^#[0-9a-f]{6}$/i);
        runs.push({ word: text.slice(end + gap, end + gap + length), colors });
        end += gap + length;
    }
    return runs;
}
test('packaged editor with the real Bolt SDK', { timeout: 30000 }, async t => {
    const root = await fs.mkdtemp(path.join(os.tmpdir(), 'Bolt VS ü '));
    t.after(() => fs.rm(root, { recursive: true, force: true }));
    const project = path.join(root, 'Sample');
    execFileSync(bolt, ['init', project]);
    const file = path.join(project, 'src/Main.bolt'), uri = pathToFileURL(file).href;
    const text = 'import Console\nstatic class Program {\npublic:\n int Main() {\n int value = 7;\n Console.WriteLine(value);\n return 0;\n }\n}\n';
    await fs.writeFile(file, text);
    const { rpc, capabilities } = await session(t, bolt, project);
    assert.ok(capabilities.definitionProvider);
    const event = editorEvent(rpc);
    await open(rpc, uri, text);
    const td = { textDocument: { uri } };
    const call = (method, params = {}) => rpc.sendRequest('textDocument/' + method, { ...td, ...params });
    const data = await event;
    const importList = await call('completion', { position: { line: 0, character: 7 } });
    const imports = new Map((importList.items || importList).map(item => [item.label, item]));
    for (const name of ['File', 'FileStream']) {
        const item = imports.get(name);
        assert.equal(item.kind, 7);
        assert.equal(item.detail, 'class System.IO.' + name);
        assert.equal(item.textEdit.newText, name);
    }
    for (const name of ['Ecs', 'System.IO']) assert.equal(imports.get(name).kind, 9);
    const runs = colorRuns(data, text);
    assert.ok(runs.some(r => r.word === 'Main')); assert.ok(data.folds.length > 0); assert.ok(data.entries.length);
    assert.equal((await call('formatting'))[0].newText, format(text));
    assert.ok((await call('foldingRange')).length);
    const pos = { position: { line: 5, character: 20 } };
    assert.match((await call('hover', pos)).contents.value, /value/);
    assert.ok((await call('definition', pos)).some(d => d.uri.toLowerCase() === uri.toLowerCase()));
    assert.ok((await call('references', { ...pos, context: { includeDeclaration: true } })).length >= 2);
    assert.ok(await call('rename', { ...pos, newName: 'amount' }));
    const stdlib = await call('definition', { position: { line: 5, character: 11 } });
    const cached = fileURLToPath(stdlib[0].uri);
    assert.match(await fs.readFile(cached, 'utf8'), /WriteLine/);
    const completions = await call('completion', { position: { line: 4, character: 1 } });
    assert.ok((completions.items || completions).some(i => i.kind === 15 && i.label === 'main'));
    const signatures = await call('signatureHelp', { position: { line: 5, character: 19 } });
    assert.ok(signatures.signatures.length);
    const afterDot = { line: 5, character: 9 }, afterW = { line: 5, character: 10 };
    await rpc.sendNotification('textDocument/didChange', { textDocument: { uri, version: 2 }, contentChanges: [{
        range: { start: afterDot, end: afterDot }, text: 'W' }] });
    const members = await call('completion', { position: afterW });
    assert.ok((members.items || members).some(i => i.label === 'WriteLine'));
    await rpc.sendNotification('textDocument/didChange', { textDocument: { uri, version: 3 }, contentChanges: [{
        range: { start: afterDot, end: afterW }, text: '' }] });
    const findImport = new Promise(resolve => {
        const sub = rpc.onNotification('textDocument/publishDiagnostics', p => {
            if (p.uri.toLowerCase() === uri.toLowerCase() && p.diagnostics.length) { sub.dispose(); resolve(p.diagnostics); }
        });
    });
    const edit = 'Print(Random.Next<int>(0, 5));';
    await rpc.sendNotification('textDocument/didChange', { textDocument: { uri, version: 4 }, contentChanges: [{
        range: { start: { line: 5, character: 1 }, end: { line: 5, character: 26 } }, text: edit }] });
    const diagnostics = await findImport;
    const fixes = await call('codeAction', { range: diagnostics[0].range, context: { diagnostics, only: ['quickfix'] } });
    assert.ok(fixes.some(f => f.title.includes('import Random')));
    const environment = { ...process.env, BOLT_VS_OPTIONS: JSON.stringify({ compilerPath: bolt, profile: 'Release',
        args: ['one two', ''], cwd: 'work dir' }) };
    const plan = op => JSON.parse(execFileSync(node, [adapter, op, file], { env: environment, encoding: 'utf8' }));
    assert.deepEqual(plan('run').args.slice(-5), ['--working-directory', path.join(project, 'work dir'), '--', 'one two', '']);
    assert.ok(plan('build').args.includes('Release'));
    assert.ok(plan('debug').args.includes('--debug-info'));
    environment.BOLT_VS_OPTIONS = JSON.stringify({ compilerPath: bolt, taskTarget: 'Sample', profile: 'Debug' });
    const task = JSON.parse(execFileSync(node, [adapter, 'check', file, root], { env: environment, encoding: 'utf8' }));
    assert.deepEqual(task.args, ['check', project, '--diagnostics', 'msbuild', '--profile', 'Debug']);
    await rpc.sendRequest('shutdown');
    await rpc.sendNotification('exit');
});
test('offline editing shares the formatter and snippets', { timeout: 10000 }, async t => {
    const { rpc, capabilities } = await session(t, path.join(os.tmpdir(), 'missing-bolt-test.exe'), os.tmpdir());
    assert.ok(capabilities.documentFormattingProvider);
    const text = 'class C { public: int X; }\n';
    const td = { textDocument: { uri: 'untitled:Script1.bolt' } };
    const initial = editorEvent(rpc);
    await open(rpc, td.textDocument.uri, text);
    const initialRuns = colorRuns(await initial, text);
    assert.ok(initialRuns.some(r => r.word === 'class'));
    const edited = editorEvent(rpc);
    for (const [version, suffix] of [[2, '// one'], [3, '// two']]) {
        await rpc.sendNotification('textDocument/didChange', { textDocument: { ...td.textDocument, version },
            contentChanges: [{ text: text + suffix }] });
    }
    assert.ok(colorRuns(await edited, text + '// two').some(r => r.word === '// two'));
    const restore = editorEvent(rpc);
    await rpc.sendNotification('textDocument/didChange', { textDocument: { ...td.textDocument, version: 4 },
        contentChanges: [{ text }] });
    await restore;
    assert.equal((await rpc.sendRequest('textDocument/formatting', td))[0].newText, format(text));
    await rpc.sendRequest('shutdown');
    await rpc.sendNotification('exit');
});
