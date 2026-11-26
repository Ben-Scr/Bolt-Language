// IntelliJ's BoltColors.semantic(kind, word) refines three LSP categories using the source text.
// Keep original server indices intact and append the refined types to VS Code's local legend.
const { legacyConditionals } = require('./legacy-conditionals.cjs');
const primitives = new Set('void bool char string float double byte short ushort int uint long ulong'.split(' '));
const controls = new Set('if elif else while for return break continue try catch finally throw await switch case do'.split(' '));
const refinedTypes = ['boltControlFlow', 'boltPrimitive', 'boltInactiveCode'];

function intellijColors(onSemantic, conditionalSymbols, pendingWrite) {
    let types = [], tokenTypes = [], refined = new Map(), inactiveMask = 0;
    const feature = {
        fillClientCapabilities() {},
        preInitialize(capabilities) {
            const original = capabilities.semanticTokensProvider;
            if (!original?.legend) return;
            types = original.legend.tokenTypes;
            const inactiveIndex = original.legend.tokenModifiers.indexOf('boltInactive');
            inactiveMask = inactiveIndex < 0 ? 0 : 1 << inactiveIndex;
            tokenTypes = [...new Set([...types, ...refinedTypes])];
            refined = new Map(refinedTypes.map((type) => [type, tokenTypes.indexOf(type)]));
            // Delta payloads may refer to preceding tokens. Use the full provider for source-aware coloring.
            capabilities.semanticTokensProvider = {
                ...original,
                full: original.full ? true : original.full,
                legend: { ...original.legend, tokenTypes },
            };
        },
        initialize() {},
        getState() { return { kind: 'static' }; },
        clear() { types = []; refined.clear(); inactiveMask = 0; },
    };
    async function classify(result, doc, version, token) {
        if (!result || !refined.size) return result;
        let line = 0, column = 0, cachedLine = -1, text = '';
        const data = result.data;
        let branches;
        for (let i = 0; i < data.length; i += 5) {
            line += data[i];
            column = data[i] ? data[i + 1] : column + data[i + 1];
            const kind = types[data[i + 3]];
            if (kind === 'comment') {
                const inactive = inactiveMask ? data[i + 4] & inactiveMask : branches && !branches.active();
                if (inactive) data[i + 3] = refined.get('boltInactiveCode');
                continue;
            }
            if (kind !== 'keyword' && kind !== 'type' && (kind !== 'macro' || inactiveMask)) continue;
            if (line !== cachedLine) {
                text = doc.lineAt(line).text;
                cachedLine = line;
            }
            const word = text.slice(column, column + data[i + 2]);
            if (kind === 'macro') {
                if (!branches) {
                    const symbols = await conditionalSymbols(doc);
                    if (token.isCancellationRequested || version !== doc.version) return undefined;
                    branches = legacyConditionals(symbols);
                }
                branches.directive(word);
            } else if (primitives.has(word)) data[i + 3] = refined.get('boltPrimitive');
            else if (kind === 'keyword' && controls.has(word)) data[i + 3] = refined.get('boltControlFlow');
        }
        return result;
    }
    async function provide(doc, token, next) {
        const version = doc.version;
        // Background highlighting can share the next edit batch. Recheck the
        // document before asking the compiler to color an obsolete version.
        const pending = pendingWrite?.();
        if (pending) {
            await pending;
            if (token.isCancellationRequested || version !== doc.version) return undefined;
        }
        const result = await next(doc, token);
        if (token.isCancellationRequested || version !== doc.version) return undefined;
        const classified = await classify(result, doc, version, token);
        if (token.isCancellationRequested || version !== doc.version) return undefined;
        onSemantic?.(doc, classified?.data, tokenTypes);
        return classified;
    }
    return {
        feature,
        middleware: { provideDocumentSemanticTokens: provide },
    };
}

module.exports = { intellijColors };
