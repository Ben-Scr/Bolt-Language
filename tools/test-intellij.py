#!/usr/bin/env python3
"""Run SDK-independent Java tests; --bolt also tests the real native language server."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--bolt',type=Path)
p.add_argument('--format-corpus',type=Path,action='append',default=[],help='Check formatter preservation/idempotence for every .bolt file under this path (repeatable)')
a=p.parse_args()
if not shutil.which('javac') or not shutil.which('java'):p.error('Java 21+ JDK must be in PATH')
sources=list((ROOT/'editors/intellij/src/main/java/dev/bolt/idea/core').glob('*.java'))+list((ROOT/'editors/intellij/src/test/java').rglob('*.java'))
with tempfile.TemporaryDirectory(prefix='bolt-java-tests-') as d:
    subprocess.run(['javac','--release','21','-encoding','UTF-8','-d',d]+list(map(str,sources)),check=True)
    classpath=os.pathsep.join([d,str(ROOT/'editors/intellij/src/main/resources')])
    command=['java','-ea','-cp',classpath,'dev.bolt.idea.core.CoreTests']
    if a.bolt:command.append(str(a.bolt.resolve()))
    subprocess.run(command,check=True,timeout=120)
    if a.format_corpus:
        subprocess.run(['java','-ea','-cp',d,'dev.bolt.idea.core.FormattingTests']+
                       [str(path.resolve()) for path in a.format_corpus],check=True,timeout=120)
