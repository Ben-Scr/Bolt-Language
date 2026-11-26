#!/usr/bin/env python3
"""Build the Bolt IntelliJ plugin OFFLINE against an existing IntelliJ installation.
No Gradle download, commercial LSP API, or third-party plugin is required.
"""
from __future__ import annotations
import argparse
import datetime
import glob
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = ROOT / 'editors' / 'intellij'
VERSION = ET.parse(PLUGIN / 'src/main/resources/META-INF/plugin.xml').getroot().findtext('version')

def ide_home(path):
    path = Path(path).expanduser().resolve()
    if (path / 'Contents' / 'lib').is_dir(): path = path / 'Contents'
    if (path / 'lib').is_dir(): return path
    raise ValueError(f'No IntelliJ lib directory in {path}. Pass --ide with the .app or installation folder.')

def candidates():
    env = os.environ.get('IDEA_HOME')
    if env: yield env
    patterns = [
        '/Applications/IntelliJ IDEA*.app', '~/Applications/IntelliJ IDEA*.app',
        '~/Library/Application Support/JetBrains/Toolbox/apps/IDEA*/ch-*/*/IntelliJ IDEA*.app',
        '~/.local/share/JetBrains/Toolbox/apps/intellij-idea*',
        '~/.local/share/JetBrains/Toolbox/apps/IDEA*/ch-*/*',
        '/opt/idea*', '/opt/IntelliJ*', '/snap/intellij-idea-*/current',
    ]
    if os.name == 'nt':
        for folder in ('ProgramFiles', 'LOCALAPPDATA'):
            base = os.environ.get(folder)
            if base:
                patterns += [str(Path(base) / 'JetBrains' / 'IntelliJ IDEA*'),
                             str(Path(base) / 'Programs' / 'IntelliJ IDEA*'),
                             str(Path(base) / 'JetBrains' / 'Toolbox' / 'apps' / 'intellij-idea*')]
    for pattern in patterns:
        yield from sorted(glob.glob(os.path.expanduser(pattern)), reverse=True)

def locate_ide(explicit):
    if explicit: return ide_home(explicit)
    valid = []
    for path in candidates():
        try:
            home = ide_home(path)
            if home not in valid: valid.append(home)
        except ValueError: pass
    if not valid: raise ValueError('IntelliJ not found. Example: --ide "/Applications/IntelliJ IDEA.app"')
    return valid[0]

def product(home):
    for p in (home/'product-info.json', home/'Resources/product-info.json'):
        if p.is_file():
            info = json.loads(p.read_text(encoding='utf-8'))
            if info.get('productCode') not in ('IC','IU','IIC','IIU') and 'IntelliJ IDEA' not in info.get('name',''):
                raise ValueError(f'This builder targets IntelliJ IDEA, not {info.get("name", "this product")}')
            build = info.get('buildNumber', '').split('-')[-1]
            if not re.fullmatch(r'\d+(?:\.\d+)+', build) or int(build.split('.')[0]) < 243:
                raise ValueError('IntelliJ IDEA 2024.3 or newer (build 243+) is required.')
            return info, build
    raise ValueError('IntelliJ product-info.json not found. Select an installed IDEA distribution, not its settings folder.')

def compiler(home, explicit):
    roots = [Path(explicit).expanduser()] if explicit else []
    roots += [home/'jbr/Contents/Home', home/'jbr', home/'jre64']
    if os.environ.get('JAVA_HOME'): roots.append(Path(os.environ['JAVA_HOME']))
    suffix = '.exe' if os.name == 'nt' else ''
    attempts = []
    for base in roots:
        javac = base/'bin'/('javac'+suffix)
        java = base/'bin'/('java'+suffix)
        if javac.is_file(): attempts.append([str(javac)])
        if java.is_file(): attempts.append([str(java), '-m', 'jdk.compiler/com.sun.tools.javac.Main'])
    if not explicit and shutil.which('javac'): attempts.append([shutil.which('javac')])
    for command in attempts:
        p = subprocess.run(command+['-version'], capture_output=True, text=True, timeout=15)
        match = re.search(r'javac\s+(\d+)', p.stdout+p.stderr)
        if p.returncode == 0 and match and int(match.group(1)) >= 21: return command
    raise ValueError('No Java 21+ compiler found in IDEA/JAVA_HOME. Install a JDK 21+, or pass --jdk /path/to/jdk.')

def quoted(value):
    # javac argument files avoid command-line length limits, especially on Windows.
    return '"' + str(value).replace('\\','\\\\').replace('"','\\"') + '"'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ide', help='IDEA installation folder or macOS .app (auto-detected when omitted)')
    parser.add_argument('--jdk', help='Optional Java 21+ JDK directory')
    parser.add_argument('--output', type=Path, default=ROOT/'editors/packages', help='Destination directory for the installable ZIP')
    args = parser.parse_args()
    try:
        home = locate_ide(args.ide); info, build = product(home); javac = compiler(home, args.jdk)
        jars = sorted((home/'lib').rglob('*.jar'))
        if not jars: raise ValueError('No IntelliJ platform JARs found.')
        sources = sorted((PLUGIN/'src/main/java').rglob('*.java'))
        if not sources: raise ValueError('Plugin Java sources are missing.')
        destination = args.output.expanduser().resolve(); destination.mkdir(parents=True, exist_ok=True)
        print(f'IDE: {info.get("name")} {info.get("version")} ({build})', flush=True)
        print('Compiling against the installed platform SDK; no SDK downloads.', flush=True)
        with tempfile.TemporaryDirectory(prefix='bolt-idea-build-') as directory:
            work = Path(directory); classes = work/'classes'; classes.mkdir()
            arguments = ['--release', '21', '-encoding', 'UTF-8', '-proc:none', '-classpath', os.pathsep.join(map(str,jars)),
                         '-d', str(classes)] + list(map(str,sources))
            argfile = work/'javac.args'; argfile.write_text('\n'.join(quoted(a) for a in arguments), encoding='utf-8')
            p = subprocess.run(javac+['@'+str(argfile)], text=True)
            if p.returncode:
                raise ValueError(f'IDE plugin compilation failed (exit {p.returncode}). No new plugin ZIP was created.')
            resources = PLUGIN/'src/main/resources'
            shutil.copytree(resources, classes, dirs_exist_ok=True)
            descriptor = classes/'META-INF/plugin.xml'
            tree = ET.parse(descriptor); compatibility = tree.getroot().find('idea-version')
            # Compiling against the actual SDK is not evidence of compatibility with
            # earlier/future SDKs. Rebuild the same sources after a major IDE update.
            compatibility.set('since-build', build)
            compatibility.set('until-build', build.split('.')[0]+'.*')
            tree.write(descriptor, encoding='utf-8', xml_declaration=True)
            details = {'pluginVersion':VERSION, 'ideBuild':build, 'javaTarget':21, 'platform':sys.platform,
                       'builtAtUtc':datetime.datetime.now(datetime.timezone.utc).isoformat(),
                       'note':'Compiled against this installed SDK. Compilation is not an IDE integration test.'}
            (classes/'META-INF/bolt-build.json').write_text(json.dumps(details,indent=2)+'\n',encoding='utf-8')
            jar = work/'bolt-intellij.jar'
            with zipfile.ZipFile(jar,'w',zipfile.ZIP_DEFLATED) as z:
                for f in sorted(classes.rglob('*')):
                    if f.is_file(): z.write(f,f.relative_to(classes).as_posix())
            output = destination/f'bolt-intellij-{VERSION}.zip'
            temp = work/output.name
            with zipfile.ZipFile(temp,'w',zipfile.ZIP_DEFLATED) as z:
                z.write(jar,'bolt-intellij/lib/bolt-intellij.jar')
            with zipfile.ZipFile(temp) as z:
                if z.testzip() is not None: raise ValueError('Generated plugin ZIP is corrupt.')
            if output.is_symlink(): raise ValueError('Refusing to overwrite a symbolic-link plugin output: '+str(output))
            shutil.copy2(temp,output)
        print('\nCreated: '+str(output))
        print('In IntelliJ: Settings > Plugins > gear icon > Install Plugin from Disk...')
        print('Select this ZIP and restart IDEA. In New Project choose Bolt, enter a name and location, and click Create.')
        print('Compiler: Settings > Tools > Bolt.')
        return 0
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        print('Bolt IntelliJ builder: '+str(e), file=sys.stderr); return 1

if __name__ == '__main__': raise SystemExit(main())
