// Only pair '<' when the active compiler resolves the preceding name to a generic declaration.
// Marks track generated closers; a normal '>' or a comparison never consumes user text.
function registerGenericPairs(vscode, getClient) {
    const { window, workspace } = vscode,
        markers = new WeakMap(),
        inserting = new WeakSet(),
        removing = new WeakSet();
    let pending;
    async function removeConsumed(editor) {
        const doc = editor.document;
        if (removing.has(doc)) return;
        removing.add(doc);
        try {
            while (!doc.isClosed) {
                const consumed = [...(markers.get(doc) ?? [])].filter(([, used]) => used);
                if (!consumed.length) return;
                const version = doc.version;
                await editor.edit(edit => {
                    for (const [offset] of consumed)
                        edit.delete(new vscode.Range(doc.positionAt(offset), doc.positionAt(offset + 1)));
                }, { undoStopBefore: false, undoStopAfter: false });
                // A competing keystroke can invalidate an edit. Its change event
                // rebases the marks, including any newly consumed closers.
                if (doc.version === version) return;
            }
        } finally {
            removing.delete(doc);
        }
    }
    function atCaret(editor, caret, version) {
        return window.activeTextEditor === editor && !editor.document.isClosed &&
            editor.document.version === version && editor.selections.length === 1 &&
            editor.selection.isEmpty && editor.selection.active.isEqual(caret);
    }
    function dispatch() {
        if (!pending) return;
        const { editor, caret, version, apply } = pending;
        if (!atCaret(editor, caret, version)) return;
        pending = undefined;
        void apply();
    }
    const edits = workspace.onDidChangeTextDocument((event) => {
        const doc = event.document;
        if (doc.languageId !== 'bolt' || !['file', 'untitled'].includes(doc.uri.scheme) ||
            !event.contentChanges.length) return;
        if (pending?.editor.document === doc) pending = undefined;
        if (event.reason !== undefined) {
            markers.delete(doc);
            return;
        }
        const change = event.contentChanges.length === 1 ? event.contentChanges[0] : undefined;
        const editor = window.activeTextEditor;
        const active = editor?.document === doc && editor.selections.length === 1;
        let marks = markers.get(doc);
        if (marks) {
            if (active && !inserting.has(doc) && change && !change.rangeLength && /^>+$/.test(change.text)) {
                // Claim each original closer once, even if the next key arrives
                // before VS Code has applied the preceding cleanup edit.
                let count = change.text.length;
                for (let offset = change.rangeOffset; count && marks.has(offset); offset++) {
                    if (!marks.get(offset)) {
                        marks.set(offset, true);
                        count--;
                    }
                }
            }
            const changes = change ? [change] : [...event.contentChanges].sort((a, b) => b.rangeOffset - a.rangeOffset);
            for (const c of changes) {
                const next = new Map();
                for (const [offset, used] of marks) {
                    if (offset < c.rangeOffset) next.set(offset, used);
                    else if (offset >= c.rangeOffset + c.rangeLength)
                        next.set(offset + c.text.length - c.rangeLength, used);
                }
                marks = next;
            }
            if (marks.size) markers.set(doc, marks);
            else markers.delete(doc);
            if (active && [...marks.values()].includes(true)) setImmediate(() => removeConsumed(editor));
        }
        if (!active || !change || change.rangeLength || change.text !== '<') return;
        const caretOffset = change.rangeOffset + 1,
            version = doc.version;
        const caret = doc.positionAt(caretOffset);
        // VS Code delivers the document edit and the new selection separately.
        // Either may already be visible when this listener runs.
        pending = { editor, caret, version, apply: () => applyAngle(editor, caret, caretOffset, version) };
        setImmediate(dispatch);
    });
    const selections = window.onDidChangeTextEditorSelection(({ textEditor }) => {
        if (pending?.editor !== textEditor) return;
        dispatch();
        pending = undefined;
    });
    const activeEditor = window.onDidChangeActiveTextEditor(() => { pending = undefined; });

    async function applyAngle(editor, caret, caretOffset, version) {
        const doc = editor.document;
        const nextChar = doc.lineAt(caret.line).text[caret.character];
        if (nextChar && /[\p{L}\p{N}_<=]/u.test(nextChar)) return;
        if (
            workspace
                .getConfiguration('editor', { languageId: 'bolt', uri: doc.uri })
                .get('autoClosingBrackets') === 'never'
        )
            return;
        if (!workspace.isTrusted) return;
        const client = getClient();
        if (!client?.isRunning()) return;
        let resolved;
        try {
            resolved = await client.sendRequest('textDocument/boltGenericContext',
                client.code2ProtocolConverter.asTextDocumentPositionParams(doc, caret));
        } catch {
            return;
        }
        if (!resolved?.generic || !atCaret(editor, caret, version)) return;
        inserting.add(doc);
        try {
            // The snippet places the caret before the closer as part of the same edit.
            const changed = await editor.insertSnippet(new vscode.SnippetString('$0>'), caret, {
                undoStopBefore: false,
                undoStopAfter: false,
            });
            if (changed && editor.document === doc && !doc.isClosed && doc.version === version + 1) {
                const marks = markers.get(doc) || new Map();
                marks.set(caretOffset, false);
                markers.set(doc, marks);
            }
        } finally {
            inserting.delete(doc);
        }
    }
    return vscode.Disposable.from(edits, selections, activeEditor);
}
module.exports = { registerGenericPairs };
