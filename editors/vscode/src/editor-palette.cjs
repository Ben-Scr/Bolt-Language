const grammar = require('../syntaxes/bolt.tmLanguage.json');
const dark = require('../themes/dark.json');
const light = require('../themes/light.json');
const { quotedEnd, interpolationEnd } = require('./editing.cjs');

// Use the TextMate rules for single tokens and the editor's Bolt scanner for
// strings and block comments, which can extend across visible lines.
const quote = 'string.quoted.double.bolt', character = 'string.quoted.single.bolt';
const interpolated = 'string.quoted.double.interpolated.bolt', block = 'comment.block.bolt';
const rules = grammar.patterns.flatMap(rule => rule.include === '#strings' ? grammar.repository.strings.patterns : [rule]);
const lexer = new RegExp(rules.map(rule => '(' + (rule.match ?? rule.begin) + ')').join('|'), 'gmu');
const wordColors = ['keyword.control.bolt', 'keyword.other.bolt', 'support.type.primitive.bolt'];
const coloredWord = new RegExp('^(?:' + rules.filter(rule => wordColors.includes(rule.name)).map(rule => rule.match).join('|') + ')$', 'u');

function scanUntil(text, cursor, limit, spans) {
    lexer.lastIndex = cursor;
    for (let match; (match = lexer.exec(text));) {
        const start = match.index;
        if (start >= limit) return limit;
        const kind = rules[match.findIndex((s, i) => i > 0 && s !== undefined) - 1].name;
        let end = lexer.lastIndex;
        if (kind === block) {
            const close = text.indexOf('*/', end);
            end = close < 0 ? text.length : close + 2;
        } else if (kind === interpolated) end = interpolationEnd(text, end);
        else if (kind === quote || kind === character) end = quotedEnd(text, end, text[start]);
        spans.push({ start, end, kind });
        if (end >= limit) return end;
        lexer.lastIndex = end;
    }
    return text.length;
}

function colors(theme) {
    const byKind = new Map(Object.entries(theme.semanticTokenColors).map(([key, value]) => [key.split(':')[0], value]));
    for (const { scope, settings } of theme.tokenColors) {
        for (const name of scope) byKind.set(name, settings.foreground);
    }
    return byKind;
}

function firstAfter(spans, offset) {
    let lo = 0, hi = spans.length;
    while (lo < hi) {
        const mid = Math.floor((lo + hi) / 2);
        if (spans[mid].end <= offset) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Semantic spans override only their actual characters, including identifiers
// within strings and inactive regions that span several lexical tokens.
function paintRange(lexical, semantic, start, end, paint) {
    let li = firstAfter(lexical, start), si = firstAfter(semantic, start);
    while (start < end) {
        while (li < lexical.length && lexical[li].end <= start) li++;
        while (si < semantic.length && semantic[si].end <= start) si++;
        const l = lexical[li], s = semantic[si];
        if (s && s.start <= start) {
            const stop = Math.min(s.end, end);
            paint(start, stop, s.kind, s);
            start = stop;
        } else if (l && l.start <= start) {
            const stop = Math.min(l.end, s?.start ?? end, end);
            paint(start, stop, l.kind, l);
            start = stop;
        } else start = Math.min(l?.start ?? end, s?.start ?? end, end);
    }
}

// VS Code rebases existing decorations on edits. Equal coordinates are enough
// to skip a write only for ranges that the edit could not have moved.
function unchangedRanges(previous, ranges, changed) {
    return previous?.length === ranges.length && ranges.every((range, i) =>
        range.isEqual(previous[i]) && (!changed ||
            range.end.line < changed.first || range.start.line > changed.last));
}

function rewindLexical(entry, lineStart) {
    if (lineStart > entry.cursor) return;
    // An unfinished block comment may end exactly at EOF after a newline.
    // Typing on the empty last line extends that token.
    const index = firstAfter(entry.lexical, lineStart - 1);
    entry.cursor = Math.min(lineStart, entry.lexical[index]?.start ?? lineStart);
    entry.lexical.length = index;
}

// Typing an ordinary name or its trailing dot can open the suggestion widget.
// Let it render first when the edit only adds the palette's plain text color.
function plainCompletionEdit(doc, changes, entry) {
    if (changes.length !== 1 || !entry) return false;
    const { range, rangeOffset, text } = changes[0];
    if (!/^[A-Za-z_.]$/.test(text) || range.start.line !== range.end.line ||
        range.start.character !== range.end.character || !Number.isInteger(rangeOffset) || rangeOffset > entry.cursor) return false;
    const column = range.start.character;
    const line = doc.lineAt(range.start.line).text;
    if (line[column + 1] && !/[\s;,)}\]]/.test(line[column + 1])) return false;
    const prior = entry.lexical[firstAfter(entry.lexical, rangeOffset - 1)];
    if (prior && prior.start < rangeOffset && rangeOffset <= prior.end) {
        if (prior.end !== rangeOffset || prior.kind !== 'variable.other.bolt') return false;
        return text === '.' || !coloredWord.test(line.slice(column - (prior.end - prior.start), column + 1));
    }
    return column === 0 || /\s/.test(line[column - 1]);
}

function semanticViewport(data, legend, byKind, lineOffset) {
    let lines;
    return view => {
        // VS Code requests single-line semantic tokens. Index only the
        // first record of each line, then decode visible lines on demand.
        if (!lines) {
            lines = [];
            let line = 0;
            for (let i = 0; i < data.length; i += 5) {
                line += data[i];
                if (i === 0 || data[i]) lines[line] = i;
            }
        }
        const spans = [];
        for (let line = view.start.line; line <= view.end.line && line < lines.length; line++) {
            let row = lines[line];
            if (row === undefined) continue;
            if (typeof row === 'number') {
                const first = row;
                lines[line] = row = [];
                let start = lineOffset(line);
                for (let i = first; i < data.length && (i === first || !data[i]); i += 5) {
                    start += data[i + 1];
                    const kind = legend[data[i + 3]];
                    if (byKind.has(kind)) row.push({ start, end: start + data[i + 2], kind });
                }
            }
            for (const span of row) spans.push(span);
        }
        return spans;
    };
}

function editorPalette(vscode, owned) {
    const { window, workspace, ColorThemeKind } = vscode;
    const darkColors = colors(dark), lightColors = colors(light), styles = new Map(), byKind = new Map();
    for (const [kind, color] of darkColors) {
        const pair = color + lightColors.get(kind);
        if (!styles.has(pair)) {
            const style = window.createTextEditorDecorationType({
                dark: { color }, light: { color: lightColors.get(kind) }, fontStyle: 'normal', fontWeight: 'normal',
            });
            owned.push(style);
            styles.set(pair, style);
        }
        byKind.set(kind, styles.get(pair));
    }
    let docs = new WeakMap(), timer, nextPaint = Infinity, lastPaint = -Infinity;
    const applied = new WeakMap(), edits = new WeakMap();
    function state(doc) {
        let entry = docs.get(doc);
        if (entry?.version !== doc.version) {
            entry = { version: doc.version, lexical: [], cursor: 0 };
            docs.set(doc, entry);
        }
        return entry;
    }
    function render() {
        for (const editor of window.visibleTextEditors) {
            const doc = editor.document, groups = new Map();
            const theme = window.activeColorTheme.kind;
            const native = workspace.getConfiguration('workbench').get('colorTheme');
            if (doc.languageId === 'bolt' && [ColorThemeKind.Dark, ColorThemeKind.Light].includes(theme)
                && native !== dark.name && native !== light.name) {
                const entry = state(doc);
                const semantic = workspace.getConfiguration('editor', doc).get('semanticHighlighting.enabled') === false
                    ? undefined : entry.semantic;
                for (const view of editor.visibleRanges) {
                    const end = doc.offsetAt(view.end);
                    if (end > entry.cursor) {
                        entry.text ??= doc.getText();
                        entry.cursor = scanUntil(entry.text, entry.cursor, end, entry.lexical);
                    }
                    paintRange(entry.lexical, semantic?.(view) ?? [], doc.offsetAt(view.start), end, (a, b, kind, span) => {
                        const style = byKind.get(kind);
                        if (!style) return;
                        if (!groups.has(style)) groups.set(style, []);
                        // Adjacent scroll positions share most tokens. A clipped
                        // decoration belongs to this viewport only.
                        const whole = a === span.start && b === span.end;
                        const range = (whole && span.range) || new vscode.Range(doc.positionAt(a), doc.positionAt(b));
                        if (whole) span.range = range;
                        groups.get(style).push(range);
                    });
                }
            }
            const prior = applied.get(editor), edit = edits.get(doc);
            const sameVersion = prior?.version === doc.version;
            const singleEdit = prior?.version === doc.version - 1 && edit?.version === doc.version;
            for (const style of prior?.groups.keys() ?? []) if (!groups.has(style)) editor.setDecorations(style, []);
            for (const [style, ranges] of groups) {
                if ((sameVersion || singleEdit) && unchangedRanges(prior.groups.get(style), ranges, sameVersion ? undefined : edit)) continue;
                editor.setDecorations(style, ranges);
            }
            applied.set(editor, { version: doc.version, groups });
        }
    }
    function scheduleAfter(delay) {
        const now = performance.now();
        const due = Math.max(now + delay, lastPaint + 16);
        if (timer) {
            if (due >= nextPaint) return;
            clearTimeout(timer);
        }
        nextPaint = due;
        timer = setTimeout(() => {
            timer = undefined;
            render();
            lastPaint = performance.now();
        }, due - now);
    }
    const schedule = () => scheduleAfter(35);
    owned.push(window.onDidChangeVisibleTextEditors(schedule), window.onDidChangeActiveColorTheme(schedule),
        window.onDidChangeTextEditorVisibleRanges(event => { if (event.textEditor.document.languageId === 'bolt') schedule(); }),
        workspace.onDidChangeTextDocument(({ document, contentChanges }) => {
            if (document.languageId !== 'bolt' || !contentChanges.length) return;
            let first = Infinity, last = -1;
            for (const { range, text } of contentChanges) {
                first = Math.min(first, range.start.line);
                // Inserting/removing lines can move every following decoration.
                last = range.start.line !== range.end.line || /[\r\n]/.test(text) ? Infinity : Math.max(last, range.end.line);
            }
            edits.set(document, { version: document.version, first, last });
            const entry = docs.get(document);
            let delay = 8;
            if (entry && entry.version + 1 === document.version) {
                if (plainCompletionEdit(document, contentChanges, entry)) delay = 40;
                // Restart at the affected line, or at the start of a multiline token crossing it.
                // The prefix before the first edited line has the same offsets
                // in the new revision, including after several unpainted edits.
                const lineStart = document.offsetAt(new vscode.Position(first, 0));
                rewindLexical(entry, lineStart);
                entry.semantic = undefined;
                entry.text = undefined;
                entry.version = document.version;
            }
            scheduleAfter(delay);
        }),
        workspace.onDidChangeConfiguration(event => {
            if (event.affectsConfiguration('editor.semanticHighlighting.enabled') || event.affectsConfiguration('workbench.colorTheme')) schedule();
        }), { dispose() { clearTimeout(timer); } });
    schedule();
    return {
        reset() { docs = new WeakMap(); schedule(); },
        setSemantic(doc, data, legend) {
            state(doc).semantic = data?.length ? semanticViewport(data, legend, byKind,
                line => doc.offsetAt(new vscode.Position(line, 0))) : undefined;
            scheduleAfter(8);
        },
    };
}
module.exports = { editorPalette, scanUntil, paintRange, unchangedRanges, rewindLexical, semanticViewport };
