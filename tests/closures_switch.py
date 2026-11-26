"""Exercise closure lifetime, event identity, and switch control flow across backends."""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument("--bolt", type=Path, required=True)
parser.add_argument("--cxx")
args = parser.parse_args()
bolt = args.bolt.resolve()
fixture = Path(__file__).parent / "features/closures-switch.bolt"

with tempfile.TemporaryDirectory(prefix="bolt-closures-switch-") as directory:
    root = Path(directory)
    source = root / "Main.bolt"
    shutil.copyfile(fixture, source)

    def run(*options):
        return subprocess.run([bolt, *options], cwd=root, capture_output=True,
                              text=True, timeout=180)

    available = run("backends").stdout
    modes = [["--runtime"]]
    if "Fast: available" in available:
        modes.append(["--jit", "--backend", "fast", "--require-native"])
    if "LLVM: not available" not in available and "LLVM: " in available:
        modes.append(["--release", "--backend", "llvm", "--require-native"])
    if args.cxx:
        modes.append(["--backend", "cpp", "--cxx", args.cxx])
    for mode in modes:
        result = run("run", source, *mode, "--no-timings")
        assert result.returncode == 0 and "closures and switch OK" in result.stdout, (
            mode, result.returncode, result.stdout, result.stderr)
        print("PASS", *mode, flush=True)

    invalid = [
        ("switch (1) { case 1: break; case 1u: break; }", "duplicate case"),
        ("switch (1) { default: break; default: break; }", "duplicate default"),
        ('switch (1) { case "1": break; }', "does not match"),
        ("switch (1) { default: continue; }", "continue requires a loop"),
        ("Action<int> f = (a,b) => Print(a);", "signature"),
        ("int x=3; int& alias=x; Action f = () => Print(alias);", "cannot capture"),
        ("Func<int,int> f=x=>{ if (x>0) return x; };", "return"),
    ]
    for code, diagnostic in invalid:
        source.write_text("import System\nstatic class Program { public: void Main() {" +
                          code + "} }", encoding="utf-8")
        result = run("check", source)
        assert result.returncode != 0 and diagnostic in result.stderr, (code, result.stderr)
