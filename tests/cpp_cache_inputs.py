#!/usr/bin/env python3
"""Exercise real C++ preprocessor dependencies, search order and failed rebuilds."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--bolt', type=Path, required=True)
    cli.add_argument('--cxx', required=True)
    args = cli.parse_args()
    bolt = args.bolt.resolve()
    with tempfile.TemporaryDirectory(prefix='bolt cpp cache ') as tmp:
        root = Path(tmp)
        project = root / 'project'
        external = root / 'ä Grüße # dollars $'
        earlier = root / 'first include'
        later = root / 'second include'
        for path in (project, external / 'nested', earlier, later):
            path.mkdir(parents=True)
        target = root / ('test.exe' if os.name == 'nt' else 'test')
        script = project / 'Main.bolt'
        public = external / 'public.hpp'
        detail = external / 'nested' / 'value'
        indicator = external / 'optional.hpp'
        environment = {**os.environ, 'BOLT_BUILD_ROOT': str(root / 'cache')}
        command = [str(bolt), 'compile', str(script), '--backend', 'cpp', '--no-pch', '--verbose',
                   '--no-debug-info', '--cxx', args.cxx, '--header-dir', str(earlier),
                   '--header-dir', str(later), '-o', str(target)]

        def build(label, expected, *, hit=False):
            output = subprocess.run(command, cwd=project, env=environment, text=True,
                                    capture_output=True, timeout=240)
            context = f'{label}: {output.stdout}{output.stderr}'
            if expected is None:
                assert output.returncode, context
                assert 'Up to date' not in output.stdout, context
            else:
                assert output.returncode == 0, context
                # Older cl.exe drivers can omit /sourceDependencies for /P.
                # In that documented mode a complete rebuild must still be correct.
                fallback = (os.name == 'nt' and Path(args.cxx).name.lower() in ('cl', 'cl.exe')
                            and 'C++ input scan unavailable' in output.stderr)
                assert ('Up to date' in output.stdout) == (hit and not fallback), context
                actual = subprocess.check_output([str(target)], env=environment, cwd=project, text=True)
                assert actual.strip() == str(expected), (context, actual, expected)
            suffix = ' (uncached driver fallback)' if 'C++ input scan unavailable' in output.stderr else ''
            print('PASS', label + suffix, flush=True)

        def bind(header):
            script.write_text(f'[Header({json.dumps(header, ensure_ascii=False)}), Native("probe")] extern "C++" int Probe();\n'
                              'public static class Program { public: static void Main(){Print(Probe());} }\n',
                              encoding='utf-8')

        public.write_text('#include "nested/value"\n#include <choice.hpp>\n'
                          '#if __has_include("optional.hpp")\n#define OPT 1\n#else\n#define OPT 0\n#endif\n'
                          'inline int probe(){return base + choice + OPT;}\n')
        detail.write_text('constexpr int base = 30;\n')
        (later / 'choice.hpp').write_text('constexpr int choice = 1;\n')
        bind(public.as_posix())
        build('absolute external header', 31)
        build('absolute warm hit', 31, hit=True)
        stamp = detail.stat()
        detail.write_text('constexpr int base = 40;\n')
        os.utime(detail, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        build('same-size same-mtime extensionless transitive input', 41)
        indicator.touch()
        build('previously absent __has_include without an include directive', 42)
        (earlier / 'choice.hpp').write_text('constexpr int choice = 4;\n')
        build('new higher-priority header', 45)
        (earlier / 'choice.hpp').unlink()
        build('deleted higher-priority header', 42)
        detail.unlink()
        build('deleted required transitive header', None)
        assert subprocess.check_output([str(target)], cwd=project, text=True).strip() == '42'
        detail.write_text('constexpr int base = 50;\n')
        build('restored required transitive header', 52)
        bind('../' + external.name + '/public.hpp')
        build('relative external header', 52)
        build('relative warm hit', 52, hit=True)
        (earlier / 'unused.hpp').write_text('#error Unrelated native input\n')
        build('unrelated header does not trigger compilation', 52, hit=True)
        if os.name != 'nt':
            (project / 'linked').symlink_to(external, target_is_directory=True)
            bind('linked/public.hpp')
            build('symlinked include directory', 52)
            detail.write_text('constexpr int base = 60;\n')
            build('changed indirect input behind symlink', 62)
        if os.name == 'nt':
            bind(str(public))
            build('native Windows path separators and Unicode', 52)
        else:
            literal = project / 'literal\\native.hpp'
            literal.write_text('inline int probe(){return 19;}\n')
            bind(literal.name)
            build('literal backslash in a POSIX filename', 19)
        # __TIMESTAMP__ depends on the header's mtime even when every byte of
        # the file remains identical. Token concatenation exercises real macro expansion.
        stamp_header = project / 'stamp.hpp'
        stamp_header.write_text('#define CAT(a,b) a ## b\n'
                                'inline int probe(){const char* s=CAT(__TIME,STAMP__);'
                                "return (s[8]-'0')*10+(s[9]-'0');}\n")
        bind('stamp.hpp')
        environment.pop('SOURCE_DATE_EPOCH', None)
        environment['TZ'] = 'UTC'
        os.utime(stamp_header, (1768910400, 1768910400))  # 2026-01-20 12:00 UTC
        build('timestamp builtin constructed by token concatenation', 20)
        os.utime(stamp_header, (1768996800, 1768996800))  # unchanged bytes, following day
        build('changed native timestamp builtin with identical header bytes', 21)
        build('unchanged file timestamp can still hit', 21, hit=True)
        # __BASE_FILE__ names the actual generated translation unit even when
        # the normal build can consume a preprocessed snapshot.
        stamp_header.write_text('#include <string_view>\ninline int probe(){return std::string_view(__BASE_FILE__).ends_with(".bolt.cpp") ? 14 : 0;}\n')
        build('base-file macro retains the original generated filename', 14)
        if os.name != 'nt':
            # Debuggers can retain checksums for the original native sources.
            # Edit only a comment in a dependency with UTF-8 and Make metacharacters.
            bind(public.as_posix())
            index = command.index('--no-debug-info')
            command[index] = '--debug-info'
            detail.write_text('constexpr int base = 70; // first\n')
            build('debug build with native source checksums', 72)
            stamp = detail.stat()
            detail.write_text('constexpr int base = 70; // other\n')
            os.utime(detail, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
            build('native debug comment-only edit with identical mtime', 72)
            build('native debug checksum warm hit', 72, hit=True)
            command[index] = '--no-debug-info'
        # Remove explicit include roots, as in an ordinary command line.
        index = command.index('--header-dir')
        del command[index:index + 4]
        script.write_text('public static class Program {public: static void Main(){Print(73);}}')
        build('ordinary source without FFI', 73)
        build('ordinary source warm hit', 73, hit=True)


if __name__ == '__main__':
    main()
