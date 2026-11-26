// Visual Studio's LSP client talks to this stdio endpoint. The compiler still owns
// all language analysis, including the contents of embedded library documents.
const { createMessageConnection, StreamMessageReader, StreamMessageWriter, CancellationToken } = require('vscode-jsonrpc/node');
const { TextDocument } = require('vscode-languageserver-types');
const { spawn } = require('node:child_process');
const { fileURLToPath, pathToFileURL } = require('node:url');
const { createHash } = require('node:crypto');
const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const sdk = require('../vscode/src/sdk.cjs');
const editing = require('../vscode/src/editing.cjs');
const { scanUntil, paintRange } = require('../vscode/src/editor-palette.cjs');
const { intellijColors } = require('../vscode/src/intellij-colors.cjs');
const { batchDocumentChanges } = require('../vscode/src/document-sync.cjs');
const dark = require('../vscode/themes/dark.json');
const light = require('../vscode/themes/light.json');
const snippets = require('../vscode/snippets.json');
const legacyImportTypes = require('../intellij/src/main/resources/dev/bolt/idea/core/legacy-import-types.json');
const settings = JSON.parse(process.env.BOLT_VS_OPTIONS || '{}');
const profile = settings.profile || 'Project default';

function colors(theme) {
    const map = Object.fromEntries(Object.entries(theme.semanticTokenColors).map(([k, v]) => [k.split(':')[0], v]));
    for (const { scope, settings } of theme.tokenColors) for (const name of scope) map[name] = settings.foreground;
    return map;
}
const darkColors = colors(dark), lightColors = colors(light);
// Send the two theme colors once per palette entry, not once per occurrence.
const palette = [], colorIds = new Map(), kindIds = new Map();
for (const [kind, darkColor] of Object.entries(darkColors)) {
    const lightColor = lightColors[kind], pair = darkColor + lightColor;
    if (!colorIds.has(pair)) {
        colorIds.set(pair, palette.length);
        palette.push([darkColor, lightColor]);
    }
    kindIds.set(kind, colorIds.get(pair));
}
const hash = text => createHash('sha256').update(text, 'utf16le').digest('base64');
const key = uri => uri.startsWith('file:') ? path.normalize(fileURLToPath(uri)).toLowerCase() : uri;

async function serve() {
    const client = connection(process.stdin, process.stdout);
    const docs = new Map(), timers = new Map(), embedded = new Map(), originals = new Map(), models = new Map();
    const lastEdit = new Map();
    let supportsPendingCompletion = false;
    const sync = batchDocumentChanges(() => supportsPendingCompletion);
    let compiler, child, server, sdkError, cacheDir, types = [], generation = 0, supportsSnippets = false;
    function connection(input, output) {
        return createMessageConnection(new StreamMessageReader(input), new StreamMessageWriter(output));
    }
    function relay(method, params, token = CancellationToken.None) {
        if (server) return sync.sendRequest(method, params, token, (m,p,t) => server.sendRequest(m,p,t));
        return Promise.resolve(null);
    }
    async function project(doc) {
        if (!doc.uri.startsWith('file:') || originals.has(key(doc.uri))) return {};
        const p = await sdk.projectFor(fileURLToPath(doc.uri));
        if (compiler && p.marker === 'bolt.proj') {
            if (!models.has(p.root)) models.set(p.root, sdk.projectInfo(compiler, p.root, profile).catch(() => null));
            return { ...p, model: await models.get(p.root) };
        }
        return p;
    }
    const coloring = intellijColors(null, async doc => (await project(doc)).model?.conditional_symbols ||
        { Windows: true, [process.arch === 'arm64' ? 'ARM64' : 'X64']: true, [profile === 'Release' ? 'Release' : 'Debug']: true });
    async function libraryFile(uri) {
        if (!embedded.has(uri)) embedded.set(uri, (async () => {
            const value = await relay('bolt/virtualDocument', { uri });
            if (typeof value?.text !== 'string') throw Error('Missing embedded Bolt source: ' + uri);
            cacheDir ??= fs.mkdtemp(path.join(os.tmpdir(), 'Bolt-VS-stdlib-'));
            const dir = path.join(await cacheDir, createHash('sha256').update(uri).digest('hex'));
            await fs.mkdir(dir, { recursive: true });
            const name = path.posix.basename(uri).replace(/[^A-Za-z0-9._-]/g, '_');
            const file = path.join(dir, /\.bolt$/i.test(name) ? name : 'Source.bolt');
            await fs.writeFile(file, value.text, { flag: 'wx' });
            await fs.chmod(file, 0o444);
            const local = pathToFileURL(file).href;
            originals.set(key(local), uri);
            return local;
        })().catch(e => { embedded.delete(uri); throw e; }));
        return embedded.get(uri);
    }
    async function locations(value) {
        if (Array.isArray(value)) return Promise.all(value.map(locations));
        if (value && typeof value === 'object') {
            for (const name of ['uri','targetUri']) if (value[name]?.startsWith('bolt-stdlib:///')) value[name] = await libraryFile(value[name]);
            if (value.location) await locations(value.location);
        }
        return value;
    }
    function remoteParams(params) {
        const uri = params?.textDocument?.uri;
        const original = uri && originals.get(key(uri));
        return original ? { ...params, textDocument: { ...params.textDocument, uri: original } } : params;
    }
    const foldCache = new WeakMap();
    function folds(doc) {
        let value = foldCache.get(doc);
        if (!value) {
            value = editing.foldPairs(doc.getText()).filter(([a,b]) => doc.positionAt(b).line > doc.positionAt(a).line);
            foldCache.set(doc, value);
        }
        return value;
    }
    async function describe(doc, epoch) {
        const text = doc.getText(), semantic = [], lexical = [], runs = [];
        if (server && types.length && settings.semanticHighlighting !== false) {
            const id = key(doc.uri);
            const token = { get isCancellationRequested() { return docs.get(id) !== doc || epoch !== generation; } };
            const result = await coloring.middleware.provideDocumentSemanticTokens(doc, token, () =>
                relay('textDocument/semanticTokens/full', remoteParams({ textDocument: { uri: doc.uri } }))).catch(() => null);
            if (token.isCancellationRequested) return;
            let line = 0, column = 0;
            for (let i = 0; i < (result?.data.length || 0); i += 5) {
                const data = result.data;
                line += data[i];
                column = (data[i] ? 0 : column) + data[i + 1];
                const start = doc.offsetAt({ line, character: column });
                semantic.push({ start, end: start + data[i + 2], kind: types[data[i + 3]] });
            }
        }
        if (docs.get(key(doc.uri)) !== doc || epoch !== generation) return;
        scanUntil(text, 0, text.length, lexical);
        let previousEnd = 0;
        paintRange(lexical, semantic, 0, text.length, (a,b,kind) => {
            const id = kindIds.get(kind);
            if (id === undefined) return;
            runs.push(a - previousEnd, b - a, id);
            previousEnd = b;
        });
        let entries = [];
        const model = (await project(doc)).model;
        if (model?.kind === 'executable') {
            const file = fileURLToPath(doc.uri);
            const candidates = (model.entrypoints || []).filter(e => path.relative(file, e.file) === '');
            if (candidates.length) {
                const disk = await fs.readFile(file).catch(() => null);
                if (disk && disk.toString('utf8').replace(/^\uFEFF/, '').replace(/\r\n/g,'\n') === text.replace(/\r\n/g,'\n')) {
                    const diskHash = sdk.sourceHash(disk);
                    entries = candidates.filter(e => e.source_hash === diskHash).map(e => e.line - 1);
                }
            }
        }
        if (docs.get(key(doc.uri)) === doc && epoch === generation)
            await client.sendNotification('bolt/editor', { uri: doc.uri, hash: hash(text), palette, runs, folds: folds(doc), entries });
    }
    function schedule(doc, delay = 0) {
        const id = key(doc.uri);
        clearTimeout(timers.get(id));
        timers.set(id, setTimeout(() => {
            timers.delete(id);
            describe(doc, generation).catch(e => log(e.message));
        }, delay));
    }
    function snapshot(uri, version, text) {
        const doc = TextDocument.create(uri, 'bolt', version, text);
        doc.lineAt = line => ({ text: doc.getText({ start: { line, character: 0 }, end: { line: line + 1, character: 0 } }) });
        return doc;
    }
    function log(message) { return client.sendNotification('window/logMessage', { type: 2, message }); }
    function snippetItems() {
        return Object.entries(snippets).map(([name, s]) => ({ label: s.prefix, kind: 15, detail: name,
            insertTextFormat: 2, insertText: s.body.join('\n') }));
    }
    client.onRequest(async (method, params, token) => {
        if (method === 'initialize') {
            supportsSnippets = !!params.capabilities?.textDocument?.completion?.completionItem?.snippetSupport;
            let result = { capabilities: { textDocumentSync: { openClose: true, change: 2, save: true } } };
            try {
                compiler = await sdk.compiler(settings.compilerPath);
                child = spawn(compiler, ['lsp'], { windowsHide: true, stdio: ['pipe','pipe','pipe'],
                    cwd: path.dirname(compiler) });
                await new Promise((ok, fail) => { child.once('spawn', ok); child.once('error', fail); });
                server = connection(child.stdout, child.stdin);
                child.stderr.on('data', data => log(String(data)));
                server.onNotification((m, p) => client.sendNotification(m, p));
                server.onRequest((m, p, t) => client.sendRequest(m, p, t));
                server.listen();
                result = await relay(method, { ...params, initializationOptions: { ...params.initializationOptions,
                    boltProfile: profile === 'Project default' ? '' : profile } }, token);
                coloring.feature.preInitialize(result.capabilities);
                supportsPendingCompletion = !!result.capabilities.experimental?.boltPendingCompletion;
                types = result.capabilities.semanticTokensProvider?.legend.tokenTypes || [];
                // Publish exact editor palette spans through MEF on both host versions.
                delete result.capabilities.semanticTokensProvider;
            } catch (e) { sdkError = e.message; server?.dispose(); server = null; child?.kill(); compiler = null; }
            Object.assign(result.capabilities, { documentFormattingProvider: true, foldingRangeProvider: true });
            result.capabilities.completionProvider ??= {};
            return result;
        }
        const uri = params?.textDocument?.uri, doc = uri && docs.get(key(uri));
        if (method === 'textDocument/formatting') {
            if (!doc || originals.has(key(uri))) return [];
            const text = doc.getText(), formatted = editing.format(text);
            return formatted === text ? [] : [{ range: { start: doc.positionAt(0), end: doc.positionAt(text.length) }, newText: formatted }];
        }
        if (method === 'textDocument/foldingRange') return doc ? folds(doc).map(([a,b]) =>
            ({ startLine: doc.positionAt(a).line, endLine: doc.positionAt(b).line - 1 })).filter(r => r.endLine > r.startLine) : [];
        if (method === 'bolt/editorRefresh') { if (doc) schedule(doc); return null; }
        if (method === 'bolt/generic') {
            if (!doc || params.hash !== hash(doc.getText())) return false;
            return !!(await relay('textDocument/boltGenericContext', remoteParams(params), token))?.generic;
        }
        if (method === 'textDocument/completion') {
            const result = await relay(method, remoteParams(params), token);
            const items = Array.isArray(result) ? result : result?.items || [];
            for (const item of items) {
                if (item.kind !== 9 || item.detail !== 'Bolt standard module ' + item.label ||
                    !Object.hasOwn(legacyImportTypes, item.label)) continue;
                const type = legacyImportTypes[item.label], intrinsic = type === item.label;
                item.kind = 7;
                item.detail = (intrinsic ? 'built-in type ' : 'class ') + type;
                item.data = { ...item.data, bolt: { ...item.data?.bolt, type, category: intrinsic ? 'type' : 'class' } };
            }
            const offset = doc?.offsetAt(params.position);
            const tail = doc?.getText().slice(offset);
            if (supportsSnippets && tail != null && !/^\w*\s*[(<]/u.test(tail)) {
                for (const item of items) {
                    const bolt = item.data?.bolt;
                    if (bolt?.category !== 'method' || item.textEdit || item.insertTextFormat === 2) continue;
                    item.insertText = (item.insertText || item.label) + (bolt.parameters?.length ? '($0)' : '()$0');
                    item.insertTextFormat = 2;
                }
            }
            if (!params.context?.triggerCharacter && !/(?:\.|->)\w*$/u.test(doc?.getText().slice(0, offset)))
                items.push(...snippetItems());
            return Array.isArray(result) ? items : { ...result, isIncomplete: !!result?.isIncomplete, items };
        }
        if (method === 'shutdown') { closing = true; return relay(method, params, token); }
        const result = await relay(method, remoteParams(params), token);
        return /\/(definition|references|implementation|typeDefinition|documentSymbol)$/.test(method) ? locations(result) : result;
    });
    client.onNotification((method, params) => {
        if (method === 'exit') { void close(); return; }
        if (method === 'initialized' && sdkError) log(sdkError);
        const item = params?.textDocument;
        if (item?.uri) {
            const id = key(item.uri);
            let doc = docs.get(id);
            if (method === 'textDocument/didOpen') doc = snapshot(item.uri, item.version, item.text);
            if (method === 'textDocument/didChange' && doc) {
                for (const change of params.contentChanges) {
                    const text = doc.getText();
                    doc = snapshot(item.uri, item.version, change.range
                        ? text.slice(0, doc.offsetAt(change.range.start)) + change.text + text.slice(doc.offsetAt(change.range.end))
                        : change.text);
                }
            }
            if (method === 'textDocument/didClose') {
                clearTimeout(timers.get(id)); timers.delete(id); docs.delete(id); lastEdit.delete(id);
            }
            else if (doc) {
                docs.set(id, doc);
                let delay = 0;
                if (method === 'textDocument/didChange') {
                    const now = performance.now(), previous = lastEdit.get(id);
                    // Coalesce rapid typing for the compiler. A single edit can paint on the next frame.
                    delay = server && previous !== undefined && now - previous < 130 ? 130 : 16;
                    lastEdit.set(id, now);
                }
                schedule(doc, delay);
            }
        }
        if (method === 'workspace/didChangeWatchedFiles' || method === 'textDocument/didSave') {
            generation++; models.clear(); for (const doc of docs.values()) schedule(doc);
        }
        if (server) sync.sendNotification(method, (m, p) => server.sendNotification(m, p), remoteParams(params))?.catch(e => log(e.message));
    });
    let closing = false;
    async function close() {
        closing = true;
        child?.kill();
        if (cacheDir) await fs.rm(await cacheDir, { force: true, recursive: true }).catch(() => {});
        process.exit(0);
    }
    process.stdin.on('end', close);
    client.listen();
    // Bolt is allowed to fail independently of formatting. The VS client can restart the session.
    process.on('exit', () => child?.kill());
    const monitor = setInterval(() => {
        if (child?.exitCode != null && server && !closing) {
            server.dispose(); server = null; types = []; compiler = null;
            log('Bolt language server exited. Use Bolt > Restart Language Server.');
        }
    }, 1000);
    monitor.unref();
}

async function task([operation, file, workspaceRoot]) {
    const resolveTarget = () => settings.taskTarget
        ? { root: workspaceRoot, target: path.resolve(workspaceRoot, settings.taskTarget) }
        : sdk.projectFor(file, workspaceRoot || undefined);
    if (operation === 'target') return resolveTarget();
    const exe = await sdk.compiler(settings.compilerPath);
    if (operation === 'info') return { executable: exe, ...await sdk.compilerVersion(exe), profile };
    if (operation === 'init') { await sdk.execute(exe, ['init', file], { windowsHide: true }); return; }
    const res = await resolveTarget();
    const cwd = settings.cwd ? path.resolve(res.root, settings.cwd) : undefined;
    if (operation === 'debug') {
        const model = res.marker === 'bolt.proj' ? await sdk.projectInfo(exe, res.target, 'Debug') : null;
        if (model && model.kind !== 'executable') throw Error('Select an executable Bolt project or source file.');
        const artifact = model?.artifact || path.join(res.root, 'bin', 'Debug', path.parse(file).name + '.exe');
        const args = ['compile', res.target, '--profile', 'Debug', '--backend', 'llvm', '--debug-info', '--diagnostics', 'msbuild'];
        if (!model) { await fs.mkdir(path.dirname(artifact), { recursive: true }); args.push('-o', artifact); }
        return { exe, args, root: res.root, artifact, cwd: cwd || model?.run?.working_directory || res.root,
            programArgs: settings.args ?? model?.run?.arguments ?? [] };
    }
    return { exe, args: sdk.invocation(operation, res.target, { profile, args: settings.args, cwd }), root: res.root };
}

if (process.argv[2] === 'lsp') serve().catch(e => { console.error(e); process.exitCode = 1; });
else if (process.argv[2] === 'className') process.stdout.write(JSON.stringify(editing.className(process.argv[3])));
else task(process.argv.slice(2)).then(result => { if (result) process.stdout.write(JSON.stringify(result)); })
    .catch(e => { console.error(e.stderr || e.message); process.exitCode = 1; });

