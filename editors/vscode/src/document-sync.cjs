// Bolt analyzes the project once per didChange. One LSP notification can carry
// several incremental edits, each relative to the text after the preceding edit.
const editIdleMs = 120;
const maxBatchAgeMs = 600;

function batchDocumentChanges(supportsPendingCompletion = () => false, readAhead) {
    let pending, preview, receivingNames, reading = false;
    const versions = new Map();

    function invalidateCompletion() {
        preview = undefined;
        receivingNames = undefined;
    }

    function flush() {
        invalidateCompletion();
        if (!pending) return;
        const batch = pending;
        pending = undefined;
        clearTimeout(batch.idle);
        clearTimeout(batch.deadline);
        try {
            // The JSON-RPC writer enqueues synchronously, preserving order with
            // the request, save, or other document change immediately following.
            batch.resolve(batch.send(batch.type, batch.params));
            versions.set(batch.params.textDocument.uri, batch.params.textDocument.version);
        } catch (error) {
            batch.reject(error);
        }
    }

    function sendNotification(type, send, params) {
        const method = type.method ?? type;
        if (method !== 'textDocument/didChange') {
            flush();
            if (method === 'textDocument/didOpen') versions.set(params.textDocument.uri, params.textDocument.version);
            if (method === 'textDocument/didClose') versions.delete(params.textDocument.uri);
            return send(type, params);
        }
        const { textDocument, contentChanges } = params;
        const length = contentChanges.reduce((sum, change) => sum + change.text.length, 0);
        // While the user is editing an expression, completions can already see
        // those edits. Avoid starting a project check between successive words.
        const typing = supportsPendingCompletion() && contentChanges.every(change =>
            change.range && change.range.start.line === change.range.end.line &&
            change.text.length <= 256 && /^[A-Za-z0-9_.]*$/.test(change.text));
        const idleMs = typing ? 500 : editIdleMs;
        if (pending && (pending.params.textDocument.uri !== textDocument.uri ||
            pending.params.textDocument.version >= textDocument.version ||
            pending.length + length > 65536 || pending.params.contentChanges.length + contentChanges.length > 64)) {
            flush();
        }
        if (length > 65536 || contentChanges.length > 64) {
            versions.set(textDocument.uri, textDocument.version);
            return send(type, params);
        }
        if (pending) {
            pending.params.contentChanges.push(...contentChanges);
            pending.params.textDocument = textDocument;
            pending.length += length;
            clearTimeout(pending.idle);
            pending.idle = setTimeout(flush, idleMs);
        } else {
            let resolve, reject;
            const sent = new Promise((ok, fail) => { resolve = ok; reject = fail; });
            // A pause delivers diagnostics promptly. Continuous typing still sends
            // a snapshot periodically, and interactive requests flush immediately.
            pending = { type, send, params: { textDocument, contentChanges: [...contentChanges] },
                length, sent, resolve, reject, idle: setTimeout(flush, idleMs),
                deadline: setTimeout(flush, typing ? 2000 : maxBatchAgeMs) };
        }
        if (readAhead && typing) prepareCompletion(textDocument, contentChanges);
        return pending.sent;
    }

    function expressionEdit(params) {
        const uri = params.textDocument.uri;
        if (!pending || pending.params.textDocument.uri !== uri || !versions.has(uri) ||
            !pending.length || pending.length > 256) return;
        const edits = pending.params.contentChanges;
        const start = edits[0]?.range?.start;
        if (!start || start.line !== params.position.line) return;
        let first = start.character, last = first, text = '';
        for (const edit of edits) {
            const range = edit.range;
            if (!range || range.start.line !== start.line || range.end.line !== start.line ||
                !/^[A-Za-z0-9_.]*$/.test(edit.text)) return;
            const begin = range.start.character, end = range.end.character;
            const currentEnd = first + text.length;
            if (begin > currentEnd || end < first || end < begin) return;
            // Coordinates on the wire are relative to each preceding edit.
            // Keep one replacement range in the original document coordinates.
            text = text.slice(0, Math.max(0, begin - first)) + edit.text + text.slice(Math.max(0, end - first));
            last += Math.max(0, end - currentEnd);
            first = Math.min(first, begin);
        }
        if (text && last - first <= 256 && first + text.length === params.position.character)
            return { version: versions.get(uri), start: { line: start.line, character: first },
                end: { line: start.line, character: last }, text };
    }

    function sameEdit(left, right) {
        return left.version === right.version && left.start.line === right.start.line &&
            left.start.character === right.start.character && left.end.character === right.end.character &&
            left.text === right.text;
    }

    function prepareCompletion(doc, changes) {
        const change = changes[0];
        if (reading || changes.length !== 1 || change.range.start.character !== change.range.end.character ||
            !/^[A-Za-z_]$/.test(change.text)) return;
        const position = { line: change.range.end.line, character: change.range.end.character + 1 };
        const word = readAhead.word(doc, position);
        if (!word) return;
        const batch = pending;
        const single = word.length === 1;
        // The receiver must be a name the actual compiler offered in this scope.
        // The predicted response is still computed from the full replacement.
        if (!single && !(receivingNames?.batch === batch && receivingNames.names.has(word))) return;
        const params = { textDocument: { uri: doc.uri }, position };
        const edit = expressionEdit(params);
        if (!edit || (!single && edit.text.length >= 256)) return;
        const expected = single ? edit : { ...edit, text: edit.text + '.' };
        if (!single) params.position = { ...position, character: position.character + 1 };
        const item = { batch, edit: expected, version: doc.version + (single ? 0 : 1) };
        preview = item;
        reading = true;
        item.promise = Promise.resolve().then(() => readAhead.query({ ...params, edit: expected })).then(result => {
            if (preview !== item) return;
            if (!result) return result;
            if (single) receivingNames = { batch, names: new Set(result.items.map(candidate => candidate.label)) };
            return result;
        }).catch(() => undefined).finally(() => { reading = false; });
    }

    function takeCompletion(uri, version, position) {
        const item = preview;
        if (!item || pending !== item.batch || pending.params.textDocument.uri !== uri ||
            pending.params.textDocument.version !== version || item.version !== version) return;
        const edit = expressionEdit({ textDocument: { uri }, position });
        if (edit && sameEdit(edit, item.edit)) {
            return item.promise.then(result => preview === item && pending === item.batch &&
                pending.params.textDocument.version === version ? result : undefined);
        }
    }

    function sendRequest(type, params, token, send) {
        // A speculative expression is evaluated against the last delivered version.
        if ((type.method ?? type) === 'textDocument/boltPendingCompletion') return send(type, params, token);
        if ((type.method ?? type) === 'textDocument/completion' && supportsPendingCompletion()) {
            const edit = expressionEdit(params);
            if (edit) {
                return send('textDocument/boltPendingCompletion', { ...params, edit }, token).then(result => {
                    if (result !== null || token?.isCancellationRequested) return result;
                    flush();
                    return send(type, params, token);
                });
            }
        }
        flush();
        return send(type, params, token);
    }
    return { flush, sendNotification, sendRequest, takeCompletion, invalidateCompletion, pendingWrite: () => pending?.sent };
}
module.exports = { batchDocumentChanges };
