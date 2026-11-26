const fs = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { constants } = require('node:fs');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const execute = promisify(execFile);

async function compiler(configured = '') {
    const win = process.platform === 'win32';
    const name = win ? 'bolt.exe' : 'bolt';
    const chosen = configured.trim() || process.env.BOLT_COMPILER || '';
    const candidates = [];
    if (chosen) {
        const full = chosen.startsWith('~/') ? path.join(os.homedir(), chosen.slice(2)) : chosen;
        if (!path.isAbsolute(full)) throw new Error('Select an absolute path to the Bolt compiler.');
        if (win && /\.(bat|cmd)$/i.test(full))
            candidates.push(path.resolve(path.dirname(full), '../libexec/bolt.exe'));
        else candidates.push(full);
    } else {
        for (const dir of (process.env.PATH || '').split(path.delimiter)) {
            if (path.isAbsolute(dir)) candidates.push(path.join(dir, name));
        }
        if (win && process.env.LOCALAPPDATA) {
            const programs = path.join(process.env.LOCALAPPDATA, 'Programs');
            for (const dir of ['Bolt-SDK/bin', 'Bolt/bin', 'Bolt/libexec'])
                candidates.push(path.join(programs, dir, name));
        } else {
            for (const dir of ['.local/share/bolt/bin', '.local/share/bolt/libexec', '.local/bin'])
                candidates.push(path.join(os.homedir(), dir, name));
            for (const dir of ['/usr/local/bin', '/opt/homebrew/bin']) candidates.push(path.join(dir, name));
        }
    }
    for (const file of candidates) {
        try {
            if (win && path.extname(file).toLowerCase() !== '.exe') continue;
            if (!(await fs.stat(file)).isFile()) continue;
            await fs.access(file, constants.X_OK);
            return await fs.realpath(file);
        } catch {
            /* Try the next SDK installation. */
        }
    }
    throw new Error('Bolt compiler not found. Install a Bolt SDK and use “Bolt: Select Compiler…”.');
}

async function compilerVersion(exe) {
    const options = { timeout: 10000, maxBuffer: 65536, windowsHide: true };
    try {
        const { stdout } = await execute(exe, ['version', '--json'], options);
        const details = JSON.parse(stdout);
        if (typeof details.version === 'string') return details;
    } catch {
        // Legacy compilers only provide their human-readable version.
    }
    const { stdout } = await execute(exe, ['--version'], options);
    return { version: stdout.trim() };
}

async function projectFor(file, workspaceRoot) {
    let dir = path.dirname(file);
    while (true) {
        for (const marker of ['bolt.proj', 'bolt.project']) {
            try {
                if ((await fs.stat(path.join(dir, marker))).isFile())
                    return { target: dir, root: dir, marker };
            } catch {
                /* Not an enclosing project. */
            }
        }
        if (dir === workspaceRoot || path.dirname(dir) === dir) break;
        dir = path.dirname(dir);
    }
    return { target: file, root: path.dirname(file) };
}

function profileArgs(profile) {
    if (!profile || profile === 'Project default') return [];
    if (!['Debug', 'Release'].includes(profile)) throw new Error('Unsupported Bolt profile: ' + profile);
    return ['--profile', profile];
}

function invocation(operation, target, options = {}) {
    const command = { build: 'compile', run: 'run', check: 'check' }[operation];
    if (!command) throw new Error('Unknown Bolt task operation: ' + operation);
    const args = [command, target, '--diagnostics', 'msbuild', ...profileArgs(options.profile)];
    if (operation === 'run') {
        if (options.cwd) args.push('--working-directory', options.cwd);
        if (options.args != null) {
            if (!Array.isArray(options.args) || options.args.some((x) => typeof x !== 'string'))
                throw new Error('Bolt arguments must be an array of strings.');
            args.push('--', ...options.args);
        }
    }
    return args;
}

async function projectInfo(exe, target, profile, signal) {
    const { stdout } = await execute(
        exe,
        ['project', 'info', target, '--format', 'json', ...profileArgs(profile)],
        { timeout: 15000, maxBuffer: 8 * 1024 * 1024, windowsHide: true, signal },
    );
    const model = JSON.parse(stdout);
    if (model.schema_version !== 1) throw new Error('Unsupported bolt project info schema.');
    return model;
}

function sourceHash(bytes) {
    // FNV-1a-64 uses the prime 2^40 + 435. The low-word product fits
    // exactly in a JavaScript Number, so only the final string needs BigInt.
    let high = 0xcbf29ce4;
    let low = 0x84222325;
    for (const byte of bytes) {
        const mixed = (low ^ byte) >>> 0;
        const product = mixed * 435;
        const carry = Math.floor(product / 0x100000000);
        high = (Math.imul(high, 435) + (mixed << 8) + carry) >>> 0;
        low = product >>> 0;
    }
    return ((BigInt(high) << 32n) | BigInt(low)).toString();
}
module.exports = { compiler, compilerVersion, projectFor, invocation, projectInfo, sourceHash, execute };
