#!/bin/sh
# Entry point in an unpacked Bolt release bundle.
set -eu
error() { printf '%s\n' "$*" >&2; exit 1; }
usage() { printf '%s\n' 'Usage: ./install.sh [--prefix DIRECTORY] [--with-vscode] [--no-path] [--from-source] [--plan]'; }
base=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd)
release=0.1.0-F5
prefix= editor=0 no_path=0 plan=0 source_mode=0
while [ "$#" -gt 0 ]; do
    case $1 in
        --prefix)
            [ "$#" -ge 2 ] && [ -n "$2" ] || error '--prefix requires a directory.'
            case $2 in --*) error '--prefix requires a directory.';; esac
            prefix=$2; shift 2;;
        --with-vscode) editor=1; shift;;
        --no-path) no_path=1; shift;;
        --from-source) source_mode=1; shift;;
        --plan) plan=1; shift;;
        -h|--help) usage; exit 0;;
        *) usage >&2; error "Unknown option: $1";;
    esac
done
os=$(uname -s); machine=$(uname -m)
case "$os/$machine" in
    Linux/x86_64|Linux/amd64|Linux/aarch64|Linux/arm64|Linux/riscv64)
        case $machine in x86_64|amd64) arch=x64;; arm64|aarch64) arch=arm64;; riscv64) arch=riscv64;; esac
        libc=$(getconf GNU_LIBC_VERSION 2>/dev/null || :)
        case $libc in glibc*) :;; *)
            banner=$(ldd --version 2>&1 || :)
            case $banner in *musl*) libc=musl;; *GLIBC*|*'GNU libc'*) libc=glibc;; *) error 'Cannot identify Linux libc. Install getconf or ldd.';; esac;;
        esac
        if [ "$libc" = musl ]; then
            [ "$arch" = x64 ] || error "No bundled musl SDK for $machine."
            package=Alpine-x64; folder=Bolt-Linux-musl-x64
        else package=Linux-$arch; folder=Bolt-Linux-$arch; fi;;
    FreeBSD/amd64|FreeBSD/x86_64) package=FreeBSD-x64; folder=Bolt-FreeBSD-x64;;
    Darwin/arm64|Darwin/aarch64) package=macOS-arm64; folder=Bolt-macOS-arm64;;
    Darwin/x86_64)
        if [ "$(/usr/sbin/sysctl -in sysctl.proc_translated 2>/dev/null || :)" = 1 ]; then
            package=macOS-arm64; folder=Bolt-macOS-arm64
        else package=macOS-x64; folder=Bolt-macOS-x64; fi;;
    *) error "No bundled desktop installer for $os/$machine.";;
esac
vsix="editor/Bolt-$release-VSCode.vsix"
sdk="sdk/Bolt-$release-$package.zip"
source_zip="source/Bolt-$release-Quellcode.zip"
source_dir=
has_source() {
    for f in CMakeLists.txt install.sh tools/install-posix.py src/Main.cpp include/bolt/Bolt.hpp stdlib/System/Collections/Generic/Queue.bolt; do
        [ -f "$1/$f" ] || return 1
    done
}
if [ "$os" = Darwin ] && [ "$source_mode" = 0 ]; then
    mac_version=$(sw_vers -productVersion 2>/dev/null || :)
    major=${mac_version%%.*}
    case $major in
        ''|*[!0-9]*) :;;
        *) if [ "$major" -lt 13 ]; then
            printf '%s\n' "The prebuilt SDK requires macOS 13 or newer. Detected $mac_version; selecting the source build." >&2
            source_mode=1
        fi;;
    esac
    if [ ! -f "$base/$sdk" ]; then source_mode=1; fi
fi
if [ "$source_mode" = 1 ]; then
    for candidate in "$base/source/Bolt-Quellcode" "$base/source"; do
        if has_source "$candidate"; then source_dir=$candidate; break; fi
    done
    if [ -z "$source_dir" ] && [ ! -f "$base/$source_zip" ]; then
        error "Extract the whole Bolt.zip. Expected $base/source/Bolt-Quellcode or $base/$source_zip."
    fi
    archive=$source_zip; folder=Bolt-Quellcode
else
    [ -f "$base/$sdk" ] || error "Extract the whole Bolt.zip. Missing SDK: $base/$sdk"
    archive=$sdk
fi
if [ "$plan" = 1 ]; then
    printf '%s\n' "Host $os/$machine" "Prefix ${prefix:-user default}"
    if [ -n "$source_dir" ]; then printf '%s\n' "Build source directory $source_dir"
    elif [ "$source_mode" = 1 ]; then printf '%s\n' "Build source archive $base/$archive"
    else printf '%s\n' "Install SDK $base/$archive"; fi
    if [ "$editor" = 1 ]; then printf '%s\n' "VS Code extension $base/$vsix"; fi
    exit 0
fi
command -v python3 >/dev/null 2>&1 && python3 -c 'import sys; sys.exit(sys.version_info < (3,9))' || error 'Install Python 3.9 or newer and rerun ./install.sh.'
if [ "$editor" = 1 ]; then
    if ! command -v code >/dev/null 2>&1 && [ "$os" = Darwin ]; then
        for dir in "/Applications/Visual Studio Code.app/Contents/Resources/app/bin" "$HOME/Applications/Visual Studio Code.app/Contents/Resources/app/bin"; do
            if [ -x "$dir/code" ]; then PATH="$dir:$PATH"; export PATH; break; fi
        done
    fi
    command -v code >/dev/null 2>&1 || error 'The --with-vscode option requires the VS Code command code.'
fi
if [ "$source_mode" = 1 ]; then
    command -v cmake >/dev/null 2>&1 || error 'Source builds require CMake 3.21 or newer.'
    if [ "$os" = Darwin ]; then
        command -v xcrun >/dev/null 2>&1 || error 'Install the Apple Command Line Tools to build from source.'
        CC=$(xcrun --find clang); CXX=$(xcrun --find clang++); export CC CXX
    fi
fi
stage=$(mktemp -d "${TMPDIR:-/tmp}/bolt-install.XXXXXXXX") || error 'Cannot create a temporary directory.'
trap 'rm -rf "$stage"' EXIT
trap 'exit 1' HUP INT TERM
python3 - "$base" "$stage" "$archive" "$folder" "$source_dir" "$editor" "$vsix" <<'PY'
import hashlib, os, pathlib, shutil, stat, sys, zipfile
base, stage = map(pathlib.Path, sys.argv[1:3])
archive, root, source_dir, editor, vsix = sys.argv[3:]
required = ([] if source_dir else [archive]) + ([vsix] if editor == '1' else [])
if required:
    manifest = base / 'SHA256SUMS'
    if not manifest.is_file():
        sys.exit('The bundle checksum list is missing. Extract the entire Bolt.zip.')
    digests = {}
    for line in manifest.read_text(encoding='utf-8').splitlines():
        checksum, sep, name = line.partition('  ')
        if sep and name in required:
            if name in digests:
                sys.exit('Duplicate bundle checksum for ' + name)
            digests[name] = checksum
    for name in required:
        path = base / name
        if not path.is_file():
            sys.exit('Missing bundle file: ' + name)
        digest = hashlib.sha256()
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != digests.get(name):
            sys.exit('Bundle checksum mismatch: ' + name)
if not source_dir:
    with zipfile.ZipFile(base / archive) as z:
        for entry in z.infolist():
            name = pathlib.PurePosixPath(entry.filename)
            mode = entry.external_attr >> 16
            if not name.parts or name.parts[0] != root or '..' in name.parts or '\\' in entry.filename or stat.S_ISLNK(mode):
                sys.exit('Unexpected SDK path: ' + entry.filename)
            dest = stage.joinpath(*name.parts)
            if entry.is_dir():
                dest.mkdir(parents=True, exist_ok=True)
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(entry) as inp, dest.open('xb') as out:
                shutil.copyfileobj(inp, out)
            os.chmod(dest, 0o755 if mode & 0o111 else 0o644)
PY
set --
if [ -n "$prefix" ]; then set -- "$@" --prefix "$prefix"; fi
if [ "$no_path" = 1 ]; then set -- "$@" --no-path; fi
if [ "$source_mode" = 1 ]; then
    if [ -z "$source_dir" ]; then source_dir=$stage/$folder; fi
    has_source "$source_dir" || error "Incomplete source tree: $source_dir"
    sh "$source_dir/install.sh" "$@" --build-dir "$stage/build"
    if [ "$editor" = 1 ]; then code --install-extension "$base/$vsix" --force; fi
else
    if [ "$editor" = 0 ]; then set -- "$@" --skip-vscode
    else cmp -s "$base/$vsix" "$stage/$folder/Bolt-VSCode.vsix" || error 'The bundle and SDK contain different VS Code extensions.'; fi
    sh "$stage/$folder/setup.sh" "$@"
fi
