// Local editing must also work before a compiler is installed and in Restricted Mode.
const reserved = new Set(
    'import namespace class struct interface enum concept require operator public private protected internal get set static const sealed abstract virtual override this base true false null default new delete var extern async event implicit explicit sizeof if elif else while for return break continue try catch finally throw await switch case do void bool char string float double byte short ushort int uint long ulong'.split(
        ' ',
    ),
);

function className(input) {
    const name = input.trim().replace(/\.bolt$/, '');
    return /^[A-Za-z_][A-Za-z_0-9]*$/.test(name) && !reserved.has(name) ? name : null;
}

function quotedEnd(text, pos, quote) {
    while (pos < text.length) {
        const c = text[pos++];
        if (c === '\\') pos = Math.min(pos + 1, text.length);
        else if (c === quote) break;
        else if (c === '\n' || c === '\r') return pos - 1;
    }
    return pos;
}

function interpolationEnd(text, pos, depth = 0) {
    if (depth > 64) return quotedEnd(text, pos, '"');
    while (pos < text.length) {
        const c = text[pos++];
        if (c === '\\') {
            pos = Math.min(pos + 1, text.length);
            continue;
        }
        if (c === '"') break;
        if (c === '\r' || c === '\n') return pos - 1;
        if (c !== '{') continue;
        if (text[pos] === '{') {
            pos++;
            continue;
        }
        let braces = 1;
        while (braces && pos < text.length) {
            const c = text[pos++];
            if (c === '/' && text[pos] === '/') {
                while (pos < text.length && text[pos] !== '\n') pos++;
            } else if (c === '/' && text[pos] === '*') {
                const end = text.indexOf('*/', pos + 1);
                pos = end < 0 ? text.length : end + 2;
            } else if (c === '$' && text[pos] === '"') pos = interpolationEnd(text, pos + 1, depth + 1);
            else if (c === '"' || c === "'") pos = quotedEnd(text, pos, c);
            else if (c === '{') braces++;
            else if (c === '}') braces--;
        }
    }
    return pos;
}

function startsDirective(text, pos) {
    while (pos > 0) {
        const c = text[--pos];
        if (c === '\n' || c === '\r') break;
        if (!/\s/u.test(c)) return false;
    }
    return true;
}

function scanTokens(text, visit) {
    const lexical =
        /(?:\s+|\/\/[^\r\n]*|\/\*[\s\S]*?(?:\*\/|$)|^[ \t]*#[^\r\n]*|\$"|["']|[\p{L}\p{N}_]+|>>=|<<=|\?\?=|\.\.\.|==|!=|<=|>=|&&|\|\||[+\-*/%&|^]=|\+\+|--|->|<<|>>|\?\?|\?\.|.)/gmuy;
    for (let pos = 0; pos < text.length; ) {
        // Consume leading horizontal indentation separately from a directive.
        lexical.lastIndex = pos;
        const m = lexical.exec(text);
        let end = m ? lexical.lastIndex : pos + 1;
        let word = text.slice(pos, end);
        let kind = 'token';
        if (/^\s+$/.test(word)) {
            pos = end;
            continue;
        }
        if (word === '#' && startsDirective(text, pos)) {
            while (end < text.length && text[end] !== '\n' && text[end] !== '\r') end++;
            word = text.slice(pos, end);
            kind = 'directive';
        } else if (word.startsWith('#') && word.length > 1) kind = 'directive';
        else if (word.startsWith('//') || word.startsWith('/*')) kind = 'comment';
        else if (word === '$"' || word === '"' || word === "'") {
            end = word === '$"' ? interpolationEnd(text, end) : quotedEnd(text, end, word);
            word = text.slice(pos, end);
            kind = 'string';
        }
        if (!visit(word, kind, pos, end)) return false;
        pos = end;
    }
    return true;
}
function tokens(text) {
    const result = [];
    scanTokens(text, (word, kind, start, end) => {
        result.push({ word, kind, start, end });
        return true;
    });
    return result;
}

function foldPairs(text) {
    const stack = [], pairs = [];
    // Only these delimiters can change brace nesting. Skip literal and comment bodies whole.
    const delimiters = /\/\/[^\r\n]*|\/\*|\$"|["'{}#]/g;
    for (let match; (match = delimiters.exec(text));) {
        const at = match.index, token = match[0];
        if (token === '{') stack.push(at);
        else if (token === '}') {
            if (stack.length) pairs.push([stack.pop(), at]);
        } else if (token === '/*') {
            const end = text.indexOf('*/', delimiters.lastIndex);
            delimiters.lastIndex = end < 0 ? text.length : end + 2;
        } else if (token === '$"') delimiters.lastIndex = interpolationEnd(text, delimiters.lastIndex);
        else if (token === '"' || token === "'") delimiters.lastIndex = quotedEnd(text, delimiters.lastIndex, token);
        else if (token === '#' && startsDirective(text, at)) {
            let end = at + 1;
            while (end < text.length && text[end] !== '\r' && text[end] !== '\n') end++;
            delimiters.lastIndex = end;
        }
    }
    return pairs;
}

const headers = new Set(['if', 'elif', 'while', 'for', 'catch', 'switch']);
const access = new Set(['public', 'private', 'protected', 'internal']);
function layoutInfo(items) {
    const n = items.length, stack = [];
    const pairs = new Int32Array(n).fill(-1), next = new Int32Array(n + 1);
    const closeToOpen = new Map([[')', '('], [']', '['], ['}', '{']]);
    for (let i = 0; i < n; i++) {
        const word = items[i].word;
        if (word === '(' || word === '[' || word === '{') stack.push(i);
        else if (stack.length && closeToOpen.get(word) === items[stack[stack.length - 1]].word) {
            const j = stack.pop();
            pairs[j] = i;
            pairs[i] = j;
        }
    }
    next[n] = n;
    for (let i = n - 1; i >= 0; i--) next[i] = items[i].kind === 'comment' ? next[i + 1] : i;
    const word = (i) => items[i]?.word;
    function body(i) {
        if (headers.has(word(i))) {
            const paren = next[i + 1];
            return word(paren) === '(' && pairs[paren] >= 0 ? next[pairs[paren] + 1] : -1;
        }
        return ['else', 'do'].includes(word(i)) ? next[i + 1] : -1;
    }
    const ends = new Map();
    function statementEnd(start, depth) {
        if (start >= n || depth > 100) return start;
        if (ends.has(start)) return ends.get(start);
        let end = start;
        const child = body(start);
        if (word(start) === '{') end = pairs[start] >= 0 ? pairs[start] + 1 : start;
        else if (child > start) {
            end = statementEnd(child, depth + 1);
            const follow = next[end];
            if (['if', 'elif'].includes(word(start)) && ['else', 'elif'].includes(word(follow)))
                end = statementEnd(follow, depth + 1);
            if (word(start) === 'do' && word(follow) === 'while' && pairs[next[follow + 1]] >= 0) {
                const semi = next[pairs[next[follow + 1]] + 1];
                if (word(semi) === ';') end = semi + 1;
            }
        } else {
            for (let i = start; i < n; i++) {
                if (word(i) === '}' || items[i].kind === 'directive') {
                    end = i;
                    break;
                }
                if (word(i) === ';') {
                    end = i + 1;
                    break;
                }
                if (pairs[i] > i) i = pairs[i];
                end = i + 1;
            }
        }
        ends.set(start, end);
        return end;
    }
    const startBody = new Set(),
        delta = new Int32Array(n + 1),
        labels = new Set();
    for (let i = 0; i < n; i++) {
        if (access.has(word(i)) && word(i + 1) === ':' && word(i + 2) !== ':') labels.add(i);
        const j = body(i);
        if (
            j < 0 ||
            j >= n ||
            ['{', ';'].includes(word(j)) ||
            (word(i) === 'else' && ['if', 'elif'].includes(word(j)))
        )
            continue;
        const end = statementEnd(j, 0);
        if (end <= j) continue;
        startBody.add(j);
        let first = j;
        while (first > i + 1 && items[first - 1].kind === 'comment') first--;
        delta[first]++;
        delta[end]--;
    }
    return { labels, startBody, delta };
}

function format(text) {
    const items = tokens(text);
    if (!items.length) return '';
    const info = layoutInfo(items),
        newline = text.match(/\r\n|\r|\n/)?.[0] ?? '\n';
    const segments = [],
        branches = [];
    let braces = [],
        delims = [],
        extra = 0,
        pending = false,
        lastEnd = 0, unchangedStart = 0;
    const snapshot = () => ({ braces: braces.slice(), delims: delims.slice() });
    const restore = (s) => {
        braces = s.braces.slice();
        delims = s.delims.slice();
    };
    function branchEnd(branch) {
        const end = snapshot();
        if (!branch.end) branch.end = end;
        else if (JSON.stringify(end) !== JSON.stringify(branch.end)) branch.compatible = false;
    }
    for (let i = 0; i < items.length; i++) {
        const t = items[i], w = t.word,
            p = items[i - 1],
            gap = text.slice(lastEnd, t.start),
            breaks = gap.match(/\r\n|\r|\n/g)?.length ?? 0;
        extra += info.delta[i];
        let indent;
        if (w === '}') {
            const open = braces.pop();
            indent = open?.indent ?? 0;
            if (open) delims = open.delims.slice();
        } else {
            if (w === ')' || w === ']') {
                const match = delims.lastIndexOf(w === ')' ? '(' : '[');
                if (match >= 0) delims = delims.slice(0, match);
            }
            const base = braces.length ? braces.at(-1).indent + 1 : 0;
            indent = info.labels.has(i) ? base - 1 : base + delims.length;
        }
        indent += extra;
        if (t.kind === 'directive') indent = 0;
        const attached =
            t.kind === 'comment' && p && !breaks && p.kind !== 'directive' && !p.word.startsWith('//');
        const punct = p?.word === '}' && [';', ',', ')', ']', '.', '->', '?.'].includes(w);
        const fresh =
            i > 0 &&
            !attached &&
            !punct &&
            (pending ||
                w === '{' ||
                w === '}' ||
                info.labels.has(i) ||
                info.startBody.has(i) ||
                t.kind === 'directive');
        const between =
            breaks || fresh
                ? newline.repeat(Math.max(1, breaks)) + '    '.repeat(Math.max(0, Math.min(indent, 150)))
                : i === 0
                  ? '    '.repeat(Math.max(0, Math.min(indent, 150)))
                  : punct || info.labels.has(i - 1)
                    ? ''
                    : gap;
        if (between !== gap) {
            segments.push(text.slice(unchangedStart, lastEnd), between);
            unchangedStart = t.start;
        }
        lastEnd = t.end;
        if (t.kind !== 'comment' || breaks) pending = false;
        if (w === '{') {
            braces.push({ indent: Math.max(0, indent - extra), delims });
            delims = [];
            pending = true;
        } else if (w === '}' || info.labels.has(i - 1) || (w === ';' && !delims.length) || w.startsWith('//'))
            pending = true;
        else if (t.kind === 'directive') {
            pending = true;
            const directive = w.match(/^#\s*(\w+)/)?.[1];
            if (['if', 'ifdef', 'ifndef'].includes(directive))
                branches.push({ start: snapshot(), compatible: true });
            else if (branches.length && ['else', 'elif', 'endif'].includes(directive)) {
                const branch = branches.at(-1);
                branchEnd(branch);
                if (directive === 'endif') {
                    branches.pop();
                    restore(branch.compatible ? branch.end : branch.start);
                } else restore(branch.start);
            }
        } else if (w === '(' || w === '[') delims = [...delims, w];
    }
    const tail = text.slice(lastEnd);
    const end = newline.repeat(Math.max(1, tail.match(/\r\n|\n|\r/g)?.length ?? 0));
    if (!segments.length && end === tail) return text;
    segments.push(text.slice(unchangedStart, lastEnd), end);
    const result = segments.join('');
    let index = 0;
    const matches = scanTokens(result, (word, kind) => {
        const original = items[index++];
        return original?.word === word && original.kind === kind;
    });
    return matches && index === items.length ? result : text;
}

module.exports = { tokens, format, foldPairs, className, quotedEnd, interpolationEnd };
