#!/usr/bin/env python3
"""Build and run a debuggable Web export from an installed Bolt SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from urllib.parse import quote, unquote

SOURCE = """public static class Program {
public:
    int Main() {
        int score = 20;
        score += 22;
        Print(score);
        return 0;
    }
}
"""

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--bolt", default=os.environ.get("BOLT_COMPILER", "bolt"))
    parser.add_argument("--emxx", default=os.environ.get("EMXX", "em++"))
    parser.add_argument("--node", default="node")
    args = parser.parse_args()
    sdk = args.sdk.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="bolt-web-debug-") as folder:
        root = Path(folder)
        source = root / "Schritt #1 ü.bolt"
        source.write_text(SOURCE, encoding="utf-8")
        output = root / "web output" / ("Schritt #1 50% " + "ä" * 22 + ".mjs")
        command = [sys.executable, str(sdk / "bolt-web-build.py"), str(source), "-o", str(output),
                   "--bolt", args.bolt, "--emxx", args.emxx, "--debug"]
        subprocess.run(command, check=True)
        wasm = output.with_suffix(".wasm")
        source_map = wasm.with_suffix(".wasm.map")
        data = json.loads(source_map.read_text(encoding="utf-8"))
        matching = [n for n, url in enumerate(data["sources"])
                    if (source_map.parent / unquote(url)).resolve() == source]
        assert len(matching) == 1, matching
        assert data["version"] == 3 and data["mappings"]
        assert data["sourcesContent"][matching[0]] == SOURCE
        script = """
            import assert from 'node:assert/strict';
            import {readFile} from 'node:fs/promises';
            import {pathToFileURL} from 'node:url';
            import {basename} from 'node:path';
            const wasm = await WebAssembly.compile(await readFile(process.argv[2]));
            const section = WebAssembly.Module.customSections(wasm, 'sourceMappingURL');
            assert.equal(section.length, 1);
            const bytes = new Uint8Array(section[0]);
            let length = 0, cursor = 0;
            for (let shift = 0; ; shift += 7) {
                const byte = bytes[cursor++];
                length += (byte & 127) * (2 ** shift);
                if (byte < 128) break;
            }
            assert.equal(length, bytes.length - cursor);
            assert.equal(Buffer.from(bytes.subarray(cursor)).toString(), process.argv[3]);
            const initialize = (await import(pathToFileURL(process.argv[1]))).default;
            const output = [];
            const app = await initialize({print: line => output.push(line)});
            assert.equal(app.callMain([]), 0);
            assert.deepEqual(output, ['42']);
            const requested = [];
            const custom = await initialize({print: () => {}, locateFile(name) {
                requested.push(name);
                return pathToFileURL(process.argv[2]).href;
            }});
            assert.equal(custom.callMain([]), 0);
            assert.deepEqual(requested, [basename(process.argv[2])]);
        """
        subprocess.run([args.node, "--input-type=module", "-e", script, str(output), str(wasm),
                        "./" + quote(source_map.name, safe="")], check=True)
        def hashes():
            return [hashlib.sha256(p.read_bytes()).digest() for p in (output, wasm, source_map)]
        original = hashes()
        source.write_text("class", encoding="utf-8")
        assert subprocess.run(command, capture_output=True).returncode != 0
        assert hashes() == original
        release = command[:-1]
        assert subprocess.run(release, capture_output=True).returncode != 0
        assert hashes() == original
        source.write_text(SOURCE, encoding="utf-8")
        subprocess.run(release, check=True)
        assert not source_map.exists()
        foreign = b'{"version":3,"sources":["other.bolt"],"mappings":""}'
        source_map.write_bytes(foreign)
        subprocess.run(release, check=True)
        assert source_map.read_bytes() == foreign
        threaded = output.parent / "threaded.mjs"
        threading_source = Path(__file__).resolve().parent / "native/runtime-free-threading.bolt"
        subprocess.run([sys.executable, str(sdk / "bolt-web-build.py"), str(threading_source),
                        "--sdk", str(sdk), "--bolt", args.bolt, "--emxx", args.emxx,
                        "--workers", "8", "-o", str(threaded)], check=True)
        runner = threaded.with_name("run-threaded.mjs")
        runner.write_text("import make from './threaded.mjs';\n"
                          "const app = await make();\nprocess.exit(app.callMain([]));\n",
                          encoding="utf-8")
        subprocess.run([args.node, str(runner)], check=True, timeout=45)
    print("PASS Web debug export, source paths, execution, rollback and owned map cleanup")

if __name__ == "__main__":
    main()
