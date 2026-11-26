const fs = require('node:fs');
const path = require('node:path');
const esbuild = require('esbuild');
const root = path.resolve(__dirname, '..');
const build = esbuild.buildSync({
    absWorkingDir: root,
    entryPoints: ['src/extension.cjs'],
    outfile: 'dist/extension.cjs',
    bundle: true,
    platform: 'node',
    target: 'node18',
    format: 'cjs',
    external: ['vscode'],
    metafile: true,
    legalComments: 'eof',
    nodePaths: require.resolve.paths('vscode-languageclient'),
});
// Retain the license text of every module included in the distribution.
const included = new Set();
for (const file of Object.keys(build.metafile.inputs)) {
    const pathParts = path.resolve(root, file).split(path.sep),
        i = pathParts.lastIndexOf('node_modules');
    if (i < 0) continue;
    const last = pathParts[i + 1].startsWith('@') ? i + 3 : i + 2;
    included.add(pathParts.slice(0, last).join(path.sep));
}
const notices = [];
for (const dir of [...included].sort()) {
    const p = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
    const file = fs
        .readdirSync(dir)
        .find((f) => /^licen[sc]e(?:[.-].*)?$/i.test(f) && fs.statSync(path.join(dir, f)).isFile());
    if (!file) throw new Error('Missing distribution license for ' + p.name);
    notices.push(p.name + ' ' + p.version + '\n\n' + fs.readFileSync(path.join(dir, file), 'utf8').trim());
}
fs.writeFileSync(path.join(root, 'ThirdPartyNotices.txt'), notices.join('\n\n---\n\n') + '\n');
