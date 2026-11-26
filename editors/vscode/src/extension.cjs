const vscode = require('vscode');
const path = require('node:path');
const fs = require('node:fs/promises');
const { LanguageClient, RevealOutputChannelOn } = require('vscode-languageclient/node');
const sdk = require('./sdk.cjs');
const editing = require('./editing.cjs');
const { intellijColors } = require('./intellij-colors.cjs');
const { editorPalette } = require('./editor-palette.cjs');
const { registerGenericPairs } = require('./generics.cjs');
const { batchDocumentChanges } = require('./document-sync.cjs');
const { memberPreview } = require('./member-preview.cjs');

function activate(context) {
    const { window, workspace, languages, commands } = vscode;
    const owned = context.subscriptions;
    const output = window.createOutputChannel('Bolt Language Server');
    const changes = new vscode.EventEmitter();
    owned.push(output, changes);
    const config = (uri) => workspace.getConfiguration('bolt', uri);
    const profile = () => config().get('profile', 'Project default');
    const selector = [{ language: 'bolt', scheme: 'file' }];
    const languageSelector = [...selector, { language: 'bolt', scheme: 'untitled' }];
    const localSelector = [...languageSelector, { language: 'bolt', scheme: 'bolt-stdlib' }];
    let client,
        sessionWatcher, editSync, preview,
        disposed = false,
        serial = Promise.resolve();
    const projectModels = new Map();
    let completionSettings = new WeakMap();
    const palette = editorPalette(vscode, owned);
    const compiler = () => sdk.compiler(config().get('compilerPath', ''));
    function guard() {
        if (!workspace.isTrusted) throw new Error('Trust this workspace before starting the Bolt compiler.');
    }
    async function stop() {
        preview?.clear();
        preview = undefined;
        editSync?.flush();
        editSync = undefined;
        const old = client;
        client = undefined;
        palette.reset();
        sessionWatcher?.dispose();
        sessionWatcher = undefined;
        if (old) {
            try {
                await old.dispose();
            } catch (e) {
                output.appendLine(String(e));
            }
        }
    }
    function restart() {
        serial = serial
            .then(async () => {
                await stop();
                projectModels.clear();
                changes.fire();
                if (disposed || !workspace.isTrusted) return;
                const exe = await compiler();
                sessionWatcher = workspace.createFileSystemWatcher(
                    '**/{*.bolt,bolt.proj,bolt.conf,bolt.project}',
                );
                // The compiler accepts standard UTF-16 LSP edits. Keep the client and watchers scoped to one session.
                const readAhead = {
                    word({ uri, version }, pos) {
                        const doc = window.activeTextEditor?.document;
                        if (!doc || doc.languageId !== 'bolt' || doc.version !== version || doc.uri.toString() !== uri) return;
                        const line = doc.lineAt(pos.line).text;
                        if (/\w/.test(line.charAt(pos.character))) return;
                        const word = /[A-Za-z_]\w*$/.exec(line.slice(Math.max(0, pos.character - 256), pos.character))?.[0];
                        if (!word) return;
                        let settings = completionSettings.get(doc);
                        if (!settings) {
                            const editor = workspace.getConfiguration('editor', doc);
                            const quick = editor.get('quickSuggestions');
                            settings = { words: quick === true || quick?.other === true || quick?.other === 'on',
                                members: editor.get('suggestOnTriggerCharacters', true) };
                            completionSettings.set(doc, settings);
                        }
                        if (word.length === 1 ? settings.words : settings.members) return word;
                    },
                    async query(params) {
                        const result = await next.sendRequest('textDocument/boltPendingCompletion', params);
                        if (result === null) return null;
                        if (!result || result.isIncomplete || !Array.isArray(result.items)) return;
                        const defaults = next.initializeResult.capabilities.completionProvider?.allCommitCharacters;
                        return next.protocol2CodeConverter.asCompletionResult(result, defaults);
                    },
                };
                editSync = batchDocumentChanges(() => next.initializeResult?.capabilities.experimental?.boltPendingCompletion, readAhead);
                const sync = editSync;
                const members = memberPreview(vscode, exe, () => next.initializeResult?.serverInfo);
                preview = members;
                const colors = intellijColors(palette.setSemantic, conditionalSymbols, editSync.pendingWrite);
                const next = new LanguageClient(
                    'bolt',
                    'Bolt Language Server',
                    { command: exe, args: ['lsp'] },
                    {
                        documentSelector: localSelector,
                        middleware: { ...colors.middleware, sendNotification: editSync.sendNotification,
                            async provideCompletionItem(doc, pos, context, token, provide) {
                                const version = doc.version;
                                const cached = sync.takeCompletion(doc.uri.toString(), version, pos);
                                if (cached) {
                                    const result = await cached;
                                    if (token.isCancellationRequested || version !== doc.version) return null;
                                    if (result) return result;
                                    // The server already asked for the real document at this exact edit.
                                    if (result === null) sync.flush();
                                }
                                return provide(doc, pos, context, token);
                            },
                            async provideDocumentSymbols(doc, token, provide) {
                                if (!next.initializeResult?.capabilities.experimental?.boltPendingCompletion) return provide(doc, token);
                                const version = doc.version;
                                await sync.pendingWrite();
                                if (!token.isCancellationRequested && version === doc.version) return provide(doc, token);
                            },
                            sendRequest(type, params, token, send) {
                                if ((type.method ?? type) === 'textDocument/completion' && !token?.isCancellationRequested) {
                                    const result = members.take(params);
                                    if (result) {
                                        sync.flush();
                                        return Promise.resolve(result);
                                    }
                                }
                                return sync.sendRequest(type, params, token, send);
                            } },
                        initializationOptions: {
                            boltProfile: profile() === 'Project default' ? '' : profile(),
                            boltCompactCompletions: true,
                        },
                        synchronize: { fileEvents: sessionWatcher },
                        outputChannel: output,
                        revealOutputChannelOn: RevealOutputChannelOn.Never,
                    },
                );
                next.registerFeature(colors.feature);
                next.registerFeature(members.feature);
                client = next;
                try {
                    await next.start();
                } catch (e) {
                    if (client === next) client = undefined;
                    await next.dispose().catch(() => {});
                    throw e;
                } finally {
                    if (client !== next) {
                        sessionWatcher?.dispose();
                        sessionWatcher = undefined;
                    }
                }
            })
            .catch((e) => {
                output.appendLine(String(e));
                if (!disposed)
                    window.showWarningMessage(e.message || String(e), 'Select Compiler…').then((choice) => {
                        if (choice) commands.executeCommand('bolt.selectCompiler');
                    });
            });
        return serial;
    }
    owned.push(
        workspace.registerTextDocumentContentProvider('bolt-stdlib', {
            async provideTextDocumentContent(uri, token) {
                await serial;
                if (!client) throw new Error('Bolt Language Server is unavailable.');
                const doc = await client.sendRequest(
                    'bolt/virtualDocument',
                    { uri: 'bolt-stdlib://' + uri.path },
                    token,
                );
                if (!doc) throw new Error('Embedded Bolt source is unavailable.');
                return doc.text;
            },
        }),
    );
    function bind(name, handler) {
        owned.push(
            commands.registerCommand('bolt.' + name, async (...args) => {
                try {
                    return await handler(...args);
                } catch (e) {
                    await window.showErrorMessage(e.message || String(e));
                }
            }),
        );
    }
    owned.push(
        window.registerTerminalLinkProvider({
            provideTerminalLinks({ line }) {
                const match = /^(.+)\((\d+),(\d+)\):\s*(?:error|warning|info)\s+BLT\d+:/i.exec(line);
                if (!match || !path.isAbsolute(match[1])) return [];
                const pos = new vscode.Position(
                    Math.max(0, Number(match[2]) - 1),
                    Math.max(0, Number(match[3]) - 1),
                );
                return [
                    {
                        startIndex: 0,
                        length: match[0].indexOf('):') + 1,
                        tooltip: 'Open Bolt diagnostic',
                        uri: vscode.Uri.file(match[1]),
                        position: pos,
                    },
                ];
            },
            async handleTerminalLink(link) {
                await window.showTextDocument(link.uri, {
                    selection: new vscode.Range(link.position, link.position),
                });
            },
        }),
    );
    bind('restart', () => {
        guard();
        return restart();
    });
    bind('showToolchain', async () => {
        guard();
        const executable = await compiler();
        const details = await sdk.compilerVersion(executable);
        const doc = window.activeTextEditor?.document;
        const scope = doc?.languageId === 'bolt' ? doc : { languageId: 'bolt' };
        const info = {
            extension: context.extension.packageJSON.version,
            compiler: details.version,
            executable,
            target: details.target,
            runtimeAbi: details.runtimeAbi,
            theme: workspace.getConfiguration('workbench').get('colorTheme'),
            semanticHighlighting: workspace.getConfiguration('editor', scope).get('semanticHighlighting.enabled'),
        };
        output.appendLine('Bolt toolchain');
        output.appendLine(JSON.stringify(info, null, 2));
        output.show(true);
        return info;
    });
    bind('selectCompiler', async () => {
        const file = (
            await window.showOpenDialog({
                canSelectMany: false,
                canSelectFiles: true,
                canSelectFolders: false,
                openLabel: 'Select Bolt compiler',
            })
        )?.[0];
        if (!file) return;
        const exe = await sdk.compiler(file.fsPath);
        await config().update('compilerPath', exe, vscode.ConfigurationTarget.Global);
    });
    bind('profile', async () => {
        guard();
        const value = await window.showQuickPick(['Project default', 'Debug', 'Release'], {
            placeHolder: 'Bolt build and analysis profile',
        });
        if (value)
            await config().update(
                'profile',
                value,
                workspace.workspaceFolders?.length
                    ? vscode.ConfigurationTarget.Workspace
                    : vscode.ConfigurationTarget.Global,
            );
    });
    owned.push(workspace.onDidGrantWorkspaceTrust(restart));
    owned.push(workspace.onDidChangeWorkspaceFolders(() => restart()));
    owned.push(
        workspace.onDidChangeConfiguration((e) => {
            if (e.affectsConfiguration('editor.quickSuggestions') || e.affectsConfiguration('editor.suggestOnTriggerCharacters')) {
                completionSettings = new WeakMap();
                editSync?.invalidateCompletion();
            }
            if (e.affectsConfiguration('bolt.compilerPath') || e.affectsConfiguration('bolt.profile'))
                restart();
        }),
    );
    async function dirFor(uri) {
        if (uri?.scheme === 'file') {
            const stat = await fs.stat(uri.fsPath);
            return stat.isDirectory() ? uri.fsPath : path.dirname(uri.fsPath);
        }
        const scope = workspace.getWorkspaceFolder(
            window.activeTextEditor?.document.uri || vscode.Uri.file(process.cwd()),
        );
        if (scope) return scope.uri.fsPath;
        const [dir] =
            (await window.showOpenDialog({
                openLabel: 'Select directory',
                canSelectFolders: true,
                canSelectFiles: false,
                canSelectMany: false,
            })) || [];
        return dir?.fsPath;
    }
    bind('newClass', async (uri) => {
        const dir = await dirFor(uri);
        if (!dir) return;
        const input = await window.showInputBox({
            prompt: 'Bolt class name',
            validateInput: (v) =>
                editing.className(v) ? undefined : 'Enter a Bolt identifier, for example Player.',
        });
        if (!input) return;
        const name = editing.className(input),
            file = vscode.Uri.file(path.join(dir, name + '.bolt'));
        await fs.writeFile(file.fsPath, 'public class ' + name + '\n{\n    \n}\n', { flag: 'wx' });
        const editor = await window.showTextDocument(await workspace.openTextDocument(file));
        editor.selection = new vscode.Selection(2, 4, 2, 4);
    });
    bind('newProject', async (uri) => {
        guard();
        const parent = await dirFor(uri);
        if (!parent) return;
        const name = await window.showInputBox({
            prompt: 'New Bolt project folder',
            validateInput: (v) =>
                /^[A-Za-z0-9_][A-Za-z0-9._ -]*$/.test(v) && !/[. ]$/.test(v)
                    ? undefined
                    : 'Enter a folder name.',
        });
        if (!name) return;
        const dest = path.join(parent, name);
        await sdk.execute(await compiler(), ['init', dest], { cwd: parent, windowsHide: true });
        const entry = vscode.Uri.file(path.join(dest, 'src', 'Main.bolt'));
        await window.showTextDocument(await workspace.openTextDocument(entry));
    });
    async function resource(uri) {
        const file = uri?.fsPath || window.activeTextEditor?.document.uri.fsPath;
        if (file && file.toLowerCase().endsWith('.bolt')) {
            const folder = workspace.getWorkspaceFolder(vscode.Uri.file(file));
            return { ...(await sdk.projectFor(file, folder?.uri.fsPath)), folder };
        }
        const folders = workspace.workspaceFolders || [];
        const folder =
            folders.length === 1
                ? folders[0]
                : await window.showWorkspaceFolderPick({ placeHolder: 'Select Bolt workspace' });
        if (!folder) throw new Error('Open a .bolt file or a Bolt project.');
        return { folder, root: folder.uri.fsPath, target: folder.uri.fsPath };
    }
    async function taskFor(def, scope, res) {
        guard();
        const cfg = config(vscode.Uri.file(res.root)),
            target = def.target ? path.resolve(scope?.uri?.fsPath || res.root, def.target) : res.target;
        const workdir = def.cwd ?? cfg.get('run.workingDirectory', '');
        const args = sdk.invocation(def.operation, target, {
            profile: def.profile ?? profile(),
            args: def.args ?? cfg.get('run.arguments'),
            cwd: workdir ? path.resolve(res.root, workdir) : '',
        });
        const execution = new vscode.ProcessExecution(await compiler(), args, {
            cwd: res.root,
            env: { ...cfg.get('run.environment', {}), ...def.env },
        });
        const task = new vscode.Task(
            def,
            scope || vscode.TaskScope.Global,
            def.operation + ' ' + path.basename(target),
            'Bolt',
            execution,
            ['$bolt'],
        );
        if (def.operation === 'build') task.group = vscode.TaskGroup.Build;
        task.presentationOptions = {
            reveal: vscode.TaskRevealKind.Always,
            panel: vscode.TaskPanelKind.Dedicated,
            clear: true,
        };
        return task;
    }
    for (const operation of ['run', 'build', 'check'])
        bind(operation, async (uri) => {
            guard();
            const res = await resource(uri);
            // Save all changed source and project files participating in this project.
            const modified = workspace.textDocuments.filter(
                (d) =>
                    d.isDirty &&
                    d.uri.scheme === 'file' &&
                    (d.fileName === res.target ||
                        (!path.isAbsolute(path.relative(res.root, d.fileName)) &&
                            !path.relative(res.root, d.fileName).split(path.sep).includes('..'))) &&
                    (d.languageId === 'bolt' ||
                        ['bolt.proj', 'bolt.conf', 'bolt.project'].includes(path.basename(d.fileName))),
            );
            for (const doc of modified)
                if (!(await doc.save()))
                    throw new Error('Save the modified project files before running Bolt.');
            return vscode.tasks.executeTask(
                await taskFor({ type: 'bolt', operation, target: res.target }, res.folder, res),
            );
        });
    owned.push(
        vscode.tasks.registerTaskProvider('bolt', {
            async provideTasks() {
                if (!workspace.isTrusted) return [];
                const result = [];
                for (const folder of workspace.workspaceFolders || []) {
                    const res = { root: folder.uri.fsPath, target: folder.uri.fsPath };
                    try {
                        for (const op of ['build', 'run', 'check'])
                            result.push(await taskFor({ type: 'bolt', operation: op }, folder, res));
                    } catch {
                        /* Configuring an SDK enables the tasks. */
                    }
                }
                return result;
            },
            async resolveTask(task) {
                if (!workspace.isTrusted) return;
                const scope = typeof task.scope === 'object' ? task.scope : workspace.workspaceFolders?.[0];
                const res =
                    task.definition.target && scope
                        ? { root: scope.uri.fsPath, target: scope.uri.fsPath }
                        : await resource();
                return taskFor(task.definition, scope, res);
            },
        }),
    );
    function projectModel(root) {
        let model = projectModels.get(root);
        if (!model) {
            model = compiler().then(exe => sdk.projectInfo(exe, root, profile())).catch(() => null);
            projectModels.set(root, model);
        }
        return model;
    }
    async function conditionalSymbols(doc) {
        if (doc.uri.scheme === 'file') {
            const project = await sdk.projectFor(doc.uri.fsPath);
            if (project.marker === 'bolt.proj') {
                const model = await projectModel(project.root);
                if (model?.conditional_symbols) return model.conditional_symbols;
            }
        }
        // Standalone buffers use the host target and the selected analysis profile.
        const platform = { win32: 'Windows', linux: 'Linux', darwin: 'MacOS', freebsd: 'FreeBSD' }[process.platform];
        const arch = { x64: 'X64', ia32: 'X86', arm64: 'ARM64', arm: 'ARM32', riscv64: 'RISCV64' }[process.arch];
        return Object.fromEntries([platform, arch, profile() === 'Release' ? 'Release' : 'Debug']
            .filter(Boolean).map(name => [name, true]));
    }
    function invalidateModels() {
        preview?.clear();
        editSync?.invalidateCompletion();
        projectModels.clear();
        changes.fire();
    }
    const modelWatch = workspace.createFileSystemWatcher('**/{*.bolt,bolt.proj,bolt.conf,bolt.project}');
    owned.push(
        modelWatch,
        modelWatch.onDidCreate(invalidateModels),
        modelWatch.onDidChange(invalidateModels),
        modelWatch.onDidDelete(invalidateModels),
    );
    owned.push(
        languages.registerCodeLensProvider(selector, {
            onDidChangeCodeLenses: changes.event,
            async provideCodeLenses(doc, token) {
                if (!workspace.isTrusted || doc.isDirty || token.isCancellationRequested) return [];
                const res = await resource(doc.uri);
                if (res.marker !== 'bolt.proj') return [];
                const info = await projectModel(res.root);
                if (info?.kind !== 'executable' || token.isCancellationRequested || doc.isDirty) return [];
                const entries = info.entrypoints.filter((e) => path.relative(e.file, doc.fileName) === '');
                if (!entries.length) return [];
                const disk = await fs.readFile(doc.fileName),
                    hash = sdk.sourceHash(disk);
                if (
                    disk
                        .toString('utf8')
                        .replace(/^\uFEFF/, '')
                        .replace(/\r\n/g, '\n') !== doc.getText().replace(/\r\n/g, '\n')
                )
                    return [];
                return entries
                    .filter((e) => e.source_hash === hash)
                    .map((e) => {
                        const pos = new vscode.Position(e.line - 1, 0);
                        return new vscode.CodeLens(new vscode.Range(pos, pos), {
                            title: '$(play) Run',
                            command: 'bolt.run',
                            arguments: [doc.uri],
                        });
                    });
            },
        }),
    );
    owned.push(
        languages.registerDocumentFormattingEditProvider(localSelector, {
            provideDocumentFormattingEdits(doc) {
                const text = doc.getText(),
                    result = editing.format(text);
                return result === text
                    ? []
                    : [
                          vscode.TextEdit.replace(
                              new vscode.Range(doc.positionAt(0), doc.positionAt(text.length)),
                              result,
                          ),
                      ];
            },
        }),
    );
    const foldsByDocument = new WeakMap();
    owned.push(
        languages.registerFoldingRangeProvider(localSelector, {
            provideFoldingRanges(doc) {
                const previous = foldsByDocument.get(doc);
                if (previous?.version === doc.version) return previous.ranges;
                const ranges = [];
                for (const [open, close] of editing.foldPairs(doc.getText())) {
                    const start = doc.positionAt(open).line, end = doc.positionAt(close).line - 1;
                    if (end > start) ranges.push(new vscode.FoldingRange(start, end));
                }
                foldsByDocument.set(doc, { version: doc.version, ranges });
                return ranges;
            },
        }),
    );
    owned.push(registerGenericPairs(vscode, () => client));
    restart();
    // VS Code calls deactivate before disposing subscriptions.
    shutdown = async () => {
        disposed = true;
        await serial;
        await stop();
    };
    return { ready: () => serial };
}
let shutdown = async () => {};
module.exports = { activate, deactivate: () => shutdown() };
