#!/usr/bin/env python3
"""Run the installed VSIX in an isolated local VS Code Desktop Extension Host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--code", required=True, type=Path, help="VS Code CLI (code, code.cmd or Code.exe)")
    parser.add_argument("--bolt", required=True, type=Path, help="Absolute path to the Bolt compiler")
    parser.add_argument("--vsix", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="Bolt VS Code ") as scratch:
        directory = Path(scratch)
        project = directory / "Fixture space"
        vscode = str(args.code.resolve())
        bolt = str(args.bolt.resolve())
        result = directory / "extension-host-result.txt"
        env = {**os.environ, "BOLT_COMPILER": bolt, "BOLT_VSCODE_TEST_RESULT": str(result)}
        profile = directory / "profile"
        (profile / "User").mkdir(parents=True)
        (profile / "User" / "settings.json").write_text(
            '{"telemetry.telemetryLevel":"off","extensions.autoUpdate":false,'
            '"extensions.autoCheckUpdates":false,"workbench.enableExperiments":false}', encoding="utf-8"
        )
        common = [vscode, "--user-data-dir", str(profile), "--extensions-dir", str(directory / "extensions")]
        subprocess.run([bolt, "init", str(project)], env=env, check=True)
        subprocess.run(common + ["--install-extension", str(args.vsix.resolve()), "--force"], env=env, check=True)
        subprocess.run(common + ["--disable-workspace-trust", "--skip-welcome", "--skip-release-notes",
            "--disable-updates", "--disable-telemetry", "--wait",
            "--extensionDevelopmentPath=" + str(ROOT / "test/driver"),
            "--extensionTestsPath=" + str(ROOT / "test/host.cjs"), str(project)], env=env, check=True, timeout=240)
        if not result.is_file() or result.read_text(encoding="utf-8") != "all checks passed\n":
            raise SystemExit("VS Code exited before the installed extension completed its integration checks.")
        print("PASS installed extension providers and Check, Build, Run tasks")

if __name__ == "__main__":
    main()
