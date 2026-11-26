const { spawn } = require('node:child_process');
const { createMessageConnection, StreamMessageReader, StreamMessageWriter } = require('vscode-jsonrpc/node');

// Predict only an unambiguous, previously declared name. The compiler resolves
// the entire expression, including visibility, shadowing and generic arguments.
function predictMember(text, offset) {
    if (/[\w.]/.test(text.charAt(offset))) return;
    const prefix = /\b[A-Za-z_]\w{2,}$/.exec(text.slice(Math.max(0, offset - 128), offset))?.[0];
    if (!prefix) return;
    let name;
    const declarations = /(?:\b[A-Za-z_]\w*|[>\]])[ \t]+([A-Za-z_]\w*)\s*(?=[=;,)])/g;
    for (const match of text.slice(0, offset - prefix.length).matchAll(declarations)) {
        const candidate = match[1];
        if (!candidate.startsWith(prefix)) continue;
        if (name && name !== candidate) return;
        name = candidate;
    }
    if (!name) return;
    const tail = name.slice(prefix.length) + '.';
    return { text: text.slice(0, offset) + tail + text.slice(offset), extra: tail.length };
}

// A separate, short-lived SDK process sees the speculative edit only in memory.
// Publishing its diagnostics to the real editor would expose text the user never typed.
async function queryMembers(exe, cwd, initParams, snapshot, signal) {
    const child = spawn(exe, ['lsp'], { cwd, windowsHide: true,
        stdio: ['pipe', 'pipe', 'ignore'] });
    const rpc = createMessageConnection(new StreamMessageReader(child.stdout), new StreamMessageWriter(child.stdin));
    const stop = () => { rpc.dispose(); child.kill(); };
    child.on('error', stop);
    child.once('exit', () => rpc.dispose());
    signal.addEventListener('abort', stop, { once: true });
    const timeout = setTimeout(stop, 1500);
    try {
        rpc.listen();
        const init = await rpc.sendRequest('initialize', initParams);
        if (init.serverInfo?.name !== 'Bolt Language Server' || init.capabilities.completionProvider?.resolveProvider ||
            (init.capabilities.positionEncoding ?? 'utf-16') !== 'utf-16') return;
        await rpc.sendNotification('initialized', {});
        const { uri, text, position, overlays } = snapshot;
        for (const overlay of overlays) {
            await rpc.sendNotification('textDocument/didOpen', { textDocument: overlay });
        }
        await rpc.sendNotification('textDocument/didOpen', { textDocument: { uri, text, version: 1, languageId: 'bolt' } });
        return await rpc.sendRequest('textDocument/completion', { textDocument: { uri }, position,
            context: { triggerKind: 2, triggerCharacter: '.' } });
    } finally {
        clearTimeout(timeout);
        signal.removeEventListener('abort', stop);
        stop();
    }
}

function memberPreview(vscode, exe, serverInfo, query = queryMembers) {
    const { workspace, window } = vscode;
    let current, initParams, nextStart = 0;
    const listeners = [];
    function clear() {
        if (current) {
            clearTimeout(current.expiry);
            current.controller.abort();
            current = undefined;
        }
    }
    function otherOverlays(doc) {
        return workspace.textDocuments.filter(other => other !== doc && other.languageId === 'bolt' &&
            (other.isDirty || other.uri.scheme === 'untitled'));
    }
    function length(doc) {
        return doc.offsetAt(doc.lineAt(doc.lineCount - 1).range.end);
    }
    function onEdit({ document: doc, contentChanges }) {
        if (!contentChanges.length || doc.languageId !== 'bolt') return;
        if (current && current.doc !== doc) clear();
        const change = contentChanges[0];
        // A whole identifier pasted just before a dot offers no typing time
        // to hide the analysis. Leave the compiler available for that request.
        if (contentChanges.length !== 1 || change.rangeLength || !/^\w$/.test(change.text) ||
            current?.busy || performance.now() < nextStart || doc.uri.scheme !== 'file' ||
            window.activeTextEditor?.document !== doc || !workspace.getWorkspaceFolder(doc.uri) ||
            length(doc) > 65536) return;
        const offset = change.rangeOffset + change.text.length;
        const prediction = predictMember(doc.getText(), offset);
        if (!prediction) return;
        const { line, character } = doc.positionAt(offset);
        const position = { line, character: character + prediction.extra };
        if (current?.text === prediction.text && current.position.line === line &&
            current.position.character === position.character) return;
        // didOpen triggers an analysis per overlay. Bound speculative work when
        // several documents are being edited or an unsaved dependency is large.
        const peers = otherOverlays(doc);
        if (peers.length > 1 || peers.some(d => d.uri.scheme !== 'file' || length(d) > 65536)) return;
        clear();
        const snapshot = { doc, uri: doc.uri.toString(), text: prediction.text,
            overlays: peers.map(d => ({ uri: d.uri.toString(), text: d.getText(),
                version: d.version, languageId: d.languageId })),
            position, controller: new AbortController(), busy: true };
        current = snapshot;
        const start = performance.now();
        nextStart = start + 500;
        snapshot.expiry = setTimeout(clear, 3000);
        query(exe, workspace.workspaceFolders?.[0]?.uri.fsPath, initParams, snapshot, snapshot.controller.signal).then(result => {
            if (Array.isArray(result) || (result && !result.isIncomplete && Array.isArray(result.items)))
                snapshot.result = result;
        }).catch(() => {
            if (!snapshot.controller.signal.aborted) nextStart = performance.now() + 15000;
        }).finally(() => {
            snapshot.busy = false;
            // Back off when a project's analysis consumes a large share of typing time.
            nextStart = Math.max(nextStart, start + 4 * (performance.now() - start));
        });
    }
    function take(params) {
        if (params?.context?.triggerCharacter !== '.' || !current) return;
        const { doc, uri, position, text, result, overlays } = current;
        // Once the real dot arrives, the preview has either finished or lost
        // the race. Release the worker before the primary server handles a miss.
        clear();
        if (!result) return;
        if (params.textDocument.uri !== uri || params.position.line !== position.line ||
            params.position.character !== position.character || doc.getText() !== text) return;
        const peers = otherOverlays(doc);
        if (peers.length !== overlays.length || peers.some((d, i) =>
            d.uri.toString() !== overlays[i].uri || d.version !== overlays[i].version)) return;
        return result;
    }
    const resetOnBolt = doc => { if (doc.languageId === 'bolt') clear(); };
    const feature = {
        fillClientCapabilities() {},
        fillInitializeParams(params) { initParams = params; },
        initialize(capabilities) {
            if (capabilities.experimental?.boltPendingCompletion) return;
            if (serverInfo()?.name !== 'Bolt Language Server' || !capabilities.completionProvider ||
                capabilities.completionProvider.resolveProvider) return;
            listeners.push(workspace.onDidChangeTextDocument(onEdit),
                workspace.onDidCloseTextDocument(resetOnBolt), workspace.onDidSaveTextDocument(resetOnBolt));
        },
        getState() { return { kind: 'static' }; },
        clear() {
            for (const listener of listeners.splice(0)) listener.dispose();
            clear();
        },
    };
    return { take, clear, feature };
}

module.exports = { memberPreview, predictMember };
