#!/usr/bin/env python3
"""Safely rename a project's .blt files to .bolt. Dry-run unless --apply is supplied.
No source contents/project settings are rewritten. Symbolic links and build/IDE caches are skipped.
"""
from __future__ import annotations
import argparse,os,sys
from pathlib import Path
EXCLUDED={'.git','.idea','.bolt','.vs','bin','obj','build','node_modules','__pycache__'}
def main()->int:
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('project',type=Path);p.add_argument('--apply',action='store_true');a=p.parse_args()
 root=a.project.expanduser().resolve(strict=True)
 if not root.is_dir():p.error('project must be a directory')
 pairs=[]
 for directory,folders,files in os.walk(root,followlinks=False):
  folders[:]=sorted(n for n in folders if n not in EXCLUDED and not (Path(directory)/n).is_symlink())
  for name in sorted(files):
   source=Path(directory)/name
   if source.suffix.lower()!='.blt' or source.is_symlink():continue
   target=source.with_suffix('.bolt')
   if target.exists() or target.is_symlink():raise FileExistsError(f'Refusing to overwrite {target}')
   pairs.append((source,target))
 for source,target in pairs:print(f'{source.relative_to(root)} -> {target.relative_to(root)}')
 if not a.apply:print(f'Dry run: {len(pairs)} file(s). Add --apply to rename.');return 0
 moved=[]
 try:
  for source,target in pairs:
   if target.exists() or target.is_symlink():raise FileExistsError(target)
   source.rename(target);moved.append((source,target))
 except OSError:
  for source,target in reversed(moved):
   if not source.exists():target.rename(source)
  raise
 print(f'Renamed {len(moved)} file(s). Review explicit filename imports, scripts and project entry settings separately.')
 return 0
if __name__=='__main__':
 try:raise SystemExit(main())
 except OSError as error:print(f'Migration failed: {error}',file=sys.stderr);raise SystemExit(1)
