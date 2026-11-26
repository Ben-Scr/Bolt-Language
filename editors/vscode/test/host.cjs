// Run in an isolated VS Code extension host with a writable workspace created by bolt init.
const vscode = require('vscode'),
    assert = require('node:assert/strict'),
    fs = require('node:fs/promises'),
    path = require('node:path');
const exec = (name, ...args) => vscode.commands.executeCommand(name, ...args);
async function until(check, label) {
    const until = Date.now() + 35000;
    while (Date.now() < until) {
        const v = await check();
        if (v) return v;
        await new Promise((r) => setTimeout(r, 150));
    }
    throw Error('Timed out: ' + label);
}
async function run() {
    const ext = vscode.extensions.getExtension('ben-schneider.bolt-language');
    assert.ok(ext, 'installed VSIX is visible');
    assert.ok(vscode.workspace.isTrusted);
    const dir = vscode.workspace.workspaceFolders[0].uri.fsPath;
    const file = vscode.Uri.file(path.join(dir, 'src', 'Main.bolt'));
    const good =
        'import System\n[Reflect] static class Program { public: int Twice(int n) { return n+n; } int Main() { int value = Twice(21); Print(value); return value; } }\n';
    await fs.writeFile(file.fsPath, good);
    const doc = await vscode.workspace.openTextDocument(file);
    const ed = await vscode.window.showTextDocument(doc);
    assert.equal(doc.languageId, 'bolt');
    await (await ext.activate()).ready();
    assert.ok(ext.packageJSON.contributes.breakpoints.some(item => item.language === 'bolt'));
    const breakpoint = new vscode.SourceBreakpoint(new vscode.Location(file, new vscode.Position(1, 0)));
    vscode.debug.addBreakpoints([breakpoint]);
    try {
        assert.ok(vscode.debug.breakpoints.some(item => item.id === breakpoint.id));
    } finally {
        vscode.debug.removeBreakpoints([breakpoint]);
    }
    const info = await exec('bolt.showToolchain');
    assert.equal(info.extension, ext.packageJSON.version);
    assert.equal(await fs.realpath(info.executable), await fs.realpath(process.env.BOLT_COMPILER));
    assert.ok(info.compiler && info.theme);
    const pos = (word, offset = 1, occurrence = 0) =>
        doc.positionAt((occurrence ? doc.getText().lastIndexOf(word) : doc.getText().indexOf(word)) + offset);
    const scratchSource = 'class Draft { public: int Twice(int n) { return n+n; } }';
    const scratch = await vscode.workspace.openTextDocument({ language: 'bolt', content: scratchSource });
    assert.equal(scratch.uri.scheme, 'untitled');
    const scratchUse = scratch.positionAt(scratchSource.lastIndexOf('n;'));
    const scratchDefs = await until(async () => {
        const defs = await exec('vscode.executeDefinitionProvider', scratch.uri, scratchUse);
        return defs?.length && defs;
    }, 'navigation in an unnamed Bolt buffer');
    assert.equal(scratchDefs[0].uri.toString(), scratch.uri.toString());
    assert.equal(scratch.offsetAt(scratchDefs[0].range.start), scratchSource.indexOf('n)'));
    const scratchRename = await exec('vscode.executeDocumentRenameProvider', scratch.uri, scratchUse, 'count');
    assert.equal(scratchRename.get(scratch.uri).length, 3, 'rename edits point to the unnamed buffer');
    const nav = await until(async () => {
        const r = await exec('vscode.executeDefinitionProvider', file, pos('Twice', 2, 1));
        return r?.length && r;
    }, 'definition');
    assert.equal(nav[0].uri.fsPath, file.fsPath);
    assert.ok((await exec('vscode.executeHoverProvider', file, pos('Twice', 2, 1))).length);
    assert.ok((await exec('vscode.executeReferenceProvider', file, pos('Twice'))).length >= 2);
    assert.ok((await exec('vscode.executeDocumentSymbolProvider', file)).length);
    assert.ok((await exec('vscode.provideDocumentSemanticTokens', file))?.data.length);
    const comp = await exec('vscode.executeCompletionItemProvider', file, pos('Twice', 2, 1));
    assert.ok(comp.items.length, 'completion');
    const sig = await exec('vscode.executeSignatureHelpProvider', file, pos('Twice(21', 6, 1), '(');
    assert.ok(sig.signatures.length, 'signature help');
    const changes = await exec(
        'vscode.executeDocumentRenameProvider',
        file,
        pos('Twice', 2, 1),
        'DoubleValue',
    );
    assert.ok(changes.entries().length > 0, 'rename');
    assert.ok(
        (await exec('vscode.executeCodeLensProvider', file)).some((l) => l.command?.command === 'bolt.run'),
        'compiler-confirmed entrypoint lens',
    );
    const fmt = await exec('vscode.executeFormatDocumentProvider', file, { tabSize: 4, insertSpaces: true });
    assert.ok(fmt?.length);
    await ed.edit((b) => {
        for (const edit of fmt) b.replace(edit.range, edit.newText);
    });
    await doc.save();
    assert.ok((await exec('vscode.executeFoldingRangeProvider', file)).length);
    await ed.edit((b) => b.insert(new vscode.Position(0, 0), '@@ @@@\n'));
    await until(
        () =>
            vscode.languages.getDiagnostics(file).some((d) => d.severity === vscode.DiagnosticSeverity.Error),
        'unsaved compiler error',
    );
    await ed.edit((b) =>
        b.replace(new vscode.Range(doc.positionAt(0), doc.positionAt(doc.getText().length)), good),
    );
    await until(
        () =>
            vscode.languages
                .getDiagnostics(file)
                .every((d) => d.severity !== vscode.DiagnosticSeverity.Error),
        'diagnostics clear',
    );
    const refs = await exec('vscode.executeDefinitionProvider', file, pos('Reflect'));
    assert.ok(refs.some((x) => x.uri.scheme === 'bolt-stdlib'));
    const virtual = await vscode.workspace.openTextDocument(
        refs.find((x) => x.uri.scheme === 'bolt-stdlib').uri,
    );
    assert.ok(virtual.getText().includes('ReflectAttribute'), 'embedded stdlib source');
    const libraryPos = virtual.positionAt(virtual.getText().indexOf('ReflectAttribute') + 2);
    assert.ok((await exec('vscode.executeHoverProvider', virtual.uri, libraryPos)).length, 'hover inside embedded stdlib');
    const sampleText = `class Person { public string Name; public int Age; }
struct Point { public int X; public int Y; }
public static class Program {
    public static void Main(string[] args) {
        Print(Random.Next<int>(0, 5));
        Console.Write("Hello World");
    }
}
`;
    const sample = await vscode.workspace.openTextDocument({ language: 'bolt', content: sampleText });
    const importError = await until(() => vscode.languages.getDiagnostics(sample.uri)
        .find(d => d.message.includes('missing import Random')), 'missing Random import');
    const actions = await exec('vscode.executeCodeActionProvider', sample.uri, importError.range, 'quickfix');
    const addRandom = actions.find(action => action.title === 'Add import Random');
    assert.ok(addRandom?.edit && await vscode.workspace.applyEdit(addRandom.edit));
    await until(() => !vscode.languages.getDiagnostics(sample.uri).some(d => d.message.includes('missing import Random')), 'Random import applied');
    const genericSource = good
        .replace('int Twice(', 'T Identity<T>(T arg) { return arg; } int Twice(')
        .replace('Print(value);', 'Identity; Print(value);');
    const endOfDoc = () => new vscode.Range(doc.positionAt(0), doc.positionAt(doc.getText().length));
    await vscode.window.showTextDocument(doc);
    const commentSource = '#define VISIBLE\n/* active\n#if inside the comment\n */\n#if VISIBLE\n// selected\n#else\n// excluded\n/* excluded */\n#endif\n';
    async function checkCommentColors(source, visible) {
        await ed.edit(edit => edit.replace(endOfDoc(), source));
        const legend = await exec('vscode.provideDocumentSemanticTokensLegend', file);
        const tokens = await exec('vscode.provideDocumentSemanticTokens', file);
        const kinds = new Map();
        let line = 0;
        for (let i = 0; i < tokens.data.length; i += 5) {
            line += tokens.data[i];
            kinds.set(line, legend.tokenTypes[tokens.data[i + 3]]);
        }
        for (const i of [1, 2, 3]) assert.equal(kinds.get(i), 'comment', 'active block comment line ' + i);
        assert.equal(kinds.get(5), visible ? 'comment' : 'boltInactiveCode');
        for (const i of [7, 8]) assert.equal(kinds.get(i), visible ? 'boltInactiveCode' : 'comment');
    }
    await checkCommentColors(commentSource, true);
    await checkCommentColors(commentSource.replace('#define VISIBLE', '#undef VISIBLE'), false);
    // showToolchain opens the output panel. Send typing commands to the source editor.
    await exec('workbench.action.closePanel');
    await exec('workbench.action.focusActiveEditorGroup');
    await ed.edit((edit) => edit.replace(endOfDoc(), genericSource));
    const genericCaret = doc.positionAt(doc.getText().lastIndexOf('Identity;') + 'Identity'.length);
    ed.selection = new vscode.Selection(genericCaret, genericCaret);
    await exec('type', { text: '<' });
    await until(() => doc.getText().includes('Identity<>;'), 'auto-pair confirmed generic call');
    await until(
        () => doc.offsetAt(ed.selection.active) === doc.getText().indexOf('>; Print'),
        'caret between generic angles',
    );
    await exec('type', { text: 'int' });
    await exec('type', { text: '>' });
    await until(
        () =>
            doc.getText().includes('Identity<int>;') &&
            doc.offsetAt(ed.selection.active) === doc.getText().indexOf('; Print'),
        'skip owned generic closer',
    );
    const nested = 'import System.Collections.Generic\n' + good.replace('Print(value);', 'List values; Print(value);');
    await ed.edit(edit => edit.replace(endOfDoc(), nested));
    const listCaret = doc.positionAt(nested.indexOf('List values;') + 'List'.length);
    ed.selection = new vscode.Selection(listCaret, listCaret);
    await exec('type', { text: '<' });
    await until(() => doc.getText().includes('List<> values;'), 'outer list pair');
    await exec('type', { text: 'List' });
    await exec('type', { text: '<' });
    await until(() => doc.getText().includes('List<List<>> values;'), 'inner list pair');
    await exec('type', { text: 'int' });
    await exec('type', { text: '>' });
    await exec('type', { text: '>' });
    await until(() => doc.getText().includes('List<List<int>> values;') &&
        doc.offsetAt(ed.selection.active) === doc.getText().indexOf(' values;'), 'both consecutive closers skipped');
    await ed.edit((edit) => edit.replace(endOfDoc(), good.replace('Print(value);', 'value; Print(value);')));
    const comparisonCaret = doc.positionAt(doc.getText().indexOf('value; Print') + 5);
    ed.selection = new vscode.Selection(comparisonCaret, comparisonCaret);
    await exec('type', { text: '<' });
    await exec('vscode.executeHoverProvider', file, pos('Twice'));
    assert.ok(doc.getText().includes('value<; Print'), 'comparison must remain unpaired');
    // A reflected type uses the C++ compatibility backend. Test Build and Run with a native script.
    await ed.edit((b) =>
        b.replace(
            new vscode.Range(doc.positionAt(0), doc.positionAt(doc.getText().length)),
            good.replace('[Reflect] ', ''),
        ),
    );
    async function expectTask(command) {
        let listener, timer;
        try {
            const done = new Promise((resolve, reject) => {
                listener = vscode.tasks.onDidEndTaskProcess((e) => {
                    if (e.execution.task.definition.operation === command) resolve(e.exitCode);
                });
                timer = setTimeout(() => reject(Error('timed out: ' + command)), 120000);
            });
            await exec('bolt.' + command, file);
            assert.equal(await done, command === 'run' ? 42 : 0, command + ' exit');
        } finally {
            listener?.dispose();
            clearTimeout(timer);
        }
    }
    for (const op of ['check', 'build', 'run']) await expectTask(op);
    assert.equal(doc.isDirty, false, 'Run saves sources');
    await exec('bolt.restart');
    await until(
        async () => (await exec('vscode.executeHoverProvider', file, pos('Twice')))?.length,
        'restart and reopen',
    );
    if (process.env.BOLT_VSCODE_TEST_RESULT) {
        await fs.writeFile(process.env.BOLT_VSCODE_TEST_RESULT, 'all checks passed\n', { flag: 'wx' });
    }
    console.log(
        'PASS installed extension: language, hover, definition/references/rename, document symbols, completion, signature, tokens, code lens, formatting/folding, unsaved diagnostics, restart and three real process tasks.',
    );
}
module.exports = { run };
