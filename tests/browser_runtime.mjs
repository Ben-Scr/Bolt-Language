import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
const {default: initialize} = await import(pathToFileURL(resolve(process.argv[2] || 'bolt-browser.mjs')));
const bolt = await initialize();
const evaluate = (source, input='', limit=1000000) => bolt.evaluate(source, input, limit, false);
const source = 'public static class Program { public: static int calls; int Main(string[] arguments) { calls++; Print(Console.ReadLine()); Print(arguments[0]); return calls; } }';
const checked = bolt.evaluate(source, '', 1000000, true);
assert.equal(checked.ok, true, checked.message);
const prepared = bolt.prepare(source, 1000000);
assert.equal(prepared.ok, true, prepared.message);
try {
    for (const [expected, text] of [[1, 'Grüße 🌍'], [2, 'zweiter Aufruf']]) {
        const result = prepared.program.run(text+'\n', ['Fünke']);
        assert.deepEqual(result, {ok:true, exitCode:expected, stdout:text+'\nFünke\n'});
    }
    assert.equal(prepared.program.run('', [4]).ok, false);
} finally {
    prepared.program.delete();
}
const invalid = evaluate('public static class Program { public: int Main(){ return unknown; } }');
assert.equal(invalid.ok, false);
assert.equal(invalid.code, 'BLT2003');
assert.equal(invalid.file, 'Main.bolt');
assert.equal(invalid.line, 1);
assert.equal(invalid.column, 58);
const capped = evaluate('public static class Program { public: int Main(){ int n=0; while(true){ n++; } return n; } }', '', 32);
assert.equal(capped.ok, false);
assert.match(capped.message, /step|budget|limit/i);
const web = evaluate('#if Web && WASM64\npublic static class Program { public: int Main(){ return 17; } }\n#endif');
assert.equal(web.exitCode, 17, web.message);
const compiler = new bolt.Compiler();
const handles = [];
try {
    for (const value of [1, 2]) {
        const result = compiler.prepare('public static class Program { public: static int count; int Main(){ return '+value+' + count++; } }', 1000000);
        assert.equal(result.ok, true, result.message);
        handles.push(result.program);
        assert.equal(compiler.prepare('class', 1000000).ok, false);
        assert.equal(compiler.prepare('public static class Program { public: int Main(){ return missing; } }', 1000000).code, 'BLT2003');
    }
} finally {
    compiler.delete();
}
try {
    for (let i = 0; i < handles.length; ++i) {
        assert.equal(handles[i].run('', []).exitCode, i + 1);
        assert.equal(handles[i].run('', []).exitCode, i + 2);
    }
} finally {
    for (const program of handles) program.delete();
}
for (const name of ['exceptions', 'ranges']) {
    const source = await readFile(new URL('./portable/'+name+'.bolt', import.meta.url), 'utf8');
    const result = evaluate(source);
    assert.equal(result.ok, true, result.message);
    assert.equal(result.exitCode, 0);
    assert.equal(result.stdout, 'portable-'+name+'-ok\n');
}
const hardLimit = evaluate('public static class Program { public: int Main(){ try { while(true){} } catch { return 42; } return 0; } }', '', 32);
assert.equal(hardLimit.ok, false);
assert.match(hardLimit.message, /budget/i);
console.log('PASS browser runtime '+bolt.version+': diagnostics, text, edits, program lifetime, typed exceptions, ranges and execution limits');
