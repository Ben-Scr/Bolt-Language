#!/usr/bin/env python3
"""Sandboxed tests of the opt-in .blt -> .bolt filename migration; no user folders touched."""
from pathlib import Path
import subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1]
def main():
 count=0
 def check(condition,label):
  nonlocal count
  assert condition,label;count+=1;print('PASS '+label,flush=True)
 with tempfile.TemporaryDirectory(prefix='bolt-migrate-') as temp:
  root=Path(temp);app=root/'project';app.mkdir();(app/'sub').mkdir();(app/'build').mkdir()
  (app/'Program.blt').write_text('import "sub/Type.blt"\n// do not rewrite content\n')
  (app/'sub/Type.BLT').write_text('class Type {}');(app/'build/cached.blt').write_text('cache')
  (app/'bolt.project').write_text('{"entry":"Program.blt"}')
  def run(*args):return subprocess.run([sys.executable,str(ROOT/'tools/migrate-extension.py'),str(app),*args],text=True,capture_output=True,timeout=10)
  original={str(p.relative_to(app)):p.read_bytes() for p in app.rglob('*') if p.is_file()}
  result=run();check(result.returncode==0 and 'Dry run: 2 file(s)' in result.stdout,'dry-run reports real source files only')
  check(original=={str(p.relative_to(app)):p.read_bytes() for p in app.rglob('*') if p.is_file()},'preview changes no file')
  result=run('--apply');check(result.returncode==0,'apply completes')
  check((app/'Program.bolt').read_bytes()==original['Program.blt'] and (app/'sub/Type.bolt').is_file(),'case-insensitive old suffix renames preserving bytes')
  check((app/'build/cached.blt').is_file() and (app/'bolt.project').read_bytes()==original['bolt.project'],'cache/config left unchanged')
  check(not (app/'Program.blt').exists(),'old source filename removed by rename, not copied')
  result=run('--apply');check(result.returncode==0 and 'Renamed 0 file(s)' in result.stdout,'second application is idempotent')
  (app/'conflict.blt').write_text('old');(app/'conflict.bolt').write_text('new');(app/'untouched.blt').write_text('stay')
  result=run('--apply');check(result.returncode!=0 and 'Refusing to overwrite' in result.stderr,'existing destination is refused')
  check((app/'conflict.bolt').read_text()=='new' and (app/'conflict.blt').read_text()=='old' and (app/'untouched.blt').is_file(),'conflict preflight preserves all sources/destinations')
  (app/'conflict.blt').unlink();(app/'conflict.bolt').unlink();(app/'untouched.blt').unlink()
  outside=root/'outside';outside.mkdir();(outside/'private.blt').write_text('outside')
  try:
   (app/'linked').symlink_to(outside,target_is_directory=True);(app/'alias.blt').symlink_to(outside/'private.blt')
  except OSError:print('SKIP symbolic-link check: host does not allow creation',flush=True)
  else:
   result=run('--apply');check(result.returncode==0 and (outside/'private.blt').read_text()=='outside' and (app/'alias.blt').is_symlink(),'links are not followed or renamed')
 print(f'{count} source migration checks passed',flush=True)
if __name__=='__main__':main()
