const fs = require('node:fs');
const path = require('node:path');
const out = path.join(__dirname, 'obj/adapter');
const deps = path.join(__dirname, '../vscode/node_modules');
fs.mkdirSync(out, { recursive: true });
require(path.join(deps, 'esbuild')).buildSync({
    entryPoints: [path.join(__dirname, 'adapter.cjs')], outfile: path.join(out, 'adapter.cjs'),
    bundle: true, platform: 'node', target: 'node18', nodePaths: [deps],
});
const grammar = require('../vscode/syntaxes/bolt.tmLanguage.json');
fs.writeFileSync(path.join(out, 'bolt.tmLanguage.json'), JSON.stringify({ ...grammar, fileTypes: ['bolt'] }));
const icon = fs.readFileSync(path.join(__dirname, '../vscode/media/file.svg'), 'utf8');
const [, png] = icon.match(/href="data:image\/png;base64,([^"]+)"/);
fs.writeFileSync(path.join(out, 'bolt-file.png'), Buffer.from(png, 'base64'));
