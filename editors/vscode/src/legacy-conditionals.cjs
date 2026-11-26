// Servers predating the boltInactive modifier mark both comments and excluded
// lines as 'comment'. Only directives recognized by the server reach this reader.
function legacyConditionals(initialSymbols) {
    const symbols = new Map(Object.entries(initialSymbols));
    const branches = [];
    const active = () => branches.at(-1)?.active ?? true;

    // The compiler has already validated this expression. Consume both operands
    // even when the left value determines the result.
    function evaluate(expression) {
        const words = expression.match(/[A-Za-z_]\w*|[01!()]|&&|\|\|/g) ?? [];
        let pos = 0;
        function atom() {
            const word = words[pos++];
            if (word === '!') return !atom();
            if (word === '(') {
                const value = or();
                pos++;
                return value;
            }
            if (word === 'defined') {
                if (words[pos] !== '(') return symbols.has(words[pos++]);
                const name = words[pos + 1];
                pos += 3;
                return symbols.has(name);
            }
            return word === '1' || word === 'true' || symbols.get(word) === true;
        }
        function and() {
            let value = atom();
            while (words[pos] === '&&') {
                pos++;
                value = atom() && value;
            }
            return value;
        }
        function or() {
            let value = and();
            while (words[pos] === '||') {
                pos++;
                value = and() || value;
            }
            return value;
        }
        return or();
    }
    function directive(text) {
        const clean = text.replace(/\/\/.*|\/\*.*?\*\//g, ' ');
        const match = /^#\s*(\w+)\s*(.*?)\s*$/u.exec(clean);
        if (!match) return;
        const [, op, arg] = match;
        const branch = branches.at(-1);
        switch (op) {
            case 'if':
            case 'ifdef':
            case 'ifndef': {
                const selected = op === 'if' ? evaluate(arg) : symbols.has(arg) === (op === 'ifdef');
                const parent = active();
                branches.push({ parent, active: parent && selected, selected });
                break;
            }
            case 'elif':
            case 'else':
                if (branch) {
                    const select = !branch.selected && (op === 'else' || evaluate(arg));
                    branch.active = branch.parent && select;
                    branch.selected ||= select;
                }
                break;
            case 'endif':
                branches.pop();
                break;
            case 'define': {
                const [name, value] = arg.split(/\s+/);
                if (active()) symbols.set(name, value !== '0' && value !== 'false');
                break;
            }
            case 'undef':
                if (active()) symbols.delete(arg);
                break;
        }
    }
    return { directive, active };
}
module.exports = { legacyConditionals };
