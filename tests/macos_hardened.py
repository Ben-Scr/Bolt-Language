#!/usr/bin/env python3
"""Run overlapping compiled images in an ad-hoc hardened ARM64 process."""
import argparse
from pathlib import Path
import platform
import plistlib
import shutil
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,required=True)
a=p.parse_args()
if platform.system()!='Darwin' or platform.machine()!='arm64':
    raise SystemExit('The Apple Silicon MAP_JIT integration test requires a native ARM64 Mac')
with tempfile.TemporaryDirectory(prefix='Bolt signed host ') as directory:
    root=Path(directory)
    executable=root/'editor'
    shutil.copy2(a.binary,executable)
    entitlements=root/'jit.plist'
    entitlements.write_bytes(plistlib.dumps({'com.apple.security.cs.allow-jit':True,
                                             'com.apple.security.cs.jit-write-allowlist':True}))
    subprocess.run(['/usr/bin/codesign','--force','--sign','-','--options','runtime',
                    '--entitlements',entitlements,executable],check=True)
    subprocess.run([executable],check=True)
